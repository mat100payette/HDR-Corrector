#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "audio_relay.h"

#include "platform.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kAudioRelayStartupTimeoutMs = 8000;
constexpr DWORD kAudioRelayStopWaitMs = 2000;
constexpr wchar_t kAudioRelaySessionName[] = L"HDR Corrector Stream Audio";

class AudioActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit AudioActivationHandler(HANDLE completedEvent) : completedEvent_(completedEvent) {}

    HRESULT WaitForAudioClient(DWORD timeoutMilliseconds, ComPtr<IAudioClient>& audioClient) {
        const DWORD waitResult = WaitForSingleObject(completedEvent_, timeoutMilliseconds);
        if (waitResult == WAIT_TIMEOUT) {
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        if (waitResult != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (FAILED(activateResult_)) {
            return activateResult_;
        }
        return activatedInterface_.As(&audioClient);
    }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }

        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&references_);
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG references = InterlockedDecrement(&references_);
        if (references == 0) {
            delete this;
        }
        return references;
    }

    IFACEMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT operationResult = E_UNEXPECTED;
        IUnknown* activated = nullptr;
        const HRESULT hr = operation->GetActivateResult(&operationResult, &activated);
        activateResult_ = FAILED(hr) ? hr : operationResult;
        if (SUCCEEDED(activateResult_)) {
            activatedInterface_.Attach(activated);
        } else if (activated) {
            activated->Release();
        }
        SetEvent(completedEvent_);
        return S_OK;
    }

private:
    volatile LONG references_ = 1;
    HANDLE completedEvent_ = nullptr;
    HRESULT activateResult_ = E_PENDING;
    ComPtr<IUnknown> activatedInterface_;
};

HRESULT ActivateProcessLoopbackClient(ComPtr<IAudioClient>& audioClient) {
    HANDLE completedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completedEvent) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    auto* handler = new (std::nothrow) AudioActivationHandler(completedEvent);
    if (!handler) {
        CloseHandle(completedEvent);
        return E_OUTOFMEMORY;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT propVariant = {};
    propVariant.vt = VT_BLOB;
    propVariant.blob.cbSize = sizeof(activationParams);
    propVariant.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOperation;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &propVariant,
        handler,
        &asyncOperation);

    if (SUCCEEDED(hr)) {
        hr = handler->WaitForAudioClient(kAudioRelayStartupTimeoutMs, audioClient);
    }

    handler->Release();
    CloseHandle(completedEvent);
    return hr;
}

HRESULT CreateDefaultRenderClient(
    ComPtr<IAudioClient>& renderClient,
    ComPtr<IAudioRenderClient>& renderService,
    WAVEFORMATEX** format,
    UINT32& bufferFrames,
    HANDLE renderEvent) {

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        return hr;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &renderClient);
    if (FAILED(hr)) {
        return hr;
    }

    hr = renderClient->GetMixFormat(format);
    if (FAILED(hr)) {
        return hr;
    }

    constexpr DWORD streamFlags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = renderClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, *format, nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    hr = renderClient->SetEventHandle(renderEvent);
    if (FAILED(hr)) {
        return hr;
    }

    hr = renderClient->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) {
        return hr;
    }

    hr = renderClient->GetService(IID_PPV_ARGS(&renderService));
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<IAudioSessionControl> sessionControl;
    if (SUCCEEDED(renderClient->GetService(IID_PPV_ARGS(&sessionControl)))) {
        sessionControl->SetDisplayName(kAudioRelaySessionName, nullptr);
    }

    return S_OK;
}

HRESULT CreateLoopbackCaptureClient(
    WAVEFORMATEX* format,
    HANDLE captureEvent,
    ComPtr<IAudioClient>& captureClient,
    ComPtr<IAudioCaptureClient>& captureService) {

    HRESULT hr = ActivateProcessLoopbackClient(captureClient);
    if (FAILED(hr)) {
        return hr;
    }

    constexpr DWORD streamFlags =
        AUDCLNT_STREAMFLAGS_LOOPBACK |
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = captureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, format, nullptr);
    if (FAILED(hr)) {
        return hr;
    }

    hr = captureClient->SetEventHandle(captureEvent);
    if (FAILED(hr)) {
        return hr;
    }

    return captureClient->GetService(IID_PPV_ARGS(&captureService));
}

bool WaitForStopOrEvent(HANDLE stopEvent, HANDLE eventHandle, DWORD timeoutMilliseconds) {
    HANDLE handles[] = {stopEvent, eventHandle};
    return WaitForMultipleObjects(2, handles, FALSE, timeoutMilliseconds) == WAIT_OBJECT_0;
}

HRESULT RelayFrames(
    HANDLE stopEvent,
    HANDLE renderEvent,
    IAudioClient* renderClient,
    IAudioRenderClient* renderService,
    UINT32 renderBufferFrames,
    const WAVEFORMATEX* format,
    const BYTE* source,
    UINT32 sourceFrames,
    DWORD captureFlags) {

    UINT32 framesRemaining = sourceFrames;
    const BYTE* cursor = source;
    const bool silent = (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !source;

    while (framesRemaining > 0) {
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            return S_FALSE;
        }

        UINT32 padding = 0;
        HRESULT hr = renderClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            return hr;
        }

        const UINT32 availableFrames = renderBufferFrames - padding;
        if (availableFrames == 0) {
            if (WaitForStopOrEvent(stopEvent, renderEvent, kAudioRelayStopWaitMs)) {
                return S_FALSE;
            }
            continue;
        }

        const UINT32 framesToWrite = std::min(framesRemaining, availableFrames);
        BYTE* destination = nullptr;
        hr = renderService->GetBuffer(framesToWrite, &destination);
        if (FAILED(hr)) {
            return hr;
        }

        if (silent) {
            std::memset(destination, 0, static_cast<size_t>(framesToWrite) * format->nBlockAlign);
            hr = renderService->ReleaseBuffer(framesToWrite, AUDCLNT_BUFFERFLAGS_SILENT);
        } else {
            std::memcpy(destination, cursor, static_cast<size_t>(framesToWrite) * format->nBlockAlign);
            hr = renderService->ReleaseBuffer(framesToWrite, 0);
            cursor += static_cast<size_t>(framesToWrite) * format->nBlockAlign;
        }
        if (FAILED(hr)) {
            return hr;
        }

        framesRemaining -= framesToWrite;
    }

    return S_OK;
}

}  // namespace

AudioRelay::~AudioRelay() {
    Stop();
}

bool AudioRelay::Start(std::wstring& error) {
    Stop();

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    startedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !startedEvent_) {
        error = L"Could not create audio relay events: " + LastErrorMessage(GetLastError());
        Stop();
        return false;
    }

    {
        std::scoped_lock lock(stateMutex_);
        startupResult_ = E_PENDING;
        startupError_.clear();
    }

    try {
        worker_ = std::thread(&AudioRelay::Worker, this);
    } catch (...) {
        error = L"Could not start audio relay thread.";
        Stop();
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(startedEvent_, kAudioRelayStartupTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        error = waitResult == WAIT_TIMEOUT
            ? L"Timed out starting audio relay."
            : L"Audio relay startup wait failed: " + LastErrorMessage(GetLastError());
        Stop();
        return false;
    }

    std::scoped_lock lock(stateMutex_);
    if (FAILED(startupResult_)) {
        error = startupError_.empty() ? HResultMessage(startupResult_) : startupError_;
        Stop();
        return false;
    }

    return true;
}

void AudioRelay::Stop() {
    if (stopEvent_) {
        SetEvent(stopEvent_);
    }

    if (worker_.joinable()) {
        worker_.join();
    }

    if (startedEvent_) {
        CloseHandle(startedEvent_);
        startedEvent_ = nullptr;
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    running_ = false;
}

bool AudioRelay::IsRunning() const {
    return running_;
}

void AudioRelay::Worker() {
    HRESULT startupHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(startupHr);
    if (startupHr == RPC_E_CHANGED_MODE) {
        startupHr = S_OK;
    }

    HANDLE captureEvent = nullptr;
    HANDLE renderEvent = nullptr;
    WAVEFORMATEX* format = nullptr;
    ComPtr<IAudioClient> captureClient;
    ComPtr<IAudioCaptureClient> captureService;
    ComPtr<IAudioClient> renderClient;
    ComPtr<IAudioRenderClient> renderService;
    UINT32 renderBufferFrames = 0;

    auto finishStartup = [&](HRESULT hr, const std::wstring& message = {}) {
        {
            std::scoped_lock lock(stateMutex_);
            startupResult_ = hr;
            startupError_ = message;
        }
        SetEvent(startedEvent_);
    };

    captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent || !renderEvent) {
        finishStartup(HRESULT_FROM_WIN32(GetLastError()), L"Could not create audio relay buffer events.");
        if (captureEvent) {
            CloseHandle(captureEvent);
        }
        if (renderEvent) {
            CloseHandle(renderEvent);
        }
        if (uninitializeCom) {
            CoUninitialize();
        }
        return;
    }

    if (SUCCEEDED(startupHr)) {
        startupHr = CreateDefaultRenderClient(renderClient, renderService, &format, renderBufferFrames, renderEvent);
    }
    if (SUCCEEDED(startupHr)) {
        startupHr = CreateLoopbackCaptureClient(format, captureEvent, captureClient, captureService);
    }
    if (SUCCEEDED(startupHr)) {
        BYTE* silence = nullptr;
        startupHr = renderService->GetBuffer(renderBufferFrames, &silence);
        if (SUCCEEDED(startupHr)) {
            startupHr = renderService->ReleaseBuffer(renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }
    if (SUCCEEDED(startupHr)) {
        startupHr = renderClient->Start();
    }
    if (SUCCEEDED(startupHr)) {
        startupHr = captureClient->Start();
    }

    if (FAILED(startupHr)) {
        finishStartup(startupHr, L"Audio relay setup failed: " + HResultMessage(startupHr));
    } else {
        running_ = true;
        finishStartup(S_OK);

        HANDLE waitHandles[] = {stopEvent_, captureEvent};
        bool stopRequested = false;
        while (!stopRequested) {
            const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (waitResult == WAIT_OBJECT_0) {
                stopRequested = true;
                break;
            }
            if (waitResult != WAIT_OBJECT_0 + 1) {
                Log(L"Audio relay wait failed: " + LastErrorMessage(GetLastError()));
                break;
            }

            UINT32 packetFrames = 0;
            HRESULT hr = captureService->GetNextPacketSize(&packetFrames);
            while (SUCCEEDED(hr) && packetFrames > 0) {
                BYTE* data = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                hr = captureService->GetBuffer(&data, &framesAvailable, &flags, &devicePosition, &qpcPosition);
                if (FAILED(hr)) {
                    break;
                }

                const HRESULT relayHr = RelayFrames(
                    stopEvent_,
                    renderEvent,
                    renderClient.Get(),
                    renderService.Get(),
                    renderBufferFrames,
                    format,
                    data,
                    framesAvailable,
                    flags);

                captureService->ReleaseBuffer(framesAvailable);
                if (relayHr == S_FALSE) {
                    stopRequested = true;
                    break;
                }
                if (FAILED(relayHr)) {
                    Log(L"Audio relay render failed: " + HResultMessage(relayHr));
                    stopRequested = true;
                    break;
                }

                hr = captureService->GetNextPacketSize(&packetFrames);
            }

            if (FAILED(hr)) {
                Log(L"Audio relay capture failed: " + HResultMessage(hr));
                break;
            }
        }
    }

    if (captureClient) {
        captureClient->Stop();
    }
    if (renderClient) {
        renderClient->Stop();
    }
    if (format) {
        CoTaskMemFree(format);
    }
    CloseHandle(captureEvent);
    CloseHandle(renderEvent);
    running_ = false;
    if (uninitializeCom) {
        CoUninitialize();
    }
}
