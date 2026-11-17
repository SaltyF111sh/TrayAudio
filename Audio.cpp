#include "Audio.h"

#include <appmodel.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT CallAudio = WM_APP + 20;
constexpr UINT DevicesChanged = WM_APP + 21;
constexpr UINT SessionsChanged = WM_APP + 22;
constexpr UINT_PTR SaveTimer = 1;
constexpr UINT_PTR RetryTimer = 2;
constexpr wchar_t VolumeKey[] = L"Software\\TrayAudio\\AppVolumes";

// The default-endpoint setter is absent from the public MMDevice API.
// These declarations retain the slots preceding SetDefaultEndpoint.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) PolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
};
const CLSID PolicyClient{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

struct TaskString {
    LPWSTR value = nullptr;
    ~TaskString() { CoTaskMemFree(value); }
    std::wstring String() const { return value ? value : L""; }
};

template<class Interface>
HRESULT Activate(IMMDevice* device, ComPtr<Interface>& result) {
    return device->Activate(__uuidof(Interface), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(result.ReleaseAndGetAddressOf()));
}

template<class Interface>
class Callback : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(Interface)) return E_NOINTERFACE;
        *result = static_cast<Interface*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --refs_;
        if (!count) delete this;
        return count;
    }
protected:
    virtual ~Callback() = default;
private:
    std::atomic<ULONG> refs_{1};
};

struct NewSession {
    std::wstring endpoint;
    ComPtr<IAudioSessionControl> control;
};

class Notices {
public:
    explicit Notices(HWND window) : window_(window) {}

    void Post(UINT message) {
        std::lock_guard lock(mutex_);
        if (window_) PostMessageW(window_, message, 0, 0);
    }

    void Add(const std::wstring& endpoint, IAudioSessionControl* control) {
        std::lock_guard lock(mutex_);
        if (!window_) return;
        if (pending_.empty()) PostMessageW(window_, SessionsChanged, 0, 0);
        pending_.push_back({endpoint, ComPtr<IAudioSessionControl>(control)});
    }

    std::vector<NewSession> Take() {
        std::lock_guard lock(mutex_);
        std::vector<NewSession> pending;
        pending.swap(pending_);
        return pending;
    }

    void Close() {
        std::vector<NewSession> pending;
        {
            std::lock_guard lock(mutex_);
            window_ = nullptr;
            pending.swap(pending_);
        }
    }

private:
    HWND window_;
    std::mutex mutex_;
    std::vector<NewSession> pending_;
};

class DeviceEvents final : public Callback<IMMNotificationClient> {
public:
    explicit DeviceEvents(std::shared_ptr<Notices> notices) : notices_(std::move(notices)) {}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Changed(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Changed(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Changed(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Changed(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        if (key == PKEY_Device_FriendlyName) return Changed();
        return S_OK;
    }
private:
    HRESULT Changed() { notices_->Post(DevicesChanged); return S_OK; }
    std::shared_ptr<Notices> notices_;
};

class SessionCreated final : public Callback<IAudioSessionNotification> {
public:
    SessionCreated(std::shared_ptr<Notices> notices, std::wstring endpoint)
        : notices_(std::move(notices)), endpoint_(std::move(endpoint)) {}
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* control) override {
        notices_->Add(endpoint_, control);
        return S_OK;
    }
private:
    std::shared_ptr<Notices> notices_;
    std::wstring endpoint_;
};

class SessionEvents final : public Callback<IAudioSessionEvents> {
public:
    explicit SessionEvents(std::shared_ptr<Notices> notices) : notices_(std::move(notices)) {}
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return Changed(); }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override { return Changed(); }
private:
    HRESULT Changed() { notices_->Post(SessionsChanged); return S_OK; }
    std::shared_ptr<Notices> notices_;
};

struct Volume {
    int level;
    bool muted;
};

int Percent(float value) {
    return std::clamp(static_cast<int>(std::lround(value * 100)), 0, 100);
}

std::wstring Lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring FileDescription(const std::wstring& path) {
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size || size > 512 * 1024) return {};
    std::vector<BYTE> version(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, version.data())) return {};
    struct Translation { WORD language; WORD codePage; };
    void* table = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", &table, &bytes)) return {};
    const auto* translations = static_cast<const Translation*>(table);
    for (UINT index = 0; index < bytes / sizeof(Translation); ++index) {
        wchar_t query[64];
        swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   translations[index].language, translations[index].codePage);
        void* description = nullptr;
        UINT length = 0;
        if (VerQueryValueW(version.data(), query, &description, &length) && length > 1)
            return static_cast<const wchar_t*>(description);
    }
    return {};
}

MixerApp Describe(IAudioSessionControl2* control, const std::wstring& instance) {
    MixerApp app{};
    if (control->IsSystemSoundsSession() == S_OK) {
        app.key = L"system";
        app.name = L"系统声音";
        app.system = true;
        return app;
    }

    DWORD pid = 0;
    if (SUCCEEDED(control->GetProcessId(&pid)) && pid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            wchar_t identity[256];
            UINT32 length = ARRAYSIZE(identity);
            if (GetApplicationUserModelId(process, &length, identity) == ERROR_SUCCESS)
                app.key = L"aumid:" + Lower(identity);

            std::wstring path(32768, L'\0');
            DWORD chars = static_cast<DWORD>(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &chars)) {
                path.resize(chars);
                if (app.key.empty()) app.key = L"exe:" + Lower(path);
                app.path = std::move(path);
            }
            CloseHandle(process);
        }
    }

    TaskString displayName;
    if (SUCCEEDED(control->GetDisplayName(&displayName.value)) && displayName.value) {
        if (displayName.value[0] == L'@') {
            wchar_t translated[256];
            if (SUCCEEDED(SHLoadIndirectString(displayName.value, translated, ARRAYSIZE(translated), nullptr)))
                app.name = translated;
        } else {
            app.name = displayName.value;
        }
    }
    if (app.name.empty() && !app.path.empty()) {
        app.name = FileDescription(app.path);
        if (app.name.empty()) {
            app.name = app.path.substr(app.path.find_last_of(L"\\/") + 1);
            if (app.name.ends_with(L".exe")) app.name.resize(app.name.size() - 4);
        }
    }
    if (app.key.empty()) {
        TaskString id;
        if (SUCCEEDED(control->GetSessionIdentifier(&id.value)) && id.value && *id.value)
            app.key = L"session:" + Lower(id.value);
        else
            app.key = L"instance:" + instance;
    }
    if (app.name.empty()) app.name = pid ? L"应用 " + std::to_wstring(pid) : L"音频会话";
    return app;
}
}

struct AudioThread::Engine {
    struct Endpoint {
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionNotification> created;
        ~Endpoint() {
            if (created) manager->UnregisterSessionNotification(created.Get());
        }
    };

    struct Session {
        std::wstring endpoint;
        MixerApp app;
        ComPtr<IAudioSessionControl2> control;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioSessionEvents> events;
        ~Session() {
            if (events) control->UnregisterAudioSessionNotification(events.Get());
        }
    };

    HWND window = nullptr;
    HWND panel;
    std::shared_ptr<Notices> notices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMNotificationClient> deviceEvents;
    ComPtr<IAudioSessionEvents> sessionEvents;
    ComPtr<IAudioEndpointVolume> master;
    ComPtr<IAudioMeterInformation> microphone;
    std::map<std::wstring, Endpoint> endpoints;
    std::map<std::wstring, std::unique_ptr<Session>> sessions;
    std::map<std::wstring, Volume> pending;
    AudioView devices;

    explicit Engine(HWND panel) : panel(panel) {}

    void Connect() {
        if (!notices) {
            notices = std::make_shared<Notices>(window);
            sessionEvents.Attach(new SessionEvents(notices));
        }
        if (!enumerator) {
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(enumerator.GetAddressOf()));
        }
        if (enumerator && !deviceEvents) {
            ComPtr<IMMNotificationClient> events;
            events.Attach(new DeviceEvents(notices));
            if (SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(events.Get())))
                deviceEvents = std::move(events);
        }
        RefreshDevices();
    }

    ~Engine() {
        if (notices) notices->Close();
        sessions.clear();
        endpoints.clear();
        if (deviceEvents) enumerator->UnregisterEndpointNotificationCallback(deviceEvents.Get());
        Flush();
    }

    void RefreshDevices() {
        if (!enumerator) {
            SetTimer(window, RetryTimer, 5000, nullptr);
            return;
        }

        std::vector<DeviceInfo> outputs;
        std::vector<DeviceInfo> inputs;
        bool retry = !deviceEvents;
        if (FAILED(List(eRender, outputs)) || FAILED(List(eCapture, inputs))) {
            SetTimer(window, RetryTimer, 5000, nullptr);
            return;
        }
        devices.outputs = std::move(outputs);
        devices.inputs = std::move(inputs);
        std::erase_if(endpoints, [&](const auto& entry) {
            return std::ranges::none_of(devices.outputs, [&](const DeviceInfo& info) {
                return info.id == entry.first;
            });
        });
        Prune();
        for (const auto& info : devices.outputs)
            if (!endpoints.contains(info.id) && !WatchEndpoint(info.id)) retry = true;
        ReadDefaults();
        if (retry) SetTimer(window, RetryTimer, 5000, nullptr);
        else KillTimer(window, RetryTimer);
        PostMessageW(panel, AudioUpdated, 0, 0);
    }

    HRESULT List(EDataFlow flow, std::vector<DeviceInfo>& result) {
        ComPtr<IMMDeviceCollection> collection;
        HRESULT hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) return hr;
        UINT count = 0;
        if (FAILED(hr = collection->GetCount(&count))) return hr;
        for (UINT index = 0; index < count; ++index) {
            ComPtr<IMMDevice> device;
            TaskString id;
            if (FAILED(collection->Item(index, &device)) || FAILED(device->GetId(&id.value))) continue;
            std::wstring name;
            ComPtr<IPropertyStore> properties;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                PROPVARIANT value{};
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
                    name = value.pwszVal;
                PropVariantClear(&value);
            }
            result.push_back({id.String(), name.empty() ? L"音频设备 " + std::to_wstring(index + 1) : name});
        }
        std::ranges::sort(result, {}, &DeviceInfo::name);
        return S_OK;
    }

    std::wstring Default(EDataFlow flow, ERole role, ComPtr<IMMDevice>* endpoint = nullptr) {
        if (!enumerator) return {};
        ComPtr<IMMDevice> device;
        TaskString id;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device)) ||
            FAILED(device->GetId(&id.value))) return {};
        if (endpoint) *endpoint = std::move(device);
        return id.String();
    }

    void ReadDefaults() {
        ComPtr<IMMDevice> output;
        ComPtr<IMMDevice> input;
        const auto outputId = Default(eRender, eMultimedia, &output);
        const auto inputId = Default(eCapture, eMultimedia, &input);
        if (outputId != devices.output || !master) {
            master.Reset();
            if (output) Activate(output.Get(), master);
        }
        if (inputId != devices.input || !microphone) {
            microphone.Reset();
            if (input) Activate(input.Get(), microphone);
        }
        devices.output = outputId;
        devices.input = inputId;
        devices.communicationOutput = Default(eRender, eCommunications);
        devices.communicationInput = Default(eCapture, eCommunications);
    }

    bool WatchEndpoint(const std::wstring& id) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(enumerator->GetDevice(id.c_str(), &device)) ||
            FAILED(Activate(device.Get(), manager))) return false;

        ComPtr<IAudioSessionNotification> created;
        created.Attach(new SessionCreated(notices, id));
        if (FAILED(manager->RegisterSessionNotification(created.Get()))) return false;
        auto& endpoint = endpoints.try_emplace(id).first->second;
        endpoint.manager = std::move(manager);
        endpoint.created = std::move(created);
        ComPtr<IAudioSessionEnumerator> list;
        int count = 0;
        if (FAILED(endpoint.manager->GetSessionEnumerator(&list)) || FAILED(list->GetCount(&count))) {
            endpoints.erase(id);
            return false;
        }
        for (int index = 0; index < count; ++index) {
            ComPtr<IAudioSessionControl> control;
            if (SUCCEEDED(list->GetSession(index, &control))) AddSession(id, control.Get());
        }
        return true;
    }

    Volume ReadVolume(ISimpleAudioVolume* volume) {
        float level = 1;
        BOOL muted = FALSE;
        volume->GetMasterVolume(&level);
        volume->GetMute(&muted);
        return {Percent(level), muted != FALSE};
    }

    bool Remembered(const std::wstring& key, Volume& volume) {
        if (const auto item = pending.find(key); item != pending.end()) {
            volume = item->second;
            return true;
        }
        DWORD raw = 0;
        DWORD size = sizeof(raw);
        if (RegGetValueW(HKEY_CURRENT_USER, VolumeKey, key.c_str(), RRF_RT_REG_DWORD,
                         nullptr, &raw, &size) != ERROR_SUCCESS || (raw & 0xff) > 100) return false;
        volume = {static_cast<int>(raw & 0xff), (raw & 0x100) != 0};
        return true;
    }

    void AddSession(const std::wstring& endpoint, IAudioSessionControl* control) {
        auto session = std::make_unique<Session>();
        if (FAILED(control->QueryInterface(IID_PPV_ARGS(&session->control))) ||
            FAILED(control->QueryInterface(IID_PPV_ARGS(&session->volume)))) return;
        TaskString instance;
        AudioSessionState state;
        if (FAILED(session->control->GetSessionInstanceIdentifier(&instance.value)) ||
            FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) return;
        const std::wstring id = endpoint + L"|" + instance.String();
        if (sessions.contains(id)) return;
        session->endpoint = endpoint;
        session->app = Describe(session->control.Get(), id);
        Volume saved;
        if (Remembered(session->app.key, saved)) {
            const Volume current = ReadVolume(session->volume.Get());
            if (saved.level != current.level)
                session->volume->SetMasterVolume(saved.level / 100.0f, nullptr);
            if (saved.muted != current.muted) session->volume->SetMute(saved.muted, nullptr);
        }
        if (SUCCEEDED(control->RegisterAudioSessionNotification(sessionEvents.Get())))
            session->events = sessionEvents;
        sessions.emplace(id, std::move(session));
    }

    void Prune() {
        std::erase_if(sessions, [&](const auto& entry) {
            AudioSessionState state;
            const auto& session = *entry.second;
            return !endpoints.contains(session.endpoint) ||
                   FAILED(session.control->GetState(&state)) || state == AudioSessionStateExpired;
        });
    }

    void RefreshSessions() {
        Prune();
        for (auto& item : notices->Take())
            if (endpoints.contains(item.endpoint)) AddSession(item.endpoint, item.control.Get());
        PostMessageW(panel, AudioUpdated, 0, 0);
    }

    float Peak() {
        float peak = 0;
        if (microphone) microphone->GetPeakValue(&peak);
        return peak;
    }

    AudioView View(bool mixer) {
        AudioView view = devices;
        if (!mixer) {
            view.peak = Peak();
            return view;
        }
        if (master) {
            float level;
            BOOL muted;
            if (SUCCEEDED(master->GetMasterVolumeLevelScalar(&level))) view.volume = Percent(level);
            if (SUCCEEDED(master->GetMute(&muted))) view.muted = muted != FALSE;
        }
        Prune();
        struct Group {
            MixerApp app;
            int priority;
        };
        std::map<std::wstring, Group> groups;
        for (const auto& item : sessions) {
            const Session& session = *item.second;
            AudioSessionState state;
            if (FAILED(session.control->GetState(&state))) continue;
            // Prefer what is currently playing, using the default endpoint to break ties.
            int priority = (state == AudioSessionStateActive ? 2 : 0) +
                           (session.endpoint == devices.output ? 1 : 0);
            auto group = groups.find(session.app.key);
            if (group != groups.end() && group->second.priority >= priority) continue;
            MixerApp app = session.app;
            const Volume current = ReadVolume(session.volume.Get());
            app.volume = current.level;
            app.muted = current.muted;
            groups.insert_or_assign(session.app.key, Group{std::move(app), priority});
        }
        for (auto& item : groups) view.apps.push_back(std::move(item.second.app));
        std::ranges::sort(view.apps, [](const MixerApp& a, const MixerApp& b) {
            if (a.system != b.system) return a.system;
            const int order = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
            return order == CSTR_EQUAL ? a.key < b.key : order == CSTR_LESS_THAN;
        });
        return view;
    }

    HRESULT SetDevice(EDataFlow flow, ERole role, const std::wstring& id) {
        const auto& list = flow == eRender ? devices.outputs : devices.inputs;
        if (std::ranges::none_of(list, [&](const DeviceInfo& info) { return info.id == id; }))
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        ComPtr<PolicyConfig> policy;
        HRESULT hr = CoCreateInstance(PolicyClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy));
        if (FAILED(hr)) return hr;
        if (role == eCommunications) {
            hr = policy->SetDefaultEndpoint(id.c_str(), role);
        } else {
            const std::wstring oldConsole = Default(flow, eConsole);
            hr = policy->SetDefaultEndpoint(id.c_str(), eConsole);
            if (SUCCEEDED(hr)) {
                hr = policy->SetDefaultEndpoint(id.c_str(), eMultimedia);
                if (FAILED(hr) && !oldConsole.empty())
                    policy->SetDefaultEndpoint(oldConsole.c_str(), eConsole);
            }
        }
        ReadDefaults();
        PostMessageW(panel, AudioUpdated, 0, 0);
        return hr;
    }

    HRESULT SetApp(const std::wstring& key, int level, int mute) {
        HRESULT result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        Volume selected{};
        bool changed = false;
        for (const auto& item : sessions) {
            Session& session = *item.second;
            if (session.app.key != key) continue;
            const HRESULT hr = level >= 0
                ? session.volume->SetMasterVolume(level / 100.0f, nullptr)
                : session.volume->SetMute(mute != 0, nullptr);
            if (SUCCEEDED(hr) && !changed) {
                selected = ReadVolume(session.volume.Get());
                changed = true;
            }
            if (FAILED(hr)) result = hr;
        }
        if (!changed) return result;
        Volume remembered = selected;
        Remembered(key, remembered);
        if (level >= 0) remembered.level = level;
        else remembered.muted = mute != 0;
        if (!key.starts_with(L"instance:")) {
            pending[key] = remembered;
            SetTimer(window, SaveTimer, 400, nullptr);
        }
        return S_OK;
    }

    void Flush() {
        if (pending.empty()) return;
        HKEY storage = nullptr;
        LSTATUS result = RegCreateKeyExW(HKEY_CURRENT_USER, VolumeKey, 0, nullptr, 0,
                                         KEY_SET_VALUE, nullptr, &storage, nullptr);
        if (result == ERROR_SUCCESS) {
            std::erase_if(pending, [&](const auto& item) {
                const DWORD value = item.second.level | (item.second.muted ? 0x100 : 0);
                result = RegSetValueExW(storage, item.first.c_str(), 0, REG_DWORD,
                                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
                return result == ERROR_SUCCESS;
            });
            RegCloseKey(storage);
        }
        if (!pending.empty() && IsWindow(window)) SetTimer(window, SaveTimer, 5000, nullptr);
    }

    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* engine = static_cast<Engine*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            engine->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(engine));
        }
        auto* engine = reinterpret_cast<Engine*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!engine) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case CallAudio:
            (*reinterpret_cast<const std::function<void(Engine&)>*>(lParam))(*engine);
            return 0;
        case DevicesChanged:
            engine->RefreshDevices();
            return 0;
        case SessionsChanged:
            engine->RefreshSessions();
            return 0;
        case WM_TIMER:
            if (wParam == SaveTimer) {
                KillTimer(window, SaveTimer);
                engine->Flush();
            }
            if (wParam == RetryTimer) engine->Connect();
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
};

AudioThread::AudioThread(HWND panel) {
    std::promise<HWND> ready;
    auto result = ready.get_future();
    thread_ = std::thread([panel, ready = std::move(ready)]() mutable {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            ready.set_value(nullptr);
            return;
        }
        {
            Engine engine(panel);
            const HINSTANCE instance = GetModuleHandleW(nullptr);
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = Engine::Proc;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = L"TrayAudio.Worker";
            RegisterClassW(&windowClass);
            HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", 0,
                                          0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &engine);
            if (window) engine.Connect();
            ready.set_value(window);
            if (window) {
                MSG message;
                while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
            }
        }
        CoUninitialize();
    });
    window_ = result.get();
}

AudioThread::~AudioThread() {
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
}

void AudioThread::Call(const std::function<void(Engine&)>& action) {
    if (window_) SendMessageW(window_, CallAudio, 0, reinterpret_cast<LPARAM>(&action));
}

AudioView AudioThread::Read(bool mixer) {
    AudioView view;
    Call([&](Engine& engine) { view = engine.View(mixer); });
    return view;
}

float AudioThread::InputPeak() {
    float peak = 0;
    Call([&](Engine& engine) { peak = engine.Peak(); });
    return peak;
}

HRESULT AudioThread::SetDevice(EDataFlow flow, ERole role, const std::wstring& id) {
    HRESULT hr = E_FAIL;
    Call([&](Engine& engine) { hr = engine.SetDevice(flow, role, id); });
    return hr;
}

HRESULT AudioThread::SetMasterVolume(int level) {
    HRESULT hr = E_FAIL;
    Call([&](Engine& engine) {
        if (engine.master) hr = engine.master->SetMasterVolumeLevelScalar(level / 100.0f, nullptr);
    });
    return hr;
}

HRESULT AudioThread::SetMasterMute(bool muted) {
    HRESULT hr = E_FAIL;
    Call([&](Engine& engine) {
        if (engine.master) hr = engine.master->SetMute(muted, nullptr);
    });
    return hr;
}

HRESULT AudioThread::SetAppVolume(const std::wstring& key, int level) {
    HRESULT hr = E_FAIL;
    Call([&](Engine& engine) { hr = engine.SetApp(key, level, -1); });
    return hr;
}

HRESULT AudioThread::SetAppMute(const std::wstring& key, bool muted) {
    HRESULT hr = E_FAIL;
    Call([&](Engine& engine) { hr = engine.SetApp(key, -1, muted); });
    return hr;
}
