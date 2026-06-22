#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

class AudioRelay {
public:
    AudioRelay() = default;
    ~AudioRelay();

    AudioRelay(const AudioRelay&) = delete;
    AudioRelay& operator=(const AudioRelay&) = delete;

    bool Start(std::wstring& error);
    void Stop();
    bool IsRunning() const;

private:
    void Worker();

    HANDLE stopEvent_ = nullptr;
    HANDLE startedEvent_ = nullptr;
    std::thread worker_;
    std::atomic_bool running_ = false;
    HRESULT startupResult_ = S_OK;
    mutable std::mutex stateMutex_;
    std::wstring startupError_;
};
