#include "Audio.h"

#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <roapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>

namespace {
constexpr wchar_t AppName[] = L"TrayAudio";
constexpr wchar_t AppVersion[] = L"v1.3.0";
constexpr wchar_t WindowClass[] = L"TrayAudio.Panel";
constexpr wchar_t StartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t OptionsKey[] = L"Software\\TrayAudio";
constexpr wchar_t StartupName[] = L"TrayAudio";
constexpr wchar_t PositionName[] = L"PanelPosition";
constexpr wchar_t SizeName[] = L"PanelSize";
constexpr wchar_t ThemeName[] = L"ThemeIndex";
constexpr wchar_t TransparencyName[] = L"TransparencyIndex";
constexpr UINT TrayMessage = WM_APP + 2;
constexpr UINT OpenPanel = WM_APP + 3;
constexpr UINT_PTR ViewTimer = 1;
constexpr UINT_PTR ErrorTimer = 2;
constexpr int PanelWidth = 396;
constexpr int PanelHeight = 548;
constexpr int MinPanelWidth = 320;
constexpr int MinPanelHeight = 430;
constexpr int PanelCorner = 32;

constexpr COLORREF Color(unsigned hex) {
    return RGB((hex >> 16) & 255, (hex >> 8) & 255, hex & 255);
}

struct Palette {
    const wchar_t* name;
    COLORREF background, border, card, cardBorder, hover;
    COLORREF text, secondary, track, accent, accentInk, progress;
};

constexpr Palette Palettes[]{
    {L"烟棕琥珀 · 深色", Color(0x443a29), Color(0x8a795d), Color(0x352d23), Color(0x62523c), Color(0x514437),
        Color(0xc9c8c2), Color(0xc1b8a9), Color(0x79664c), Color(0xf8a93f), Color(0x2d2115), Color(0xf8a93f)},
    {L"石板靛蓝 · 深色", Color(0x212631), Color(0x65708b), Color(0x272d3b), Color(0x414a60), Color(0x353d52),
        Color(0xf0f1f7), Color(0xc1c7d7), Color(0x566179), Color(0x9eb1ff), Color(0x232945), Color(0x9eb1ff)},
    {L"暖砂米白 · 浅色", Color(0xece7db), Color(0xa69b87), Color(0xf8f3e9), Color(0xc9c8c2), Color(0xeee3d0),
        Color(0x443a29), Color(0x615442), Color(0xcdc0a8), Color(0xf8a93f), Color(0x443a29), Color(0x9b520c)},
    {L"雾绿 · 浅色", Color(0xdce7de), Color(0x809b8a), Color(0xf4f8f4), Color(0xb9cbbf), Color(0xe2ede5),
        Color(0x1c352a), Color(0x415d50), Color(0xadbeaf), Color(0x1b7158), Color(0xf1fff9), Color(0x1b7158)},
};

constexpr int TransparencyLevels[]{0, 10, 20, 30, 40};
constexpr DWORD DefaultTransparency = 2;
constexpr UINT OpenCommand = 1, StartupCommand = 2, ExitCommand = 3, AboutCommand = 4;
constexpr UINT ThemeCommand = 10, TransparencyCommand = 20, OutputCommand = 100;

enum class Action { Mix, Voice, Back, Pin, Close, Device, Volume, Mute, Scroll };
enum class DeviceSlot { Output, Input, VoiceOutput, VoiceInput };
enum class Glyph { Speaker, Mute, Mic, Back, Pin, Close, Chevron, App };

struct Control {
    Action action;
    RECT rect;
    int index = -1;
    bool enabled = true;
};

struct DeviceChoice {
    const std::vector<DeviceInfo>& list;
    const std::wstring& current;
    EDataFlow flow;
    ERole role;
};

class PaintBitmap {
public:
    PaintBitmap(HDC reference, int width, int height) : dc(CreateCompatibleDC(reference)) {
        if (!dc) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        bitmap_ = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap_) {
            previous_ = SelectObject(dc, bitmap_);
            pixels = static_cast<DWORD*>(bits);
        }
    }

    ~PaintBitmap() {
        if (previous_) SelectObject(dc, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc) DeleteDC(dc);
    }

    PaintBitmap(const PaintBitmap&) = delete;
    PaintBitmap& operator=(const PaintBitmap&) = delete;
    explicit operator bool() const { return previous_ != nullptr; }

    const HDC dc;
    DWORD* pixels = nullptr;

private:
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

struct LabelRun {
    RECT bounds;
    RECT clip;
    std::wstring text;
    HFONT font;
    COLORREF color;
    UINT format;
};

struct DrawContext {
    HDC dc;
    std::vector<LabelRun>* labels = nullptr;
};

DWORD BlendGlyph(DWORD destination, COLORREF color, DWORD coverage) {
    const DWORD remaining = 255 - coverage;
    const auto channel = [&](DWORD before, DWORD ink) {
        return (before * remaining + ink * coverage + 127) / 255;
    };
    const DWORD alpha = channel(destination >> 24, 255);
    const DWORD red = channel((destination >> 16) & 255, GetRValue(color));
    const DWORD green = channel((destination >> 8) & 255, GetGValue(color));
    const DWORD blue = channel(destination & 255, GetBValue(color));
    return (alpha << 24) | (red << 16) | (green << 8) | blue;
}

bool CompositeLabels(PaintBitmap& surface, int stride, const std::vector<LabelRun>& labels) {
    if (labels.empty()) return true;
    LONG maskWidth = 0;
    LONG maskHeight = 0;
    for (const auto& label : labels) {
        maskWidth = std::max(maskWidth, label.clip.right - label.clip.left);
        maskHeight = std::max(maskHeight, label.clip.bottom - label.clip.top);
    }
    PaintBitmap mask(surface.dc, maskWidth, maskHeight);
    if (!mask) return false;
    SetBkMode(mask.dc, TRANSPARENT);
    SetTextColor(mask.dc, RGB(255, 255, 255));
    const HGDIOBJ defaultFont = GetCurrentObject(mask.dc, OBJ_FONT);

    for (const auto& label : labels) {
        const int width = label.clip.right - label.clip.left;
        const int height = label.clip.bottom - label.clip.top;
        PatBlt(mask.dc, 0, 0, width, height, BLACKNESS);
        SelectObject(mask.dc, label.font);
        RECT bounds = label.bounds;
        OffsetRect(&bounds, -label.clip.left, -label.clip.top);
        DrawTextW(mask.dc, label.text.c_str(), static_cast<int>(label.text.size()), &bounds, label.format);
        GdiFlush();
        for (int y = 0; y < height; ++y) {
            const DWORD* coverageRow = mask.pixels + static_cast<size_t>(y) * maskWidth;
            DWORD* destination = surface.pixels + static_cast<size_t>(label.clip.top + y) * stride +
                                 label.clip.left;
            for (int x = 0; x < width; ++x) {
                const DWORD coverage = coverageRow[x] & 255;
                if (coverage) destination[x] = BlendGlyph(destination[x], label.color, coverage);
            }
        }
    }
    SelectObject(mask.dc, defaultFont);
    return true;
}

bool StartupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, StartupKey, StartupName, RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool SetStartup(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, StartupKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LSTATUS result;
    if (enabled) {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) {
            RegCloseKey(key);
            return false;
        }
        path.resize(length);
        const std::wstring command = L"\"" + path + L"\" --tray";
        result = RegSetValueExW(key, StartupName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, StartupName);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool ConfigureStartup(bool enabled) {
    if (!SetStartup(enabled)) return false;
    const DWORD configured = 1;
    return RegSetKeyValueW(HKEY_CURRENT_USER, OptionsKey, L"StartupConfigured", REG_DWORD,
                          &configured, sizeof(configured)) == ERROR_SUCCESS;
}

bool StartupConfigured() {
    return RegGetValueW(HKEY_CURRENT_USER, OptionsKey, L"StartupConfigured", RRF_RT_REG_DWORD,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

DWORD ReadOption(PCWSTR name, DWORD count, DWORD fallback) {
    DWORD option = 0;
    DWORD size = sizeof(option);
    if (RegGetValueW(HKEY_CURRENT_USER, OptionsKey, name, RRF_RT_REG_DWORD,
                     nullptr, &option, &size) == ERROR_SUCCESS && option < count)
        return option;
    return fallback;
}

bool EnableHostBackdrop(HWND window) {
    const BOOL enabled = TRUE;
    if (SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_USE_HOSTBACKDROPBRUSH,
                                        &enabled, sizeof(enabled)))) return true;

    // Before Windows 11 the same compositor capability is exposed through an accent policy.
    struct AccentPolicy { int state; DWORD flags; DWORD color; DWORD animation; };
    struct AttributeData { int attribute; void* value; SIZE_T size; };
    using SetAttribute = BOOL (WINAPI*)(HWND, AttributeData*);
    static const auto setAttribute = reinterpret_cast<SetAttribute>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    constexpr int hostBackdropAccent = 5;
    constexpr int accentAttribute = 19;
    AccentPolicy policy{hostBackdropAccent, 0, 0, 0};
    AttributeData data{accentAttribute, &policy, sizeof(policy)};
    return setAttribute && setAttribute(window, &data) != FALSE;
}

class PanelGlass {
public:
    ~PanelGlass() {
        if (foreground_ && IsWindow(foreground_)) DestroyWindow(foreground_);
    }

    bool Create(HWND window, HINSTANCE instance) {
        using namespace winrt::Windows::UI::Composition;
        namespace abi = ABI::Windows::UI::Composition::Desktop;
        if (!EnableHostBackdrop(window)) return false;
        constexpr MARGINS extendedFrame{-1, -1, -1, -1};
        winrt::check_hresult(DwmExtendFrameIntoClientArea(window, &extendedFrame));
        constexpr DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue_))));
        Compositor compositor;
        winrt::check_hresult(compositor.as<abi::ICompositorDesktopInterop>()->CreateDesktopWindowTarget(
            window, FALSE, reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(target_))));

        auto root = compositor.CreateContainerVisual();
        clip_ = compositor.CreateRoundedRectangleGeometry();
        root.Clip(compositor.CreateGeometricClip(clip_));
        backdrop_ = compositor.CreateSpriteVisual();
        backdrop_.Brush(compositor.CreateHostBackdropBrush());
        root.Children().InsertAtBottom(backdrop_);
        tint_ = compositor.CreateSpriteVisual();
        tintBrush_ = compositor.CreateColorBrush();
        tint_.Brush(tintBrush_);
        root.Children().InsertAtTop(tint_);
        target_.Root(root);

        // The compositor draws over the parent's GDI surface. A click-through child keeps the controls above it.
        foreground_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT, L"Static", L"",
                                      WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        return foreground_ != nullptr;
    }

    void Resize(int width, int height, int corner) {
        const winrt::Windows::Foundation::Numerics::float2 size{
            static_cast<float>(width), static_cast<float>(height)};
        const float radius = corner / 2.0f;
        clip_.CornerRadius({radius, radius});
        clip_.Size(size);
        backdrop_.Size(size);
        tint_.Size(size);
    }

    void SetAppearance(COLORREF background, int transparency) {
        backdrop_.IsVisible(transparency != 0);
        tintBrush_.Color({static_cast<BYTE>(MulDiv(100 - transparency, 255, 100)),
                          GetRValue(background), GetGValue(background), GetBValue(background)});
    }

    bool Present(HDC buffer, int width, int height) const {
        SIZE size{width, height};
        POINT source{};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        return UpdateLayeredWindow(foreground_, nullptr, nullptr, &size, buffer, &source, 0,
                                   &blend, ULW_ALPHA) != FALSE;
    }

private:
    HWND foreground_ = nullptr;
    winrt::Windows::System::DispatcherQueueController queue_{nullptr};
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target_{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry clip_{nullptr};
    winrt::Windows::UI::Composition::CompositionColorBrush tintBrush_{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual backdrop_{nullptr}, tint_{nullptr};
};

class Panel {
public:
    explicit Panel(HINSTANCE instance) : instance_(instance) {}
    ~Panel() {
        ClearIcons();
        DeleteFonts();
        if (fontResource_) RemoveFontMemResourceEx(fontResource_);
    }

    bool Create() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = Proc;
        windowClass.hInstance = instance_;
        windowClass.style = CS_DROPSHADOW;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        windowClass.lpszClassName = WindowClass;
        if (!RegisterClassW(&windowClass)) return false;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, AppName, WS_POPUP | WS_THICKFRAME,
                                  0, 0, PanelWidth, PanelHeight, nullptr, nullptr, instance_, this);
        if (!window_) return false;

        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
        wcscpy_s(tray_.szTip, AppName);
        AddTrayIcon();
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        dpi_ = GetDpiForWindow(window_);
        UpdateShape();
        POINT position{};
        DWORD size = sizeof(position);
        if (RegGetValueW(HKEY_CURRENT_USER, OptionsKey, PositionName, RRF_RT_REG_BINARY,
                         nullptr, &position, &size) == ERROR_SUCCESS && size == sizeof(position))
            savedPosition_ = position;
        SIZE dimensions{};
        size = sizeof(dimensions);
        if (RegGetValueW(HKEY_CURRENT_USER, OptionsKey, SizeName, RRF_RT_REG_BINARY,
                         nullptr, &dimensions, &size) == ERROR_SUCCESS && size == sizeof(dimensions) &&
            dimensions.cx >= MinPanelWidth && dimensions.cy >= MinPanelHeight)
            savedSize_ = dimensions;
        theme_ = ReadOption(ThemeName, ARRAYSIZE(Palettes), 0);
        transparency_ = ReadOption(TransparencyName, ARRAYSIZE(TransparencyLevels), DefaultTransparency);
        audio_ = std::make_unique<AudioThread>(window_);
        return true;
    }

    void FirstLaunch() {
        Show();
        if ((StartupEnabled() || !StartupConfigured()) && !ConfigureStartup(true))
            Error(L"无法设置开机自启");
    }

private:
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            panel->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
        }
        auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!panel) return DefWindowProcW(window, message, wParam, lParam);
        return panel->Message(message, wParam, lParam);
    }

    LRESULT Message(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == taskbarCreated_) AddTrayIcon();
        switch (message) {
        case TrayMessage:
            if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) {
                Show();
            } else if (LOWORD(lParam) == WM_CONTEXTMENU) {
                POINT anchor{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
                if (anchor.x == -1 && anchor.y == -1) GetCursorPos(&anchor);
                TrayMenu(anchor);
            }
            return 0;
        case OpenPanel:
            Show();
            return 0;
        case AudioUpdated:
            if (IsWindowVisible(window_) && !about_ && !Interacting()) Refresh();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            Paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCCALCSIZE:
            // Keep the frameless client area while letting Windows run its resize loop.
            return 0;
        case WM_GETMINMAXINFO: {
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT windowDpi = GetDpiForWindow(window_);
            const UINT dpi = windowDpi ? windowDpi : dpi_;
            limits.ptMinTrackSize = {MulDiv(MinPanelWidth, dpi, 96), MulDiv(MinPanelHeight, dpi, 96)};
            return 0;
        }
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window_, &point);
            if (point.x >= 0 && point.x < width_ && point.y >= 0 && point.y < height_)
                return FrameHit(point);
            break;
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCAPTION) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        case WM_ENTERSIZEMOVE: {
            RECT rect{};
            if (GetWindowRect(window_, &rect)) dragOrigin_ = rect;
            break;
        }
        case WM_EXITSIZEMOVE:
            FinishGeometryChange();
            break;
        case WM_TIMER:
            if (wParam == ViewTimer && !about_ && !Interacting()) {
                if (voice_) {
                    const float peak = audio_->InputPeak();
                    if (std::abs(peak - view_.peak) > 0.003f) {
                        view_.peak = peak;
                        RECT meter{Px(22), Px(194), width_ - Px(22), Px(217)};
                        InvalidateRect(window_, &meter, FALSE);
                    }
                } else {
                    Refresh();
                }
            }
            if (wParam == ErrorTimer) {
                KillTimer(window_, ErrorTimer);
                error_.clear();
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_SIZE:
            Layout();
            UpdateShape();
            return 0;
        case WM_DPICHANGED: {
            SetFonts(HIWORD(wParam));
            const auto& rect = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect.left, rect.top, rect.right - rect.left,
                         rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER);
            Layout();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !pinned_ && !Interacting())
                Hide();
            return 0;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (dragging_) SliderTo(point.x);
            else if (scrollDragging_) ScrollTo(point.y);
            else HoverAt(point);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hovered_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            if (!about_ && !voice_ && !Interacting()) {
                wheel_ += GET_WHEEL_DELTA_WPARAM(wParam);
                ScrollBy(-wheel_ / WHEEL_DELTA * Px(54));
                wheel_ %= WHEEL_DELTA;
            }
            return 0;
        case WM_LBUTTONDOWN:
            Press({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_LBUTTONUP:
            Release({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_ || scrollDragging_) {
                dragging_ = false;
                scrollDragging_ = false;
                Refresh();
                if (GetForegroundWindow() != window_ && !pinned_) Hide();
            }
            pressed_ = -1;
            return 0;
        case WM_KEYDOWN:
            Key(wParam);
            return 0;
        case WM_CLOSE:
            Hide();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &tray_);
            glass_.reset();
            audio_.reset();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    void AddTrayIcon() {
        if (Shell_NotifyIconW(NIM_ADD, &tray_)) {
            tray_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &tray_);
        }
    }

    LRESULT FrameHit(POINT point) const {
        const int horizontal = std::min<int>(point.x, width_ - point.x - 1);
        const int vertical = std::min<int>(point.y, height_ - point.y - 1);
        const int edge = Px(7);
        const int corner = Px(24);
        const bool left = point.x < width_ / 2;
        const bool top = point.y < height_ / 2;
        if (!left && !top && horizontal < Px(22) && vertical < Px(22)) return HTBOTTOMRIGHT;
        if ((horizontal < edge && vertical < corner) || (vertical < edge && horizontal < corner)) {
            if (top) return left ? HTTOPLEFT : HTTOPRIGHT;
            return left ? HTBOTTOMLEFT : HTBOTTOMRIGHT;
        }
        if (horizontal < edge) return left ? HTLEFT : HTRIGHT;
        if (vertical < edge) return top ? HTTOP : HTBOTTOM;
        if (point.y < Px(49) && Hit(point) < 0) return HTCAPTION;
        return HTCLIENT;
    }

    void UpdateShape() {
        if (width_ <= 0 || height_ <= 0) return;
        const int corner = Px(PanelCorner);
        HRGN region = CreateRoundRectRgn(0, 0, width_, height_, corner, corner);
        if (region && !SetWindowRgn(window_, region, TRUE)) DeleteObject(region);
        if (glass_) {
            try {
                glass_->Resize(width_, height_, corner);
            } catch (const winrt::hresult_error& error) {
                OutputDebugStringW(error.message().c_str());
                glass_.reset();
            }
        }
    }

    void EnableGlass() {
        if (glassAttempted_) return;
        glassAttempted_ = true;
        try {
            auto candidate = std::make_unique<PanelGlass>();
            if (candidate->Create(window_, instance_)) {
                candidate->Resize(width_, height_, Px(PanelCorner));
                candidate->SetAppearance(Colors().background, TransparencyLevels[transparency_]);
                glass_ = std::move(candidate);
            }
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW(error.message().c_str());
        }
    }

    void FinishGeometryChange() {
        if (!dragOrigin_) return;
        const RECT origin = *dragOrigin_;
        dragOrigin_.reset();
        RECT rect{};
        if (!GetWindowRect(window_, &rect) || EqualRect(&rect, &origin)) return;
        savedPosition_ = POINT{rect.left, rect.top};
        savedSize_ = {std::max(MinPanelWidth, MulDiv(rect.right - rect.left, 96, dpi_)),
                      std::max(MinPanelHeight, MulDiv(rect.bottom - rect.top, 96, dpi_))};
        const LSTATUS position = RegSetKeyValueW(HKEY_CURRENT_USER, OptionsKey, PositionName, REG_BINARY,
                                                &*savedPosition_, sizeof(POINT));
        const LSTATUS size = RegSetKeyValueW(HKEY_CURRENT_USER, OptionsKey, SizeName, REG_BINARY,
                                            &savedSize_, sizeof(savedSize_));
        if (position != ERROR_SUCCESS || size != ERROR_SUCCESS) Error(L"无法保存窗口位置和大小");
    }

    void Show() {
        if (!audio_) return;
        if (IsWindowVisible(window_)) {
            SetForegroundWindow(window_);
            return;
        }
        NOTIFYICONIDENTIFIER icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = 1;
        RECT anchor{};
        if (FAILED(Shell_NotifyIconGetRect(&icon, &anchor))) {
            POINT point;
            GetCursorPos(&point);
            anchor = {point.x, point.y, point.x + 1, point.y + 1};
        }
        const HMONITOR monitor = savedPosition_
            ? MonitorFromPoint(*savedPosition_, MONITOR_DEFAULTTONEAREST)
            : MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);

        // Move onto the chosen monitor before sizing in its logical pixels.
        SetWindowPos(window_, nullptr, info.rcWork.left + 1, info.rcWork.top + 1,
                     0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
        SetFonts(GetDpiForWindow(window_));
        const int gap = savedPosition_ ? 0 : Px(8);
        const int width = std::min<int>(Px(savedSize_.cx), info.rcWork.right - info.rcWork.left - 2 * gap);
        const int height = std::min<int>(Px(savedSize_.cy), info.rcWork.bottom - info.rcWork.top - 2 * gap);
        const int x = std::clamp<int>(savedPosition_ ? savedPosition_->x : anchor.right - width,
                                      info.rcWork.left + gap, info.rcWork.right - width - gap);
        const int trayY = anchor.bottom <= info.rcWork.top ? info.rcWork.top + gap
                                                         : info.rcWork.bottom - height - gap;
        const int y = std::clamp<int>(savedPosition_ ? savedPosition_->y : trayY,
                                      info.rcWork.top + gap, info.rcWork.bottom - height - gap);
        SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        EnableGlass();
        Refresh();
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        StartTimer();
    }

    void Hide() {
        if (!IsWindowVisible(window_)) return;
        KillTimer(window_, ViewTimer);
        ShowWindow(window_, SW_HIDE);
        if (GetCapture() == window_) ReleaseCapture();
        hovered_ = focused_ = -1;
        keyFocus_ = false;
        about_ = false;
        ClearIcons();
        view_ = {};
        controls_.clear();
    }

    void StartTimer() {
        KillTimer(window_, ViewTimer);
        if (!about_) SetTimer(window_, ViewTimer, voice_ ? 100 : 450, nullptr);
    }

    void NavigateAbout(bool open) {
        about_ = open;
        hovered_ = focused_ = -1;
        keyFocus_ = false;
        if (!IsWindowVisible(window_)) {
            Show();
            return;
        }
        Refresh();
        StartTimer();
    }

    void Refresh() {
        const std::wstring focusKey = FocusKey();
        Action focusAction = Action::Close;
        int focusIndex = -1;
        if (focused_ >= 0 && focused_ < static_cast<int>(controls_.size())) {
            focusAction = controls_[focused_].action;
            focusIndex = controls_[focused_].index;
        }
        view_ = about_ ? AudioView{} : audio_->Read(!voice_);
        for (auto cached = icons_.begin(); cached != icons_.end();) {
            const bool displayed = std::ranges::any_of(view_.apps, [&](const MixerApp& app) {
                return app.path == cached->first;
            });
            if (displayed) ++cached;
            else {
                if (cached->second) DestroyIcon(cached->second);
                cached = icons_.erase(cached);
            }
        }
        Layout();
        if (!focusKey.empty()) {
            focused_ = -1;
            const auto app = std::ranges::find(view_.apps, focusKey, &MixerApp::key);
            if (app != view_.apps.end()) {
                focusIndex = static_cast<int>(app - view_.apps.begin());
                for (int index = 0; index < static_cast<int>(controls_.size()); ++index)
                    if (controls_[index].action == focusAction && controls_[index].index == focusIndex)
                        focused_ = index;
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    std::wstring FocusKey() const {
        if (focused_ < 0 || focused_ >= static_cast<int>(controls_.size())) return {};
        const auto& control = controls_[focused_];
        if (IsAppControl(control) && control.index < static_cast<int>(view_.apps.size()))
            return view_.apps[control.index].key;
        return {};
    }

    void Error(const std::wstring& message) {
        error_ = message;
        SetTimer(window_, ErrorTimer, 4500, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }

    int Px(int value) const { return MulDiv(value, dpi_, 96); }
    RECT Box(int left, int top, int right, int bottom) const {
        return {Px(left), Px(top), Px(right), Px(bottom)};
    }

    void SetFonts(UINT dpi) {
        if (dpi_ == dpi && normal_) return;
        if (!fontAttempted_) {
            fontAttempted_ = true;
            HRSRC font = FindResourceW(instance_, MAKEINTRESOURCEW(2), RT_RCDATA);
            HGLOBAL resource = font ? LoadResource(instance_, font) : nullptr;
            if (resource) {
                DWORD faces = 0;
                fontResource_ = AddFontMemResourceEx(LockResource(resource), SizeofResource(instance_, font),
                                                      nullptr, &faces);
            }
        }
        DeleteFonts();
        ClearIcons();
        dpi_ = dpi ? dpi : 96;
        normal_ = Font(14, FW_NORMAL);
        medium_ = Font(14, FW_SEMIBOLD);
        small_ = Font(12, FW_NORMAL);
        title_ = Font(18, FW_NORMAL);
        aboutName_ = Font(27, FW_NORMAL);
    }

    HFONT Font(int size, int weight) const {
        return CreateFontW(-Px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, fontResource_ ? L"MiSans" : L"Microsoft YaHei UI");
    }

    void DeleteFonts() {
        for (HFONT font : {normal_, medium_, small_, title_, aboutName_})
            if (font) DeleteObject(font);
    }

    void ClearIcons() {
        for (const auto& icon : icons_)
            if (icon.second) DestroyIcon(icon.second);
        icons_.clear();
    }

    RECT AppViewport() const {
        return {Px(20), Px(266), width_ - Px(13), std::max(Px(266), height_ - Px(38))};
    }

    int ScrollLimit() const {
        const RECT list = AppViewport();
        return std::max(0, Px(61) * static_cast<int>(view_.apps.size()) - static_cast<int>(list.bottom - list.top));
    }

    void Layout() {
        RECT client{};
        GetClientRect(window_, &client);
        width_ = client.right;
        height_ = client.bottom;
        const int edge = width_ - Px(22);
        controls_.clear();
        if (about_) controls_.push_back({Action::Back, Box(22, 12, 54, 44)});
        controls_.push_back({Action::Pin, {width_ - Px(85), Px(12), width_ - Px(53), Px(44)}});
        controls_.push_back({Action::Close, {width_ - Px(48), Px(12), width_ - Px(16), Px(44)}});
        if (about_) return;
        controls_.push_back({Action::Mix, Box(25, 58, 96, 89)});
        controls_.push_back({Action::Voice, Box(99, 58, 170, 89)});
        if (voice_) {
            controls_.push_back({Action::Device, {Px(22), Px(143), edge, Px(182)},
                                 static_cast<int>(DeviceSlot::Input), !view_.inputs.empty()});
            controls_.push_back({Action::Device, {Px(22), Px(264), edge, Px(303)},
                                 static_cast<int>(DeviceSlot::VoiceOutput), !view_.outputs.empty()});
            controls_.push_back({Action::Device, {Px(22), Px(355), edge, Px(394)},
                                 static_cast<int>(DeviceSlot::VoiceInput), !view_.inputs.empty()});
        } else {
            controls_.push_back({Action::Device, {Px(22), Px(143), edge, Px(182)},
                                 static_cast<int>(DeviceSlot::Output), !view_.outputs.empty()});
            controls_.push_back({Action::Mute, Box(22, 193, 52, 223), -1, view_.volume >= 0});
            controls_.push_back({Action::Volume, {Px(61), Px(193), width_ - Px(70), Px(223)},
                                 -1, view_.volume >= 0});
            const RECT viewport = AppViewport();
            scroll_ = std::clamp(scroll_, 0, ScrollLimit());
            for (int index = 0; index < static_cast<int>(view_.apps.size()); ++index) {
                const int y = viewport.top - scroll_ + index * Px(61);
                controls_.push_back({Action::Mute, {Px(65), y + Px(24), Px(89), y + Px(52)}, index});
                controls_.push_back({Action::Volume, {Px(98), y + Px(24), width_ - Px(70), y + Px(52)}, index});
            }
            if (ScrollLimit() > 0)
                controls_.push_back({Action::Scroll, {width_ - Px(17), viewport.top,
                                                      width_ - Px(7), viewport.bottom}});
        }
        if (focused_ >= static_cast<int>(controls_.size())) focused_ = -1;
        if (hovered_ >= static_cast<int>(controls_.size())) hovered_ = -1;
    }

    static bool IsAppControl(const Control& control) {
        return control.index >= 0 && (control.action == Action::Volume || control.action == Action::Mute);
    }

    bool Interacting() const {
        return dragOrigin_ || dragging_ || scrollDragging_ || pressed_ >= 0 || menuOpen_;
    }

    int Hit(POINT point) const {
        const RECT list = AppViewport();
        for (int index = 0; index < static_cast<int>(controls_.size()); ++index) {
            const auto& control = controls_[index];
            if (IsAppControl(control) && !PtInRect(&list, point)) continue;
            if (control.enabled && PtInRect(&control.rect, point)) return index;
        }
        return -1;
    }

    void HoverAt(POINT point) {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        const int next = Hit(point);
        if (hovered_ != next) {
            hovered_ = next;
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void Press(POINT point) {
        const int index = Hit(point);
        if (index < 0) return;
        keyFocus_ = false;
        focused_ = index;
        const Control control = controls_[index];
        if (control.action == Action::Volume) {
            dragging_ = true;
            dragControl_ = control;
            SetCapture(window_);
            SliderTo(point.x);
        } else if (control.action == Action::Scroll) {
            scrollDragging_ = true;
            SetCapture(window_);
            ScrollTo(point.y);
        } else {
            pressed_ = index;
            SetCapture(window_);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    void Release(POINT point) {
        if (dragging_ || scrollDragging_) {
            ReleaseCapture();
            return;
        }
        const int index = pressed_;
        pressed_ = -1;
        if (GetCapture() == window_) ReleaseCapture();
        if (index >= 0 && index == Hit(point)) Activate(controls_[index]);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ScrollBy(int delta) {
        const int next = std::clamp(scroll_ + delta, 0, ScrollLimit());
        if (next == scroll_) return;
        scroll_ = next;
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void ScrollTo(int y) {
        const RECT list = AppViewport();
        const int range = std::max(1L, list.bottom - list.top);
        const int next = MulDiv(std::clamp<int>(y - list.top, 0, range), ScrollLimit(), range);
        ScrollBy(next - scroll_);
    }

    void SliderTo(int x) {
        const RECT rect = dragControl_.rect;
        const int level = std::clamp(MulDiv(x - rect.left - Px(6), 100,
                                          rect.right - rect.left - Px(12)), 0, 100);
        SetVolume(dragControl_, level);
    }

    void SetVolume(const Control& control, int level) {
        int& value = control.index < 0 ? view_.volume : view_.apps[control.index].volume;
        if (value == level) return;
        const HRESULT hr = control.index < 0 ? audio_->SetMasterVolume(level)
            : audio_->SetAppVolume(view_.apps[control.index].key, level);
        if (FAILED(hr)) Error(L"无法调整音量，请检查音频设备");
        else value = level;
        InvalidateRect(window_, nullptr, FALSE);
    }

    void Activate(const Control& control) {
        switch (control.action) {
        case Action::Mix:
        case Action::Voice:
            voice_ = control.action == Action::Voice;
            Refresh();
            StartTimer();
            break;
        case Action::Pin:
            pinned_ = !pinned_;
            break;
        case Action::Back:
            NavigateAbout(false);
            break;
        case Action::Close:
            Hide();
            break;
        case Action::Device:
            DeviceMenu(static_cast<DeviceSlot>(control.index), control.rect);
            break;
        case Action::Mute: {
            bool& muted = control.index < 0 ? view_.muted : view_.apps[control.index].muted;
            const HRESULT hr = control.index < 0 ? audio_->SetMasterMute(!muted)
                : audio_->SetAppMute(view_.apps[control.index].key, !muted);
            if (FAILED(hr)) Error(L"无法切换静音");
            else muted = !muted;
            break;
        }
        default:
            break;
        }
    }

    void Key(WPARAM key) {
        if (key == VK_ESCAPE) {
            if (about_) NavigateAbout(false);
            else Hide();
            return;
        }
        if (key == VK_TAB && !controls_.empty()) {
            keyFocus_ = true;
            const int direction = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            const int count = static_cast<int>(controls_.size());
            int next = focused_ < 0 ? (direction > 0 ? count - 1 : 0) : focused_;
            do {
                next = (next + direction + count) % count;
            } while (!controls_[next].enabled || controls_[next].action == Action::Scroll);
            focused_ = next;
            if (IsAppControl(controls_[next])) {
                const RECT rect = controls_[next].rect;
                const RECT list = AppViewport();
                if (rect.top < list.top) ScrollBy(rect.top - list.top);
                else if (rect.bottom > list.bottom) ScrollBy(rect.bottom - list.bottom);
            }
        } else if (focused_ >= 0 && focused_ < static_cast<int>(controls_.size())) {
            const Control control = controls_[focused_];
            if (!control.enabled) return;
            if (control.action == Action::Volume) {
                int level = control.index < 0 ? view_.volume : view_.apps[control.index].volume;
                if (key == VK_LEFT || key == VK_DOWN) level -= 2;
                if (key == VK_RIGHT || key == VK_UP) level += 2;
                if (key == VK_HOME) level = 0;
                if (key == VK_END) level = 100;
                SetVolume(control, std::clamp(level, 0, 100));
            } else if (key == VK_RETURN || key == VK_SPACE ||
                       (key == VK_DOWN && control.action == Action::Device)) {
                Activate(control);
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    DeviceChoice Choice(const AudioView& view, DeviceSlot slot) const {
        switch (slot) {
        case DeviceSlot::Input:
            return {view.inputs, view.input, eCapture, eMultimedia};
        case DeviceSlot::VoiceOutput:
            return {view.outputs, view.communicationOutput, eRender, eCommunications};
        case DeviceSlot::VoiceInput:
            return {view.inputs, view.communicationInput, eCapture, eCommunications};
        default:
            return {view.outputs, view.output, eRender, eMultimedia};
        }
    }

    UINT MenuStyle(UINT id, const std::wstring& name, bool current, HMENU menu) {
        AppendMenuW(menu, MF_STRING | (current ? MF_CHECKED : MF_UNCHECKED), id, name.c_str());
        return id + 1;
    }

    UINT AddDevices(HMENU menu, const DeviceChoice& choice, UINT id) {
        for (const auto& device : choice.list) {
            std::wstring name = device.name;
            size_t pos = 0;
            while ((pos = name.find(L'&', pos)) != std::wstring::npos) {
                name.insert(pos, 1, L'&');
                pos += 2;
            }
            id = MenuStyle(id, name, device.id == choice.current, menu);
        }
        if (choice.list.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"无可用设备");
        return id;
    }

    void Switch(const DeviceChoice& choice, size_t index) {
        if (index >= choice.list.size() || choice.list[index].id == choice.current) return;
        if (FAILED(audio_->SetDevice(choice.flow, choice.role, choice.list[index].id))) {
            if (IsWindowVisible(window_)) Error(L"切换失败，请检查设备连接");
            else MessageBoxW(window_, L"切换失败，请检查设备连接。", AppName, MB_OK | MB_ICONWARNING);
        }
    }

    void DeviceMenu(DeviceSlot slot, RECT rect) {
        const auto choice = Choice(view_, slot);
        HMENU menu = CreatePopupMenu();
        AddDevices(menu, choice, 1);
        POINT anchor{rect.left, rect.bottom};
        ClientToScreen(window_, &anchor);
        menuOpen_ = true;
        const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                             anchor.x, anchor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (selected) Switch(choice, selected - 1);
        menuOpen_ = false;
        if (IsWindowVisible(window_)) Refresh();
    }

    void TrayMenu(POINT anchor) {
        if (!audio_) return;
        const AudioView view = audio_->Read(false);
        const auto output = Choice(view, DeviceSlot::Output);
        const auto input = Choice(view, DeviceSlot::Input);
        HMENU menu = CreatePopupMenu();
        HMENU outputs = CreatePopupMenu();
        HMENU inputs = CreatePopupMenu();
        HMENU themes = CreatePopupMenu();
        HMENU transparency = CreatePopupMenu();
        const UINT inputBase = AddDevices(outputs, output, OutputCommand);
        AddDevices(inputs, input, inputBase);
        for (UINT index = 0; index < ARRAYSIZE(Palettes); ++index)
            AppendMenuW(themes, MF_STRING, ThemeCommand + index, Palettes[index].name);
        CheckMenuRadioItem(themes, ThemeCommand, ThemeCommand + ARRAYSIZE(Palettes) - 1,
                           ThemeCommand + theme_, MF_BYCOMMAND);
        for (UINT index = 0; index < ARRAYSIZE(TransparencyLevels); ++index) {
            const auto label = std::to_wstring(TransparencyLevels[index]) + L"%";
            AppendMenuW(transparency, MF_STRING, TransparencyCommand + index, label.c_str());
        }
        CheckMenuRadioItem(transparency, TransparencyCommand, TransparencyCommand + ARRAYSIZE(TransparencyLevels) - 1,
                           TransparencyCommand + transparency_, MF_BYCOMMAND);
        AppendMenuW(menu, MF_STRING, OpenCommand, L"打开音量面板");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(outputs), L"切换默认输出");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(inputs), L"切换默认输入");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(transparency), L"透明度");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themes), L"配色");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const bool startup = StartupEnabled();
        MenuStyle(StartupCommand, L"开机自启", startup, menu);
        AppendMenuW(menu, MF_STRING, AboutCommand, L"关于");
        AppendMenuW(menu, MF_STRING, ExitCommand, L"退出");
        menuOpen_ = true;
        SetForegroundWindow(window_);
        const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                             anchor.x, anchor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        menuOpen_ = false;
        if (selected >= inputBase) Switch(input, selected - inputBase);
        else if (selected >= OutputCommand) Switch(output, selected - OutputCommand);
        else if (selected >= TransparencyCommand && selected < TransparencyCommand + ARRAYSIZE(TransparencyLevels))
            ChangeAppearance(TransparencyName, transparency_, selected - TransparencyCommand);
        else if (selected >= ThemeCommand && selected < ThemeCommand + ARRAYSIZE(Palettes))
            ChangeAppearance(ThemeName, theme_, selected - ThemeCommand);
        if (selected == StartupCommand && !ConfigureStartup(!startup))
            MessageBoxW(window_, L"无法更改开机自启。", AppName, MB_OK | MB_ICONWARNING);
        if (selected == AboutCommand) NavigateAbout(true);
        if (selected == OpenCommand) Show();
        if (selected == ExitCommand) DestroyWindow(window_);
        else if (IsWindowVisible(window_) && !about_) Refresh();
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    const Palette& Colors() const { return Palettes[theme_]; }

    void ChangeAppearance(PCWSTR option, DWORD& value, DWORD selected) {
        value = selected;
        if (glass_) {
            try {
                glass_->SetAppearance(Colors().background, TransparencyLevels[transparency_]);
            } catch (const winrt::hresult_error& error) {
                OutputDebugStringW(error.message().c_str());
                glass_.reset();
            }
        }
        Show();
        if (RegSetKeyValueW(HKEY_CURRENT_USER, OptionsKey, option, REG_DWORD, &value,
                            sizeof(value)) != ERROR_SUCCESS)
            Error(L"无法保存外观设置");
    }

    void Fill(HDC dc, const RECT& rect, COLORREF color) const {
        SetDCBrushColor(dc, color);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void Round(HDC dc, const RECT& rect, int radius, COLORREF color, COLORREF stroke = CLR_INVALID) const {
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(stroke == CLR_INVALID ? NULL_PEN : DC_PEN));
        SetDCBrushColor(dc, color);
        if (stroke != CLR_INVALID) SetDCPenColor(dc, stroke);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Px(radius), Px(radius));
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }

    void Circle(HDC dc, const RECT& rect, COLORREF color) const {
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        SetDCBrushColor(dc, color);
        Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
    }

    void Label(const DrawContext& context, RECT rect, const std::wstring& value, HFONT font,
               COLORREF color = CLR_INVALID, UINT align = DT_LEFT) const {
        const HDC dc = context.dc;
        const COLORREF ink = color == CLR_INVALID ? Colors().text : color;
        const UINT format = align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
        if (context.labels) {
            RECT visible{}, clip{};
            if (GetClipBox(dc, &clip) != ERROR && IntersectRect(&visible, &rect, &clip))
                context.labels->push_back({rect, visible, value, font, ink, format});
            return;
        }
        HGDIOBJ previous = SelectObject(dc, font);
        SetTextColor(dc, ink);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, format);
        SelectObject(dc, previous);
    }

    void Symbol(HDC dc, int centerX, int centerY, Glyph glyph, COLORREF color, int size = 20) const {
        const int width = Px(size);
        const int x = centerX - width / 2;
        const int y = centerY - width / 2;
        auto point = [&](int a, int b) -> POINT {
            return {x + MulDiv(a, width, 20), y + MulDiv(b, width, 20)};
        };
        auto line = [&](std::initializer_list<POINT> vertices) {
            Polyline(dc, vertices.begin(), static_cast<int>(vertices.size()));
        };
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(1)), color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        switch (glyph) {
        case Glyph::Speaker:
        case Glyph::Mute:
            line({point(2, 8), point(5, 8), point(9, 4), point(9, 16),
                  point(5, 12), point(2, 12), point(2, 8)});
            if (glyph == Glyph::Mute) {
                line({point(12, 7), point(18, 13)});
                line({point(18, 7), point(12, 13)});
            } else {
                line({point(12, 6), point(14, 8), point(14, 12), point(12, 14)});
                line({point(15, 3), point(18, 7), point(18, 13), point(15, 17)});
            }
            break;
        case Glyph::Mic: {
            const POINT left = point(7, 2), right = point(14, 13);
            RoundRect(dc, left.x, left.y, right.x, right.y, Px(6), Px(6));
            line({point(4, 9), point(4, 12), point(7, 16), point(13, 16), point(16, 12), point(16, 9)});
            line({point(10, 16), point(10, 19)});
            line({point(7, 19), point(14, 19)});
            break;
        }
        case Glyph::Back:
            line({point(10, 4), point(4, 10), point(10, 16)});
            line({point(4, 10), point(17, 10)});
            break;
        case Glyph::Pin:
            line({point(11, 3), point(17, 9), point(15, 10), point(12, 10),
                  point(10, 15), point(5, 10), point(10, 8), point(10, 5), point(11, 3)});
            line({point(8, 12), point(3, 17)});
            break;
        case Glyph::Close:
            line({point(4, 4), point(16, 16)});
            line({point(16, 4), point(4, 16)});
            break;
        case Glyph::Chevron:
            line({point(4, 8), point(10, 14), point(16, 8)});
            break;
        case Glyph::App: {
            const POINT left = point(3, 4), right = point(18, 16);
            RoundRect(dc, left.x, left.y, right.x, right.y, Px(4), Px(4));
            line({point(3, 8), point(18, 8)});
            break;
        }
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void Slider(HDC dc, RECT rect, int level, bool muted, bool hovered) const {
        const auto& colors = Colors();
        const int middle = (rect.top + rect.bottom) / 2;
        const int x0 = rect.left + Px(6);
        const int x1 = rect.right - Px(6);
        const int thumb = x0 + MulDiv(std::max(0, level), x1 - x0, 100);
        const int height = std::max(2, Px(4));
        const COLORREF color = muted ? colors.secondary : colors.progress;
        Round(dc, {x0, middle - height / 2, x1, middle - height / 2 + height}, 4, colors.track);
        if (level < 0) return;
        if (thumb > x0)
            Round(dc, {x0, middle - height / 2, thumb + 1, middle - height / 2 + height}, 4, color);
        const int radius = Px(hovered ? 9 : 7);
        Circle(dc, {thumb - radius, middle - radius, thumb + radius + 1, middle + radius + 1}, colors.card);
        const int dot = Px(hovered ? 6 : 5);
        Circle(dc, {thumb - dot, middle - dot, thumb + dot + 1, middle + dot + 1}, color);
    }

    HICON AppIcon(const std::wstring& path) {
        if (path.empty()) return nullptr;
        if (auto item = icons_.find(path); item != icons_.end()) return item->second;
        HICON icon = nullptr;
        ExtractIconExW(path.c_str(), 0, &icon, nullptr, 1);
        icons_.emplace(path, icon);
        return icon;
    }

    void Paint(HDC target) {
        if (width_ <= 0 || height_ <= 0) return;
        PaintBitmap surface(target, width_, height_);
        if (!surface) {
            glass_.reset();
            Draw({target});
            return;
        }
        std::vector<LabelRun> labels;
        Draw({surface.dc, glass_ ? &labels : nullptr});
        if (!glass_) {
            BitBlt(target, 0, 0, width_, height_, surface.dc, 0, 0, SRCCOPY);
            return;
        }

        // GDI shapes are opaque. Blend the separate grayscale text coverage after setting their alpha.
        GdiFlush();
        const size_t count = static_cast<size_t>(width_) * height_;
        for (size_t index = 0; index < count; ++index) {
            const DWORD rgb = surface.pixels[index] & 0x00ffffff;
            surface.pixels[index] = rgb ? (0xff000000 | rgb) : 0;
        }
        PatBlt(target, 0, 0, width_, height_, BLACKNESS);
        if (!CompositeLabels(surface, width_, labels) || !glass_->Present(surface.dc, width_, height_)) {
            OutputDebugStringW(L"TrayAudio: unable to update the glass panel foreground.");
            glass_.reset();
            Draw({target});
        }
    }

    void Draw(const DrawContext& context) {
        const HDC dc = context.dc;
        const auto& colors = Colors();
        SetBkMode(dc, TRANSPARENT);
        const RECT bounds{0, 0, width_, height_};
        const COLORREF outside = glass_ ? RGB(0, 0, 0) : colors.background;
        Fill(dc, bounds, outside);
        Round(dc, bounds, PanelCorner, outside, colors.border);
        if (!about_) {
            Round(dc, Box(22, 13, 52, 43), 10, colors.accent);
            Symbol(dc, Px(37), Px(28), Glyph::Speaker, colors.accentInk, 19);
        }
        Label(context, Box(62, 13, 210, 43), about_ ? L"关于" : AppName, title_);
        for (const int dx : {0, 4}) {
            for (const int y : {24, 28, 32}) {
                const int x = width_ - Px(102 - dx);
                Circle(dc, {x, Px(y), x + Px(2), Px(y) + Px(2)}, colors.secondary);
            }
        }
        if (!about_) Round(dc, Box(22, 55, 173, 92), 15, colors.card, colors.cardBorder);
        if (about_) {
            DrawAbout(context);
        } else if (voice_) {
            Label(context, Box(22, 112, 370, 137), L"Windows 默认输入", medium_);
            Label(context, Box(22, 233, 370, 258), L"默认通信输出", medium_);
            Label(context, Box(22, 324, 370, 349), L"默认通信输入", medium_);
            Symbol(dc, Px(33), Px(205), Glyph::Mic, colors.secondary, 18);
            const int left = Px(55), right = width_ - Px(22);
            Round(dc, {left, Px(203), right, Px(208)}, 5, colors.track);
            const int end = left + static_cast<int>((right - left) * std::clamp(view_.peak, 0.0f, 1.0f));
            if (end > left) Round(dc, {left, Px(203), end, Px(208)}, 5, colors.progress);
        } else {
            Label(context, Box(22, 112, 370, 137), L"Windows 默认输出", medium_);
            Fill(dc, {Px(22), Px(229), width_ - Px(22), Px(229) + 1}, colors.cardBorder);
            Label(context, Box(22, 237, 370, 261), L"应用音量", medium_);
            DrawApps(context);
        }
        for (int index = 0; index < static_cast<int>(controls_.size()); ++index) {
            const auto& control = controls_[index];
            if (!IsAppControl(control)) DrawControl(context, index);
        }
        if (!error_.empty())
            Label(context, {Px(22), height_ - Px(26), width_ - Px(20), height_ - Px(5)},
                  error_, small_, colors.text);
        for (int index = 0; index < 3; ++index) {
            const int x = width_ - Px(19 - index * 3);
            const int y = height_ - Px(13 + index * 3);
            Circle(dc, {x, y, x + Px(2), y + Px(2)}, colors.secondary);
        }
    }

    void DrawAbout(const DrawContext& context) {
        const auto& colors = Colors();
        const int top = std::max(Px(64), (height_ - Px(219)) / 2);
        const int center = width_ / 2;
        const int left = Px(22);
        const int right = width_ - left;
        Round(context.dc, {left, top, right, top + Px(219)}, 20, colors.card, colors.cardBorder);
        Round(context.dc, {center - Px(24), top + Px(23), center + Px(24), top + Px(71)}, 15, colors.accent);
        Symbol(context.dc, center, top + Px(47), Glyph::Speaker, colors.accentInk, 27);
        Label(context, {left, top + Px(78), right, top + Px(115)}, AppName, aboutName_, colors.text, DT_CENTER);
        Label(context, {left, top + Px(117), right, top + Px(139)}, AppVersion, small_, colors.secondary, DT_CENTER);
        Label(context, {left, top + Px(142), right, top + Px(164)},
              L"Windows 托盘音频控制", normal_, colors.text, DT_CENTER);
        Fill(context.dc, {left + Px(24), top + Px(178), right - Px(24), top + Px(178) + 1}, colors.cardBorder);
        Label(context, {left, top + Px(185), right, top + Px(210)},
              L"界面字体 · 小米 MiSans", small_, colors.secondary, DT_CENTER);
    }

    void DrawApps(const DrawContext& context) {
        const HDC dc = context.dc;
        const auto& colors = Colors();
        const RECT list = AppViewport();
        const int previous = SaveDC(dc);
        IntersectClipRect(dc, list.left, list.top, list.right, list.bottom);
        if (view_.apps.empty()) {
            Label(context, {Px(22), list.top + Px(8), width_ - Px(22), list.top + Px(40)},
                  L"播放声音后，应用会显示在这里", normal_, colors.secondary);
        }
        for (int index = 0; index < static_cast<int>(view_.apps.size()); ++index) {
            const int y = list.top - scroll_ + index * Px(61);
            if (y + Px(60) < list.top || y >= list.bottom) continue;
            const auto& app = view_.apps[index];
            Round(dc, {Px(22), y + Px(1), width_ - Px(22), y + Px(57)}, 14, colors.card, colors.cardBorder);
            Round(dc, {Px(29), y + Px(12), Px(57), y + Px(40)}, 9, colors.hover);
            if (HICON icon = AppIcon(app.path))
                DrawIconEx(dc, Px(32), y + Px(15), icon, Px(22), Px(22), 0, nullptr, DI_NORMAL);
            else
                Symbol(dc, Px(43), y + Px(26), app.system ? Glyph::Speaker : Glyph::App, colors.text, 18);
            Label(context, {Px(66), y + Px(2), width_ - Px(32), y + Px(27)}, app.name, normal_);
        }
        for (int index = 0; index < static_cast<int>(controls_.size()); ++index) {
            const auto& control = controls_[index];
            if (IsAppControl(control) && control.rect.bottom >= list.top && control.rect.top < list.bottom)
                DrawControl(context, index);
        }
        RestoreDC(dc, previous);
    }

    void DrawControl(const DrawContext& context, int index) {
        const HDC dc = context.dc;
        const auto& colors = Colors();
        const auto& control = controls_[index];
        const auto& rect = control.rect;
        const bool hover = index == hovered_ || index == pressed_;
        const bool focus = index == focused_ && keyFocus_;
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        switch (control.action) {
        case Action::Back:
            if (hover || focus) Round(dc, rect, 8, colors.hover, focus ? colors.progress : CLR_INVALID);
            Symbol(dc, cx, cy, Glyph::Back, colors.text, 19);
            break;
        case Action::Pin:
        case Action::Close: {
            const bool pin = control.action == Action::Pin;
            if (hover || focus || (pin && pinned_))
                Round(dc, rect, 8, colors.hover, focus ? colors.progress : CLR_INVALID);
            Symbol(dc, cx, cy, pin ? Glyph::Pin : Glyph::Close,
                   pin && pinned_ ? colors.progress : colors.secondary, 19);
            break;
        }
        case Action::Mix:
        case Action::Voice: {
            const bool mix = control.action == Action::Mix;
            const bool active = voice_ != mix;
            if (active || hover || focus)
                Round(dc, rect, 11, active ? colors.accent : colors.hover, focus ? colors.text : CLR_INVALID);
            Label(context, rect, mix ? L"混合" : L"语音", medium_,
                  active ? colors.accentInk : (hover ? colors.text : colors.secondary), DT_CENTER);
            break;
        }
        case Action::Device: {
            const auto choice = Choice(view_, static_cast<DeviceSlot>(control.index));
            const auto item = std::ranges::find(choice.list, choice.current, &DeviceInfo::id);
            const std::wstring name = item != choice.list.end() ? item->name :
                (choice.list.empty() ? L"无可用设备" : L"选择设备");
            Round(dc, rect, 13, hover ? colors.hover : colors.card, focus ? colors.progress : colors.border);
            Label(context, {rect.left + Px(12), rect.top, rect.right - Px(35), rect.bottom},
                  name, normal_, control.enabled ? colors.text : colors.secondary);
            Symbol(dc, rect.right - Px(18), cy, Glyph::Chevron, colors.secondary, 18);
            break;
        }
        case Action::Mute: {
            const bool muted = control.index < 0 ? view_.muted : view_.apps[control.index].muted;
            if (hover || focus) Round(dc, rect, 7, colors.hover, focus ? colors.progress : CLR_INVALID);
            Symbol(dc, cx, cy, muted ? Glyph::Mute : Glyph::Speaker,
                   muted ? colors.secondary : colors.text, control.index < 0 ? 20 : 17);
            break;
        }
        case Action::Volume: {
            const int level = control.index < 0 ? view_.volume : view_.apps[control.index].volume;
            const bool muted = control.index < 0 ? view_.muted : view_.apps[control.index].muted;
            if (focus) Round(dc, rect, 7, colors.card, colors.progress);
            Slider(dc, rect, level, muted, hover || focus);
            const int right = width_ - Px(IsAppControl(control) ? 30 : 24);
            Label(context, {rect.right + Px(3), rect.top, right, rect.bottom},
                  level < 0 ? L"—" : std::to_wstring(level) + L"%", small_, colors.secondary, DT_RIGHT);
            break;
        }
        case Action::Scroll: {
            const int length = rect.bottom - rect.top;
            const int range = ScrollLimit();
            const int thumb = std::min(length, std::max(Px(18), MulDiv(length, length, length + range)));
            const int y = rect.top + MulDiv(scroll_, length - thumb, range);
            Round(dc, {rect.left + Px(3), y, rect.right - Px(3), y + thumb}, 4,
                  hover ? colors.secondary : colors.track);
            break;
        }
        }
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_ = 0;
    std::unique_ptr<AudioThread> audio_;
    std::unique_ptr<PanelGlass> glass_;
    AudioView view_;
    std::vector<Control> controls_;
    std::map<std::wstring, HICON> icons_;
    std::wstring error_;
    std::optional<POINT> savedPosition_;
    std::optional<RECT> dragOrigin_;
    SIZE savedSize_{PanelWidth, PanelHeight};
    DWORD theme_ = 0, transparency_ = DefaultTransparency;
    UINT dpi_ = 96;
    HANDLE fontResource_ = nullptr;
    HFONT normal_ = nullptr, medium_ = nullptr, small_ = nullptr, title_ = nullptr, aboutName_ = nullptr;
    int width_ = 0, height_ = 0, scroll_ = 0, wheel_ = 0;
    int hovered_ = -1, focused_ = -1, pressed_ = -1;
    bool voice_ = false, about_ = false, pinned_ = false, menuOpen_ = false, glassAttempted_ = false;
    bool fontAttempted_ = false;
    bool tracking_ = false, dragging_ = false, scrollDragging_ = false, keyFocus_ = false;
    Control dragControl_{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    const bool trayOnly = wcscmp(command, L"--tray") == 0;
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\TrayAudio.SingleInstance");
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!trayOnly) {
            if (HWND existing = FindWindowW(WindowClass, nullptr)) {
                DWORD process = 0;
                GetWindowThreadProcessId(existing, &process);
                AllowSetForegroundWindow(process);
                PostMessageW(existing, OpenPanel, 0, 0);
            }
        }
        CloseHandle(singleInstance);
        return 0;
    }

    const HRESULT com = RoInitialize(RO_INIT_SINGLETHREADED);
    int result = 1;
    {
        Panel panel(instance);
        if (panel.Create()) {
            if (!trayOnly) panel.FirstLaunch();
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            result = 0;
        } else {
            MessageBoxW(nullptr, L"无法启动托盘音量。", AppName, MB_OK | MB_ICONERROR);
        }
    }
    if (SUCCEEDED(com)) RoUninitialize();
    CloseHandle(singleInstance);
    return result;
}
