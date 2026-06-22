#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "audio_relay.h"
#include "clipboard.h"
#include "platform.h"
#include "resource.h"

#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <wincodec.h>
#include <winreg.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureAccess;
using winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus;
using DxgiInterfaceAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

namespace {

constexpr wchar_t kAppName[] = L"HDR Corrector";
constexpr wchar_t kMutexName[] = L"Local\\HDRCorrector.SingleInstance";
constexpr wchar_t kHiddenWindowClass[] = L"HDRCorrector.HiddenWindow";
constexpr wchar_t kMirrorWindowClass[] = L"HDRCorrector.MirrorWindow";
constexpr wchar_t kRunValueName[] = L"HDRCorrector";

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kRenderMirrorMessage = WM_APP + 2;
constexpr UINT_PTR kTrayIconId = 1;
constexpr int kHotkeyScreenshot = 100;
constexpr int kHotkeyMirror = 101;
constexpr int kHotkeyWindowScreenshot = 102;

constexpr UINT kCmdScreenshot = 200;
constexpr UINT kCmdMirror = 201;
constexpr UINT kCmdSelectMonitor = 202;
constexpr UINT kCmdStartup = 203;
constexpr UINT kCmdOpenFolder = 204;
constexpr UINT kCmdExit = 205;
constexpr UINT kCmdAudioRelay = 206;
constexpr UINT kCmdWindowScreenshot = 207;

constexpr DXGI_FORMAT kCaptureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DirectXPixelFormat kFramePoolFormat = DirectXPixelFormat::R16G16B16A16Float;
constexpr UINT kFramePoolBufferCount = 2;

constexpr size_t kRegistryValueBufferCharacters = 2048;
constexpr float kDisplayConfigSdrWhiteLevelBase = 1000.0f;

constexpr int kMirrorInitialWidth = 1280;
constexpr int kMirrorInitialHeight = 720;
constexpr int kMirrorInitialOffset = 80;
constexpr UINT kSwapChainBufferCount = 2;
constexpr UINT kFullscreenTriangleVertexCount = 3;
constexpr UINT kPresentSyncInterval = 1;
constexpr UINT kPresentFlags = 0;
constexpr float kMirrorWaitingClearColor[4] = {0.02f, 0.02f, 0.02f, 1.0f};
constexpr float kMirrorFrameClearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

constexpr DWORD kExistingCaptureFrameWaitMs = 500;
constexpr DWORD kNewCaptureFrameWaitMs = 2000;

constexpr UINT kRgbaRedIndex = 0;
constexpr UINT kRgbaGreenIndex = 1;
constexpr UINT kRgbaBlueIndex = 2;
constexpr UINT kBgraBlueIndex = 0;
constexpr UINT kBgraGreenIndex = 1;
constexpr UINT kBgraRedIndex = 2;
constexpr UINT kBgraAlphaIndex = 3;
constexpr UINT kHalfRgbaChannelCount = 4;
constexpr UINT kHalfRgbaBytesPerPixel = sizeof(uint16_t) * kHalfRgbaChannelCount;
constexpr UINT kBgraBytesPerPixel = 4;
constexpr uint8_t kOpaqueAlpha = 255;

constexpr uint32_t kHalfSignMask = 0x8000u;
constexpr uint32_t kHalfExponentMask = 0x1fu;
constexpr uint32_t kHalfMantissaMask = 0x03ffu;
constexpr uint32_t kHalfHiddenMantissaBit = 0x0400u;
constexpr uint32_t kHalfExponentMax = 31u;
constexpr int32_t kHalfExponentBias = 15;
constexpr int32_t kFloatExponentBias = 127;
constexpr uint32_t kHalfExponentShift = 10;
constexpr uint32_t kHalfSignToFloatShift = 16;
constexpr uint32_t kFloatExponentShift = 23;
constexpr uint32_t kHalfMantissaToFloatShift = 13;
constexpr uint32_t kFloatInfinityBits = 0x7f800000u;

constexpr float kSrgbLinearThreshold = 0.0031308f;
constexpr float kSrgbLinearScale = 12.92f;
constexpr float kSrgbGammaScale = 1.055f;
constexpr float kSrgbGamma = 2.4f;
constexpr float kSrgbGammaOffset = 0.055f;
constexpr float kUint8Max = 255.0f;

const char kFullscreenTriangleShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

cbuffer ToneMapConstants : register(b0) {
    float sdrWhiteScale;
    float3 padding;
};

static const float kSrgbLinearThreshold = 0.0031308;
static const float kSrgbLinearScale = 12.92;
static const float kSrgbGammaScale = 1.055;
static const float kSrgbGamma = 2.4;
static const float kSrgbGammaOffset = 0.055;

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    // Oversized fullscreen triangle: the off-screen vertices avoid a diagonal seam.
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
    output.uv = uvs[id];
    return output;
}

float3 LinearToSrgb(float3 color) {
    float3 low = color * kSrgbLinearScale;
    float3 high = kSrgbGammaScale * pow(color, 1.0 / kSrgbGamma) - kSrgbGammaOffset;
    return float3(
        color.r <= kSrgbLinearThreshold ? low.r : high.r,
        color.g <= kSrgbLinearThreshold ? low.g : high.g,
        color.b <= kSrgbLinearThreshold ? low.b : high.b);
}

float4 PSMain(VSOut input) : SV_TARGET {
    float3 hdr = max(sourceTexture.Sample(sourceSampler, input.uv).rgb, 0.0);

    // Windows HDR desktop capture is scRGB linear. In HDR mode, SDR UI white is
    // scaled above 1.0 according to the user's SDR brightness setting, so normalize
    // by that multiplier before encoding an SDR mirror target.
    float whiteScale = max(sdrWhiteScale, 1.0);
    float3 mapped = saturate(hdr / whiteScale);
    return float4(LinearToSrgb(mapped), 1.0);
}
)";

struct ToneMapConstants {
    float sdrWhiteScale = 1.0f;
    float padding[3] = {};
};

template <size_t Size>
void CopyTruncated(wchar_t (&destination)[Size], const std::wstring& source) {
    wcsncpy_s(destination, Size, source.c_str(), _TRUNCATE);
}

std::wstring QuoteForRegistry(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

std::filesystem::path CurrentExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[kRegistryValueBufferCharacters] = {};
    DWORD valueSize = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value), &valueSize);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }

    const auto exe = QuoteForRegistry(CurrentExecutablePath());
    return _wcsicmp(value, exe.c_str()) == 0;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key = nullptr;
    const LONG openResult = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);

    if (openResult != ERROR_SUCCESS) {
        Log(L"RegCreateKeyExW failed: " + LastErrorMessage(openResult));
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const auto value = QuoteForRegistry(CurrentExecutablePath());
        result = RegSetValueExW(
            key,
            kRunValueName,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);

    if (result != ERROR_SUCCESS) {
        Log(L"Startup registry update failed: " + LastErrorMessage(result));
        return false;
    }

    return true;
}

HMONITOR PrimaryMonitor() {
    const POINT origin = {0, 0};
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

HMONITOR MonitorUnderCursor() {
    POINT cursor = {};
    GetCursorPos(&cursor);
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
}

std::wstring WindowTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return L"active window";
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied <= 0) {
        return L"active window";
    }

    title.resize(static_cast<size_t>(copied));
    return title;
}

std::wstring MonitorName(HMONITOR monitor) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        return info.szDevice;
    }
    return L"selected monitor";
}

std::wstring AppCapabilityAccessStatusName(AppCapabilityAccessStatus status) {
    switch (status) {
    case AppCapabilityAccessStatus::Allowed:
        return L"Allowed";
    case AppCapabilityAccessStatus::DeniedBySystem:
        return L"DeniedBySystem";
    case AppCapabilityAccessStatus::DeniedByUser:
        return L"DeniedByUser";
    case AppCapabilityAccessStatus::NotDeclaredByApp:
        return L"NotDeclaredByApp";
    case AppCapabilityAccessStatus::UserPromptRequired:
        return L"UserPromptRequired";
    default:
        return L"Unknown";
    }
}

float SdrWhiteScaleForMonitor(HMONITOR monitor) {
    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return 1.0f;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS) {
        Log(L"GetDisplayConfigBufferSizes failed while reading SDR white: " + LastErrorMessage(result));
        return 1.0f;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (result != ERROR_SUCCESS) {
        Log(L"QueryDisplayConfig failed while reading SDR white: " + LastErrorMessage(result));
        return 1.0f;
    }

    paths.resize(pathCount);
    for (const auto& path : paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;

        result = DisplayConfigGetDeviceInfo(&sourceName.header);
        if (result != ERROR_SUCCESS || _wcsicmp(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
        whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        whiteLevel.header.size = sizeof(whiteLevel);
        whiteLevel.header.adapterId = path.targetInfo.adapterId;
        whiteLevel.header.id = path.targetInfo.id;

        result = DisplayConfigGetDeviceInfo(&whiteLevel.header);
        if (result == ERROR_SUCCESS && whiteLevel.SDRWhiteLevel > 0) {
            // Windows reports SDR white as a 1000-based multiplier; 1000 maps to 1.0 scRGB.
            return std::max(1.0f, static_cast<float>(whiteLevel.SDRWhiteLevel) / kDisplayConfigSdrWhiteLevelBase);
        }

        Log(L"DisplayConfigGetDeviceInfo(GET_SDR_WHITE_LEVEL) failed: " + LastErrorMessage(result));
        return 1.0f;
    }

    return 1.0f;
}

float HalfToFloat(uint16_t value) {
    // The captured texture is RGBA half-float. Convert manually so the CPU preview
    // uses the same frame data as the HDR file.
    const uint32_t sign = (value & kHalfSignMask) << kHalfSignToFloatShift;
    int32_t exponent = static_cast<int32_t>((value >> kHalfExponentShift) & kHalfExponentMask);
    uint32_t mantissa = value & kHalfMantissaMask;
    uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & kHalfHiddenMantissaBit) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= kHalfMantissaMask;
            exponent = exponent + (kFloatExponentBias - kHalfExponentBias);
            bits = sign | (static_cast<uint32_t>(exponent) << kFloatExponentShift) | (mantissa << kHalfMantissaToFloatShift);
        }
    } else if (exponent == kHalfExponentMax) {
        bits = sign | kFloatInfinityBits | (mantissa << kHalfMantissaToFloatShift);
    } else {
        exponent = exponent + (kFloatExponentBias - kHalfExponentBias);
        bits = sign | (static_cast<uint32_t>(exponent) << kFloatExponentShift) | (mantissa << kHalfMantissaToFloatShift);
    }

    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

uint8_t ToSrgb8(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    const float gammaEncoded = linear <= kSrgbLinearThreshold
        ? linear * kSrgbLinearScale
        : kSrgbGammaScale * std::pow(linear, 1.0f / kSrgbGamma) - kSrgbGammaOffset;
    return static_cast<uint8_t>(std::lround(gammaEncoded * kUint8Max));
}

bool SaveWicBitmapFromMemory(
    const std::filesystem::path& path,
    REFGUID containerFormat,
    REFGUID pixelFormat,
    UINT width,
    UINT height,
    UINT stride,
    const std::vector<uint8_t>& pixels,
    std::wstring& error) {

    winrt::com_ptr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.put()));
    }
    if (FAILED(hr)) {
        error = L"Could not create WIC imaging factory: " + HResultMessage(hr);
        return false;
    }

    winrt::com_ptr<IWICBitmap> source;
    hr = factory->CreateBitmapFromMemory(
        width,
        height,
        pixelFormat,
        stride,
        static_cast<UINT>(pixels.size()),
        const_cast<BYTE*>(pixels.data()),
        source.put());
    if (FAILED(hr)) {
        error = L"CreateBitmapFromMemory failed: " + HResultMessage(hr);
        return false;
    }

    winrt::com_ptr<IWICStream> stream;
    hr = factory->CreateStream(stream.put());
    if (FAILED(hr)) {
        error = L"CreateStream failed: " + HResultMessage(hr);
        return false;
    }

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        error = L"Could not open image for writing: " + HResultMessage(hr);
        return false;
    }

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, encoder.put());
    if (FAILED(hr)) {
        error = L"CreateEncoder failed: " + HResultMessage(hr);
        return false;
    }

    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        error = L"Encoder initialization failed: " + HResultMessage(hr);
        return false;
    }

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> propertyBag;
    hr = encoder->CreateNewFrame(frame.put(), propertyBag.put());
    if (FAILED(hr)) {
        error = L"CreateNewFrame failed: " + HResultMessage(hr);
        return false;
    }

    hr = frame->Initialize(propertyBag.get());
    if (FAILED(hr)) {
        error = L"Frame initialization failed: " + HResultMessage(hr);
        return false;
    }

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) {
        error = L"SetSize failed: " + HResultMessage(hr);
        return false;
    }

    WICPixelFormatGUID acceptedFormat = pixelFormat;
    hr = frame->SetPixelFormat(&acceptedFormat);
    if (FAILED(hr)) {
        error = L"SetPixelFormat failed: " + HResultMessage(hr);
        return false;
    }

    if (IsEqualGUID(acceptedFormat, pixelFormat)) {
        hr = frame->WriteSource(source.get(), nullptr);
    } else {
        winrt::com_ptr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.put());
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(
                source.get(),
                acceptedFormat,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }
        if (SUCCEEDED(hr)) {
            hr = frame->WriteSource(converter.get(), nullptr);
        }
    }

    if (FAILED(hr)) {
        error = L"WriteSource failed: " + HResultMessage(hr);
        return false;
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        error = L"Frame commit failed: " + HResultMessage(hr);
        return false;
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        error = L"Image commit failed: " + HResultMessage(hr);
        return false;
    }

    return true;
}

class App {
public:
    bool Initialize(HINSTANCE instance) {
        instance_ = instance;

        if (!RegisterWindowClasses()) {
            return false;
        }

        hiddenHwnd_ = CreateWindowExW(
            0,
            kHiddenWindowClass,
            kAppName,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            nullptr,
            nullptr,
            instance_,
            this);

        if (!hiddenHwnd_) {
            Log(L"CreateWindowExW failed: " + LastErrorMessage(GetLastError()));
            return false;
        }

        if (!InitializeD3D()) {
            return false;
        }

        if (!GraphicsCaptureSession::IsSupported()) {
            Log(L"Windows Graphics Capture is not supported on this device.");
            return false;
        }

        frameReadyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!frameReadyEvent_) {
            Log(L"CreateEventW frameReadyEvent failed: " + LastErrorMessage(GetLastError()));
            return false;
        }

        selectedMonitor_ = PrimaryMonitor();
        capturedMonitorName_ = MonitorName(selectedMonitor_);
        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon();
        RegisterHotkeys();
        return true;
    }

    int Run() {
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK HiddenWindowProcSetup(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::HiddenWindowProcThunk));
            return app->HiddenWindowProc(hwnd, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK HiddenWindowProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return app ? app->HiddenWindowProc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK MirrorWindowProcSetup(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::MirrorWindowProcThunk));
            return app->MirrorWindowProc(hwnd, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK MirrorWindowProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return app ? app->MirrorWindowProc(hwnd, message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HiddenWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbarCreatedMessage_) {
            AddTrayIcon();
            return 0;
        }

        switch (message) {
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case WM_HOTKEY:
            HandleHotkey(static_cast<int>(wparam));
            return 0;
        case kTrayMessage:
            HandleTrayMessage(lparam);
            return 0;
        case WM_DESTROY:
            Shutdown();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    LRESULT MirrorWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_SIZE:
            ResizeSwapChain();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            BeginPaint(hwnd, &paint);
            EndPaint(hwnd, &paint);
            RenderMirror();
            return 0;
        }
        case kRenderMirrorMessage:
            RenderMirror();
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            mirrorVisible_ = false;
            audioRelay_.Stop();
            StopCapture();
            UpdateTrayTooltip();
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    bool RegisterWindowClasses() const {
        WNDCLASSEXW hiddenClass = {};
        hiddenClass.cbSize = sizeof(hiddenClass);
        hiddenClass.lpfnWndProc = &App::HiddenWindowProcSetup;
        hiddenClass.hInstance = instance_;
        hiddenClass.lpszClassName = kHiddenWindowClass;
        hiddenClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hiddenClass.hIcon = AppIcon();
        hiddenClass.hIconSm = AppSmallIcon();

        if (!RegisterClassExW(&hiddenClass)) {
            Log(L"RegisterClassExW hidden failed: " + LastErrorMessage(GetLastError()));
            return false;
        }

        WNDCLASSEXW mirrorClass = {};
        mirrorClass.cbSize = sizeof(mirrorClass);
        mirrorClass.lpfnWndProc = &App::MirrorWindowProcSetup;
        mirrorClass.hInstance = instance_;
        mirrorClass.lpszClassName = kMirrorWindowClass;
        mirrorClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        mirrorClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        mirrorClass.hIcon = AppIcon();
        mirrorClass.hIconSm = AppSmallIcon();

        if (!RegisterClassExW(&mirrorClass)) {
            Log(L"RegisterClassExW mirror failed: " + LastErrorMessage(GetLastError()));
            return false;
        }

        return true;
    }

    HICON AppIcon() const {
        HICON icon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP_ICON));
        return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    }

    HICON AppSmallIcon() const {
        HICON icon = reinterpret_cast<HICON>(LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_SHARED));
        return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
    }

    bool InitializeD3D() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            d3dDevice_.put(),
            &createdLevel,
            d3dContext_.put());

#ifdef DEBUG
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                d3dDevice_.put(),
                &createdLevel,
                d3dContext_.put());
        }
#endif

        if (FAILED(hr)) {
            Log(L"D3D11CreateDevice failed: " + HResultMessage(hr));
            return false;
        }

        auto dxgiDevice = d3dDevice_.as<IDXGIDevice>();

        winrt::com_ptr<::IInspectable> inspectableDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectableDevice.put());
        if (FAILED(hr)) {
            Log(L"CreateDirect3D11DeviceFromDXGIDevice failed: " + HResultMessage(hr));
            return false;
        }

        direct3DDevice_ = inspectableDevice.as<IDirect3DDevice>();
        return InitializeRenderer();
    }

    bool InitializeRenderer() {
        winrt::com_ptr<ID3DBlob> vertexShaderBlob;
        winrt::com_ptr<ID3DBlob> pixelShaderBlob;
        winrt::com_ptr<ID3DBlob> errors;

        HRESULT hr = D3DCompile(
            kFullscreenTriangleShader,
            strlen(kFullscreenTriangleShader),
            nullptr,
            nullptr,
            nullptr,
            "VSMain",
            "vs_5_0",
            0,
            0,
            vertexShaderBlob.put(),
            errors.put());
        if (FAILED(hr)) {
            Log(L"Vertex shader compilation failed.");
            return false;
        }

        errors = nullptr;
        hr = D3DCompile(
            kFullscreenTriangleShader,
            strlen(kFullscreenTriangleShader),
            nullptr,
            nullptr,
            nullptr,
            "PSMain",
            "ps_5_0",
            0,
            0,
            pixelShaderBlob.put(),
            errors.put());
        if (FAILED(hr)) {
            Log(L"Pixel shader compilation failed.");
            return false;
        }

        hr = d3dDevice_->CreateVertexShader(
            vertexShaderBlob->GetBufferPointer(),
            vertexShaderBlob->GetBufferSize(),
            nullptr,
            vertexShader_.put());
        if (FAILED(hr)) {
            Log(L"CreateVertexShader failed: " + HResultMessage(hr));
            return false;
        }

        hr = d3dDevice_->CreatePixelShader(
            pixelShaderBlob->GetBufferPointer(),
            pixelShaderBlob->GetBufferSize(),
            nullptr,
            pixelShader_.put());
        if (FAILED(hr)) {
            Log(L"CreatePixelShader failed: " + HResultMessage(hr));
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = d3dDevice_->CreateSamplerState(&samplerDesc, samplerState_.put());
        if (FAILED(hr)) {
            Log(L"CreateSamplerState failed: " + HResultMessage(hr));
            return false;
        }

        D3D11_BUFFER_DESC constantsDesc = {};
        constantsDesc.ByteWidth = sizeof(ToneMapConstants);
        constantsDesc.Usage = D3D11_USAGE_DEFAULT;
        constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        ToneMapConstants constants = {};
        D3D11_SUBRESOURCE_DATA constantsData = {};
        constantsData.pSysMem = &constants;
        hr = d3dDevice_->CreateBuffer(&constantsDesc, &constantsData, toneMapConstants_.put());
        if (FAILED(hr)) {
            Log(L"CreateBuffer tone map constants failed: " + HResultMessage(hr));
            return false;
        }

        return true;
    }

    void RegisterHotkeys() const {
        if (!RegisterHotKey(hiddenHwnd_, kHotkeyScreenshot, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) {
            Log(L"Could not register Ctrl+PrtScn: " + LastErrorMessage(GetLastError()));
        }

        if (!RegisterHotKey(hiddenHwnd_, kHotkeyWindowScreenshot, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SNAPSHOT)) {
            Log(L"Could not register Ctrl+Alt+PrtScn: " + LastErrorMessage(GetLastError()));
        }

        if (!RegisterHotKey(hiddenHwnd_, kHotkeyMirror, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H')) {
            Log(L"Could not register Ctrl+Alt+H: " + LastErrorMessage(GetLastError()));
        }
    }

    void AddTrayIcon() {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hiddenHwnd_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = AppSmallIcon();
        CopyTruncated(data.szTip, TrayTooltip());

        Shell_NotifyIconW(NIM_ADD, &data);

        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    void UpdateTrayTooltip() {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hiddenHwnd_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        CopyTruncated(data.szTip, TrayTooltip());
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    std::wstring TrayTooltip() const {
        if (!mirrorVisible_) {
            return L"HDR Corrector";
        }
        return audioRelay_.IsRunning() ? L"HDR Corrector - mirror and audio relay visible" : L"HDR Corrector - mirror visible";
    }

    void ShowNotification(const std::wstring& title, const std::wstring& message) const {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hiddenHwnd_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_INFO;
        CopyTruncated(data.szInfoTitle, title);
        CopyTruncated(data.szInfo, message);
        data.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void HandleTrayMessage(LPARAM lparam) {
        switch (LOWORD(lparam)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            ShowContextMenu();
            break;
        case WM_LBUTTONDBLCLK:
            CaptureScreenshot();
            break;
        default:
            break;
        }
    }

    void ShowContextMenu() {
        POINT cursor = {};
        GetCursorPos(&cursor);

        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        AppendMenuW(menu, MF_STRING, kCmdScreenshot, L"Capture HDR screenshot\tCtrl+PrtScn");
        AppendMenuW(menu, MF_STRING, kCmdWindowScreenshot, L"Capture active window\tCtrl+Alt+PrtScn");
        AppendMenuW(
            menu,
            MF_STRING | (mirrorVisible_ ? MF_CHECKED : MF_UNCHECKED),
            kCmdMirror,
            L"Show stream mirror\tCtrl+Alt+H");
        AppendMenuW(
            menu,
            MF_STRING | (audioRelayEnabled_ ? MF_CHECKED : MF_UNCHECKED),
            kCmdAudioRelay,
            L"Relay desktop audio with mirror");
        AppendMenuW(menu, MF_STRING, kCmdSelectMonitor, L"Use monitor under cursor");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), kCmdStartup, L"Run at startup");
        AppendMenuW(menu, MF_STRING, kCmdOpenFolder, L"Open screenshots folder");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

        SetForegroundWindow(hiddenHwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hiddenHwnd_, nullptr);
        DestroyMenu(menu);
    }

    void HandleCommand(UINT command) {
        switch (command) {
        case kCmdScreenshot:
            CaptureScreenshot();
            break;
        case kCmdWindowScreenshot:
            CaptureActiveWindowScreenshot();
            break;
        case kCmdMirror:
            ToggleMirrorWindow();
            break;
        case kCmdSelectMonitor:
            SelectMonitorUnderCursor();
            break;
        case kCmdStartup:
            ToggleStartup();
            break;
        case kCmdAudioRelay:
            ToggleAudioRelay();
            break;
        case kCmdOpenFolder:
            OpenScreenshotsFolder();
            break;
        case kCmdExit:
            DestroyWindow(hiddenHwnd_);
            break;
        default:
            break;
        }
    }

    void HandleHotkey(int hotkey) {
        switch (hotkey) {
        case kHotkeyScreenshot:
            CaptureScreenshot();
            break;
        case kHotkeyWindowScreenshot:
            CaptureActiveWindowScreenshot();
            break;
        case kHotkeyMirror:
            ToggleMirrorWindow();
            break;
        default:
            break;
        }
    }

    bool StartCaptureItem(const GraphicsCaptureItem& item, const std::wstring& label, float sdrWhiteScale, bool notify) {
        StopCapture();

        try {
            if (frameReadyEvent_) {
                ResetEvent(frameReadyEvent_);
            }

            captureItem_ = item;
            captureSize_ = item.Size();
            capturedMonitorName_ = label;
            sdrWhiteScale_ = sdrWhiteScale;
            framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3DDevice_,
                kFramePoolFormat,
                kFramePoolBufferCount,
                captureSize_);
            frameArrivedToken_ = framePool_.FrameArrived({this, &App::OnFrameArrived});
            captureSession_ = framePool_.CreateCaptureSession(captureItem_);
            ApplyCaptureBorderPreference();
            captureSession_.IsCursorCaptureEnabled(true);
            captureSession_.StartCapture();

            {
                std::scoped_lock lock(frameMutex_);
                latestTexture_ = nullptr;
                latestWidth_ = 0;
                latestHeight_ = 0;
            }

            if (notify) {
                std::wstringstream stream;
                stream << L"Capturing " << capturedMonitorName_ << L" at " << captureSize_.Width << L"x" << captureSize_.Height
                       << L", SDR white scale " << sdrWhiteScale_;
                ShowNotification(kAppName, stream.str());
            }
            return true;
        } catch (const winrt::hresult_error& error) {
            Log(L"StartCapture failed: " + std::wstring(error.message()));
            ShowNotification(kAppName, L"Could not start HDR capture. See log for details.");
            return false;
        }
    }

    bool StartCapture(HMONITOR monitor, bool notify = true) {
        try {
            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            GraphicsCaptureItem item = nullptr;
            winrt::check_hresult(interop->CreateForMonitor(
                monitor,
                winrt::guid_of<GraphicsCaptureItem>(),
                winrt::put_abi(item)));

            return StartCaptureItem(item, MonitorName(monitor), SdrWhiteScaleForMonitor(monitor), notify);
        } catch (const winrt::hresult_error& error) {
            Log(L"Create monitor capture item failed: " + std::wstring(error.message()));
            ShowNotification(kAppName, L"Could not start HDR capture. See log for details.");
            return false;
        }
    }

    bool StartWindowCapture(HWND hwnd, bool notify = true) {
        try {
            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            GraphicsCaptureItem item = nullptr;
            winrt::check_hresult(interop->CreateForWindow(
                hwnd,
                winrt::guid_of<GraphicsCaptureItem>(),
                winrt::put_abi(item)));

            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            return StartCaptureItem(item, WindowTitle(hwnd), SdrWhiteScaleForMonitor(monitor), notify);
        } catch (const winrt::hresult_error& error) {
            Log(L"Create active window capture item failed: " + std::wstring(error.message()));
            ShowNotification(kAppName, L"Could not capture the active window. See log for details.");
            return false;
        }
    }

    bool HasPackageIdentity() {
        if (!packageIdentityChecked_) {
            UINT32 length = 0;
            const LONG result = GetCurrentPackageFullName(&length, nullptr);
            hasPackageIdentity_ = result == ERROR_INSUFFICIENT_BUFFER;
            if (result != ERROR_INSUFFICIENT_BUFFER && result != APPMODEL_ERROR_NO_PACKAGE) {
                Log(L"GetCurrentPackageFullName failed while checking package identity: " + LastErrorMessage(result));
            }
            packageIdentityChecked_ = true;
        }

        return hasPackageIdentity_;
    }

    AppCapabilityAccessStatus RequestBorderlessAccessOnBackgroundThread() {
        std::promise<AppCapabilityAccessStatus> promise;
        auto future = promise.get_future();
        std::thread worker([promise = std::move(promise)]() mutable {
            bool apartmentInitialized = false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                apartmentInitialized = true;
                promise.set_value(GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get());
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
            if (apartmentInitialized) {
                winrt::uninit_apartment();
            }
        });

        worker.join();
        return future.get();
    }

    bool CanUseBorderlessCapture() {
        if (!HasPackageIdentity()) {
            return false;
        }

        if (!borderlessCaptureAccessChecked_) {
            borderlessCaptureAccessChecked_ = true;
            try {
                const AppCapabilityAccessStatus status = RequestBorderlessAccessOnBackgroundThread();
                borderlessCaptureAllowed_ = status == AppCapabilityAccessStatus::Allowed;
                if (borderlessCaptureAllowed_) {
                    Log(L"Borderless capture access granted.");
                } else {
                    Log(L"Borderless capture access not granted: " + AppCapabilityAccessStatusName(status));
                }
            } catch (const winrt::hresult_error& error) {
                Log(L"Borderless capture access request failed: " + std::wstring(error.message()));
            } catch (...) {
                Log(L"Borderless capture access request failed.");
            }
        }

        return borderlessCaptureAllowed_;
    }

    void ApplyCaptureBorderPreference() {
        if (!captureSession_ || !CanUseBorderlessCapture()) {
            return;
        }

        try {
            captureSession_.IsBorderRequired(false);
        } catch (const winrt::hresult_error& error) {
            Log(L"Could not disable capture border: " + std::wstring(error.message()));
        }
    }

    bool WaitForFirstFrame(DWORD timeoutMilliseconds) const {
        if (!frameReadyEvent_) {
            return false;
        }
        return WaitForSingleObject(frameReadyEvent_, timeoutMilliseconds) == WAIT_OBJECT_0;
    }

    void StopCapture() {
        if (framePool_) {
            framePool_.FrameArrived(frameArrivedToken_);
        }

        if (captureSession_) {
            captureSession_.Close();
            captureSession_ = nullptr;
        }

        if (framePool_) {
            framePool_.Close();
            framePool_ = nullptr;
        }

        captureItem_ = nullptr;
    }

    void OnFrameArrived(Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) {
                return;
            }

            const auto contentSize = frame.ContentSize();
            if (contentSize.Width != captureSize_.Width || contentSize.Height != captureSize_.Height) {
                captureSize_ = contentSize;
                framePool_.Recreate(direct3DDevice_, kFramePoolFormat, kFramePoolBufferCount, captureSize_);
            }

            auto access = frame.Surface().as<DxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> frameTexture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), frameTexture.put_void()));

            D3D11_TEXTURE2D_DESC desc = {};
            frameTexture->GetDesc(&desc);

            std::scoped_lock lock(frameMutex_);
            EnsureLatestTexture(desc.Width, desc.Height);
            d3dContext_->CopyResource(latestTexture_.get(), frameTexture.get());
            latestWidth_ = desc.Width;
            latestHeight_ = desc.Height;
            if (frameReadyEvent_) {
                SetEvent(frameReadyEvent_);
            }

            if (mirrorHwnd_ && mirrorVisible_) {
                PostMessageW(mirrorHwnd_, kRenderMirrorMessage, 0, 0);
            }
        } catch (const winrt::hresult_error& error) {
            Log(L"Frame processing failed: " + std::wstring(error.message()));
        }
    }

    void EnsureLatestTexture(UINT width, UINT height) {
        if (latestTexture_) {
            D3D11_TEXTURE2D_DESC existing = {};
            latestTexture_->GetDesc(&existing);
            if (existing.Width == width && existing.Height == height) {
                return;
            }
        }

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kCaptureFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        latestTexture_ = nullptr;
        const HRESULT hr = d3dDevice_->CreateTexture2D(&desc, nullptr, latestTexture_.put());
        if (FAILED(hr)) {
            Log(L"Create latest HDR texture failed: " + HResultMessage(hr));
        }
    }

    void ToggleMirrorWindow() {
        if (!mirrorHwnd_) {
            CreateMirrorWindow();
        }

        if (!mirrorHwnd_) {
            ShowNotification(kAppName, L"Could not create stream mirror.");
            return;
        }

        const bool showMirror = !mirrorVisible_;
        if (showMirror && !StartCapture(selectedMonitor_)) {
            return;
        }

        mirrorVisible_ = showMirror;
        ShowWindow(mirrorHwnd_, mirrorVisible_ ? SW_SHOWNORMAL : SW_HIDE);
        if (mirrorVisible_) {
            SetForegroundWindow(mirrorHwnd_);
            RenderMirror();
            StartAudioRelayForMirror();
        } else {
            audioRelay_.Stop();
            StopCapture();
        }
        UpdateTrayTooltip();
    }

    void StartAudioRelayForMirror() {
        if (!audioRelayEnabled_ || audioRelay_.IsRunning()) {
            return;
        }

        std::wstring error;
        if (!audioRelay_.Start(error)) {
            Log(L"Audio relay failed: " + error);
            ShowNotification(kAppName, L"Stream mirror started, but audio relay failed. See log for details.");
            return;
        }

        ShowNotification(kAppName, L"Stream mirror started with desktop audio relay.");
        UpdateTrayTooltip();
    }

    void ToggleAudioRelay() {
        audioRelayEnabled_ = !audioRelayEnabled_;

        if (!audioRelayEnabled_) {
            audioRelay_.Stop();
            ShowNotification(kAppName, L"Audio relay disabled.");
        } else if (mirrorVisible_) {
            StartAudioRelayForMirror();
        } else {
            ShowNotification(kAppName, L"Audio relay will start with the stream mirror.");
        }

        UpdateTrayTooltip();
    }

    void SelectMonitorUnderCursor() {
        selectedMonitor_ = MonitorUnderCursor();
        capturedMonitorName_ = MonitorName(selectedMonitor_);
        sdrWhiteScale_ = SdrWhiteScaleForMonitor(selectedMonitor_);

        if (mirrorVisible_) {
            StartCapture(selectedMonitor_);
        }

        ShowNotification(kAppName, L"Selected " + capturedMonitorName_);
    }

    void CreateMirrorWindow() {
        RECT workArea = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int width = kMirrorInitialWidth;
        const int height = kMirrorInitialHeight;
        const int x = workArea.left + kMirrorInitialOffset;
        const int y = workArea.top + kMirrorInitialOffset;

        mirrorHwnd_ = CreateWindowExW(
            0,
            kMirrorWindowClass,
            L"HDR Corrector Stream Mirror",
            WS_OVERLAPPEDWINDOW,
            x,
            y,
            width,
            height,
            nullptr,
            nullptr,
            instance_,
            this);

        if (!mirrorHwnd_) {
            Log(L"Create mirror window failed: " + LastErrorMessage(GetLastError()));
            return;
        }

        CreateSwapChain();
    }

    void CreateSwapChain() {
        if (!mirrorHwnd_) {
            return;
        }

        auto dxgiDevice = d3dDevice_.as<IDXGIDevice>();

        winrt::com_ptr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(adapter.put());

        winrt::com_ptr<IDXGIFactory2> factory;
        adapter->GetParent(IID_PPV_ARGS(factory.put()));

        RECT client = {};
        GetClientRect(mirrorHwnd_, &client);
        const UINT width = std::max<UINT>(1, client.right - client.left);
        const UINT height = std::max<UINT>(1, client.bottom - client.top);

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kSwapChainBufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        swapChain_ = nullptr;
        renderTargetView_ = nullptr;

        HRESULT hr = factory->CreateSwapChainForHwnd(
            d3dDevice_.get(),
            mirrorHwnd_,
            &desc,
            nullptr,
            nullptr,
            swapChain_.put());
        if (FAILED(hr)) {
            Log(L"CreateSwapChainForHwnd failed: " + HResultMessage(hr));
            return;
        }

        factory->MakeWindowAssociation(mirrorHwnd_, DXGI_MWA_NO_ALT_ENTER);
        CreateRenderTargetView();
    }

    void ResizeSwapChain() {
        if (!swapChain_) {
            return;
        }

        renderTargetView_ = nullptr;
        d3dContext_->OMSetRenderTargets(0, nullptr, nullptr);

        RECT client = {};
        GetClientRect(mirrorHwnd_, &client);
        const UINT width = std::max<UINT>(1, client.right - client.left);
        const UINT height = std::max<UINT>(1, client.bottom - client.top);

        const HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            Log(L"ResizeBuffers failed: " + HResultMessage(hr));
            return;
        }

        CreateRenderTargetView();
    }

    void CreateRenderTargetView() {
        winrt::com_ptr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
        if (FAILED(hr)) {
            Log(L"GetBuffer failed: " + HResultMessage(hr));
            return;
        }

        hr = d3dDevice_->CreateRenderTargetView(backBuffer.get(), nullptr, renderTargetView_.put());
        if (FAILED(hr)) {
            Log(L"CreateRenderTargetView failed: " + HResultMessage(hr));
        }
    }

    void RenderMirror() {
        if (!swapChain_ || !renderTargetView_) {
            return;
        }

        RECT client = {};
        GetClientRect(mirrorHwnd_, &client);
        const UINT width = std::max<UINT>(1, client.right - client.left);
        const UINT height = std::max<UINT>(1, client.bottom - client.top);

        winrt::com_ptr<ID3D11ShaderResourceView> sourceView;
        {
            std::scoped_lock lock(frameMutex_);
            if (!latestTexture_) {
                d3dContext_->ClearRenderTargetView(renderTargetView_.get(), kMirrorWaitingClearColor);
                swapChain_->Present(kPresentSyncInterval, kPresentFlags);
                return;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
            viewDesc.Format = kCaptureFormat;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;

            const HRESULT hr = d3dDevice_->CreateShaderResourceView(latestTexture_.get(), &viewDesc, sourceView.put());
            if (FAILED(hr)) {
                Log(L"CreateShaderResourceView failed: " + HResultMessage(hr));
                return;
            }
        }

        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        d3dContext_->ClearRenderTargetView(renderTargetView_.get(), kMirrorFrameClearColor);
        ID3D11RenderTargetView* renderTarget = renderTargetView_.get();
        d3dContext_->OMSetRenderTargets(1, &renderTarget, nullptr);
        d3dContext_->RSSetViewports(1, &viewport);
        d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext_->VSSetShader(vertexShader_.get(), nullptr, 0);
        d3dContext_->PSSetShader(pixelShader_.get(), nullptr, 0);

        ToneMapConstants constants = {};
        constants.sdrWhiteScale = sdrWhiteScale_;
        d3dContext_->UpdateSubresource(toneMapConstants_.get(), 0, nullptr, &constants, 0, 0);

        ID3D11ShaderResourceView* srv = sourceView.get();
        ID3D11SamplerState* sampler = samplerState_.get();
        ID3D11Buffer* constantBuffer = toneMapConstants_.get();
        d3dContext_->PSSetShaderResources(0, 1, &srv);
        d3dContext_->PSSetSamplers(0, 1, &sampler);
        d3dContext_->PSSetConstantBuffers(0, 1, &constantBuffer);
        // A fullscreen triangle covers the target without a vertex buffer or edge seam.
        d3dContext_->Draw(kFullscreenTriangleVertexCount, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
        swapChain_->Present(kPresentSyncInterval, kPresentFlags);
    }

    bool CopyLatestFrameToCpu(std::vector<uint8_t>& halfRgba, UINT& width, UINT& height, std::wstring& error) {
        winrt::com_ptr<ID3D11Texture2D> staging;

        {
            std::scoped_lock lock(frameMutex_);
            if (!latestTexture_) {
                error = L"No HDR frame has been captured yet.";
                return false;
            }

            D3D11_TEXTURE2D_DESC desc = {};
            latestTexture_->GetDesc(&desc);
            width = desc.Width;
            height = desc.Height;

            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;

            HRESULT hr = d3dDevice_->CreateTexture2D(&desc, nullptr, staging.put());
            if (FAILED(hr)) {
                error = L"Create staging texture failed: " + HResultMessage(hr);
                return false;
            }

            d3dContext_->CopyResource(staging.get(), latestTexture_.get());
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3dContext_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            error = L"Map staging texture failed: " + HResultMessage(hr);
            return false;
        }

        const UINT destinationStride = width * kHalfRgbaBytesPerPixel;
        halfRgba.resize(static_cast<size_t>(destinationStride) * height);
        auto* source = static_cast<const uint8_t*>(mapped.pData);

        for (UINT y = 0; y < height; ++y) {
            memcpy(
                halfRgba.data() + static_cast<size_t>(y) * destinationStride,
                source + static_cast<size_t>(y) * mapped.RowPitch,
                destinationStride);
        }

        d3dContext_->Unmap(staging.get(), 0);
        return true;
    }

    std::vector<uint8_t> ToneMapHalfRgbaToBgra8(const std::vector<uint8_t>& halfRgba, UINT width, UINT height) const {
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * kBgraBytesPerPixel);
        const auto* source = reinterpret_cast<const uint16_t*>(halfRgba.data());
        const float whiteScale = std::max(1.0f, sdrWhiteScale_);

        for (size_t i = 0, pixelCount = static_cast<size_t>(width) * height; i < pixelCount; ++i) {
            const size_t rgbaOffset = i * kHalfRgbaChannelCount;
            const size_t bgraOffset = i * kBgraBytesPerPixel;
            const float r = std::clamp(HalfToFloat(source[rgbaOffset + kRgbaRedIndex]) / whiteScale, 0.0f, 1.0f);
            const float g = std::clamp(HalfToFloat(source[rgbaOffset + kRgbaGreenIndex]) / whiteScale, 0.0f, 1.0f);
            const float b = std::clamp(HalfToFloat(source[rgbaOffset + kRgbaBlueIndex]) / whiteScale, 0.0f, 1.0f);

            bgra[bgraOffset + kBgraBlueIndex] = ToSrgb8(b);
            bgra[bgraOffset + kBgraGreenIndex] = ToSrgb8(g);
            bgra[bgraOffset + kBgraRedIndex] = ToSrgb8(r);
            bgra[bgraOffset + kBgraAlphaIndex] = kOpaqueAlpha;
        }

        return bgra;
    }

    bool SaveCurrentCaptureFrameAsScreenshot(std::wstring& error) {
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> halfRgba;
        if (!CopyLatestFrameToCpu(halfRgba, width, height, error)) {
            error = L"Screenshot capture failed: " + error;
            return false;
        }

        const auto hdrPath = MakeScreenshotPath(L".jxr");
        const UINT halfStride = width * kHalfRgbaBytesPerPixel;
        if (!SaveWicBitmapFromMemory(
                hdrPath,
                GUID_ContainerFormatWmp,
                GUID_WICPixelFormat64bppRGBAHalf,
                width,
                height,
                halfStride,
                halfRgba,
                error)) {
            error = L"HDR JXR save failed: " + error;
            return false;
        }

        const auto sdrPreview = ToneMapHalfRgbaToBgra8(halfRgba, width, height);
        const auto previewPath = hdrPath;
        auto pngPath = previewPath;
        pngPath.replace_extension(L".preview.png");
        const bool previewSaved = SaveWicBitmapFromMemory(
                pngPath,
                GUID_ContainerFormatPng,
                GUID_WICPixelFormat32bppBGRA,
                width,
                height,
                width * kBgraBytesPerPixel,
                sdrPreview,
                error);
        if (!previewSaved) {
            Log(L"Preview PNG save failed: " + error);
        }

        if (!CopyBgraToClipboard(hiddenHwnd_, width, height, sdrPreview, previewSaved ? pngPath : std::filesystem::path(), error)) {
            error = L"Clipboard copy failed: " + error;
            return false;
        }

        return true;
    }

    void FinishTemporaryScreenshotCapture(bool restoreMirrorCapture) {
        if (restoreMirrorCapture) {
            if (StartCapture(selectedMonitor_, false)) {
                RenderMirror();
            } else {
                Log(L"Could not restore stream mirror capture after active window screenshot.");
            }
            return;
        }

        if (!mirrorVisible_) {
            StopCapture();
        }
    }

    HWND ActiveWindowForScreenshot() const {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) {
            return nullptr;
        }

        hwnd = GetAncestor(hwnd, GA_ROOT);
        if (!hwnd || hwnd == hiddenHwnd_ || hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) {
            return nullptr;
        }
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            return nullptr;
        }

        return hwnd;
    }

    void CaptureScreenshot() {
        ShowNotification(kAppName, L"Capturing HDR screenshot...");

        const bool wasCapturing = captureSession_ != nullptr;
        if (!wasCapturing && !StartCapture(selectedMonitor_)) {
            ShowNotification(kAppName, L"Could not start capture. See log for details.");
            return;
        }

        if (!WaitForFirstFrame(wasCapturing ? kExistingCaptureFrameWaitMs : kNewCaptureFrameWaitMs)) {
            Log(L"Timed out waiting for first capture frame.");
            ShowNotification(kAppName, L"Screenshot failed waiting for capture frame.");
            if (!mirrorVisible_) {
                StopCapture();
            }
            return;
        }

        std::wstring error;
        if (!SaveCurrentCaptureFrameAsScreenshot(error)) {
            Log(error);
            ShowNotification(kAppName, L"Screenshot failed. See log for details.");
            if (!mirrorVisible_) {
                StopCapture();
            }
            return;
        }

        ShowNotification(kAppName, L"Screenshot copied to clipboard. HDR .jxr saved.");
        if (!mirrorVisible_) {
            StopCapture();
        }
    }

    void CaptureActiveWindowScreenshot() {
        HWND hwnd = ActiveWindowForScreenshot();
        if (!hwnd) {
            ShowNotification(kAppName, L"No active window to capture.");
            return;
        }

        const bool restoreMirrorCapture = mirrorVisible_;
        ShowNotification(kAppName, L"Capturing active window HDR screenshot...");

        if (!StartWindowCapture(hwnd, false)) {
            if (restoreMirrorCapture) {
                StartCapture(selectedMonitor_, false);
            }
            return;
        }

        if (!WaitForFirstFrame(kNewCaptureFrameWaitMs)) {
            Log(L"Timed out waiting for first active window capture frame.");
            ShowNotification(kAppName, L"Active window screenshot failed waiting for capture frame.");
            FinishTemporaryScreenshotCapture(restoreMirrorCapture);
            return;
        }

        std::wstring error;
        if (!SaveCurrentCaptureFrameAsScreenshot(error)) {
            Log(error);
            ShowNotification(kAppName, L"Active window screenshot failed. See log for details.");
            FinishTemporaryScreenshotCapture(restoreMirrorCapture);
            return;
        }

        ShowNotification(kAppName, L"Active window screenshot copied. HDR .jxr saved.");
        FinishTemporaryScreenshotCapture(restoreMirrorCapture);
    }

    void ToggleStartup() {
        const bool desiredState = !IsStartupEnabled();
        if (SetStartupEnabled(desiredState)) {
            ShowNotification(kAppName, desiredState ? L"Startup enabled." : L"Startup disabled.");
        } else {
            ShowNotification(kAppName, L"Could not update startup setting.");
        }
    }

    void OpenScreenshotsFolder() const {
        const auto folder = ScreenshotDirectory();
        ShellExecuteW(hiddenHwnd_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void Shutdown() {
        UnregisterHotKey(hiddenHwnd_, kHotkeyScreenshot);
        UnregisterHotKey(hiddenHwnd_, kHotkeyWindowScreenshot);
        UnregisterHotKey(hiddenHwnd_, kHotkeyMirror);
        StopCapture();
        audioRelay_.Stop();

        if (mirrorHwnd_) {
            DestroyWindow(mirrorHwnd_);
            mirrorHwnd_ = nullptr;
        }

        if (frameReadyEvent_) {
            CloseHandle(frameReadyEvent_);
            frameReadyEvent_ = nullptr;
        }

        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hiddenHwnd_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }

    HINSTANCE instance_ = nullptr;
    HWND hiddenHwnd_ = nullptr;
    HWND mirrorHwnd_ = nullptr;
    HMONITOR selectedMonitor_ = nullptr;
    HANDLE frameReadyEvent_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool mirrorVisible_ = false;
    bool audioRelayEnabled_ = true;
    bool packageIdentityChecked_ = false;
    bool hasPackageIdentity_ = false;
    bool borderlessCaptureAccessChecked_ = false;
    bool borderlessCaptureAllowed_ = false;
    AudioRelay audioRelay_;

    winrt::com_ptr<ID3D11Device> d3dDevice_;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext_;
    IDirect3DDevice direct3DDevice_ = nullptr;

    GraphicsCaptureItem captureItem_ = nullptr;
    Direct3D11CaptureFramePool framePool_ = nullptr;
    GraphicsCaptureSession captureSession_ = nullptr;
    winrt::event_token frameArrivedToken_ = {};
    winrt::Windows::Graphics::SizeInt32 captureSize_ = {};
    std::wstring capturedMonitorName_;

    std::mutex frameMutex_;
    winrt::com_ptr<ID3D11Texture2D> latestTexture_;
    UINT latestWidth_ = 0;
    UINT latestHeight_ = 0;

    winrt::com_ptr<IDXGISwapChain1> swapChain_;
    winrt::com_ptr<ID3D11RenderTargetView> renderTargetView_;
    winrt::com_ptr<ID3D11VertexShader> vertexShader_;
    winrt::com_ptr<ID3D11PixelShader> pixelShader_;
    winrt::com_ptr<ID3D11SamplerState> samplerState_;
    winrt::com_ptr<ID3D11Buffer> toneMapConstants_;
    float sdrWhiteScale_ = 1.0f;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) {
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Log(L"HDR Corrector starting.");

    int result = 1;
    {
        App app;
        result = app.Initialize(instance) ? app.Run() : 1;
    }

    Log(L"HDR Corrector exiting.");
    winrt::uninit_apartment();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}
