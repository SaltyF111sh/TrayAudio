#pragma once

#include <windows.h>
#include <mmdeviceapi.h>

#include <functional>
#include <string>
#include <thread>
#include <vector>

constexpr UINT AudioUpdated = WM_APP + 1;
constexpr UINT MicMonitorUpdated = WM_APP + 4;

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct MixerApp {
    std::wstring key;
    std::wstring name;
    std::wstring path;
    int volume;
    bool muted;
    bool system;
};

struct AudioView {
    std::vector<DeviceInfo> outputs;
    std::vector<DeviceInfo> inputs;
    std::wstring output;
    std::wstring input;
    std::wstring communicationOutput;
    std::wstring communicationInput;
    std::vector<MixerApp> apps;
    int volume = -1;
    bool muted = false;
    float peak = 0;
};

class AudioThread {
public:
    explicit AudioThread(HWND panel);
    ~AudioThread();
    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    AudioView Read(bool mixer);
    float InputPeak();
    HRESULT SetDevice(EDataFlow flow, ERole role, const std::wstring& id);
    HRESULT SetMasterVolume(int volume);
    HRESULT SetMasterMute(bool muted);
    HRESULT SetAppVolume(const std::wstring& key, int volume);
    HRESULT SetAppMute(const std::wstring& key, bool muted);
    explicit operator bool() const { return window_ != nullptr; }

private:
    struct Engine;
    void Call(const std::function<void(Engine&)>& action);
    HWND window_ = nullptr;
    std::thread thread_;
};

enum class MicMonitorState { Off, Starting, Playing };

class MicMonitor {
public:
    MicMonitor() = default;
    ~MicMonitor();
    MicMonitor(const MicMonitor&) = delete;
    MicMonitor& operator=(const MicMonitor&) = delete;

    HRESULT Start(HWND panel, const std::wstring& input, const std::wstring& output);
    void Stop();
    bool Update(WPARAM generation, HRESULT result);
    MicMonitorState State() const { return state_; }

private:
    HANDLE stop_ = nullptr;
    std::thread worker_;
    WPARAM generation_ = 0;
    MicMonitorState state_ = MicMonitorState::Off;
};
