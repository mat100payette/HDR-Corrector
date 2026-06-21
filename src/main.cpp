#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <knownfolders.h>
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

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
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

constexpr UINT kCmdScreenshot = 200;
constexpr UINT kCmdMirror = 201;
constexpr UINT kCmdSelectMonitor = 202;
constexpr UINT kCmdStartup = 203;
constexpr UINT kCmdOpenFolder = 204;
constexpr UINT kCmdExit = 205;

constexpr DXGI_FORMAT kCaptureFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DirectXPixelFormat kFramePoolFormat = DirectXPixelFormat::R16G16B16A16Float;

const char kFullscreenTriangleShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);
SamplerState sourceSampler : register(s0);

cbuffer ToneMapConstants : register(b0) {
    float sdrWhiteScale;
    float3 padding;
};

struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
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
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(color, 1.0 / 2.4) - 0.055;
    return float3(
        color.r <= 0.0031308 ? low.r : high.r,
        color.g <= 0.0031308 ? low.g : high.g,
        color.b <= 0.0031308 ? low.b : high.b);
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

std::wstring LastErrorMessage(DWORD error) {
    if (error == ERROR_SUCCESS) {
        return L"success";
    }

    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = length && buffer ? buffer : L"unknown error";
    if (buffer) {
        LocalFree(buffer);
    }

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }

    std::wstringstream stream;
    stream << message << L" (" << error << L")";
    return stream.str();
}

std::wstring HResultMessage(HRESULT hr) {
    return LastErrorMessage(static_cast<DWORD>(hr));
}

std::filesystem::path KnownFolderPath(REFKNOWNFOLDERID folderId) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        std::filesystem::path path(raw);
        CoTaskMemFree(raw);
        return path;
    }
    return {};
}

std::filesystem::path AppDataDirectory() {
    auto base = KnownFolderPath(FOLDERID_LocalAppData);
    if (base.empty()) {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
        base = temp;
    }

    auto directory = base / L"HDRCorrector";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

std::filesystem::path ScreenshotDirectory() {
    auto base = KnownFolderPath(FOLDERID_Pictures);
    if (base.empty()) {
        base = AppDataDirectory();
    }

    auto directory = base / L"HDRCorrector";
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    return directory;
}

std::wstring TimestampForLog() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    wchar_t buffer[64] = {};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

std::wstring TimestampForFile() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    wchar_t buffer[64] = {};
    swprintf_s(
        buffer,
        L"%04u%02u%02u_%02u%02u%02u_%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

void Log(std::wstring_view message) {
    const auto path = AppDataDirectory() / L"hdr-corrector.log";
    std::wofstream file(path, std::ios::app);
    if (file) {
        file << L"[" << TimestampForLog() << L"] " << message << L"\n";
    }
}

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

    wchar_t value[2048] = {};
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

std::filesystem::path MakeScreenshotPath(std::wstring_view extension) {
    return ScreenshotDirectory() / (L"HDRCorrector_" + TimestampForFile() + std::wstring(extension));
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

std::wstring MonitorName(HMONITOR monitor) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        return info.szDevice;
    }
    return L"selected monitor";
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
            return std::max(1.0f, static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0f);
        }

        Log(L"DisplayConfigGetDeviceInfo(GET_SDR_WHITE_LEVEL) failed: " + LastErrorMessage(result));
        return 1.0f;
    }

    return 1.0f;
}

float HalfToFloat(uint16_t value) {
    const uint32_t sign = (value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x03ffu;
    uint32_t bits = 0;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03ffu;
            exponent = exponent + (127 - 15);
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        exponent = exponent + (127 - 15);
        bits = sign | (exponent << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

uint8_t ToSrgb8(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    const float gammaEncoded = linear <= 0.0031308f
        ? linear * 12.92f
        : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(gammaEncoded * 255.0f));
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

HGLOBAL DuplicateGlobalMemory(HGLOBAL source) {
    const SIZE_T size = GlobalSize(source);
    if (size == 0) {
        return nullptr;
    }

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) {
        return nullptr;
    }

    const void* sourceData = GlobalLock(source);
    void* copyData = GlobalLock(copy);
    if (!sourceData || !copyData) {
        if (sourceData) {
            GlobalUnlock(source);
        }
        if (copyData) {
            GlobalUnlock(copy);
        }
        GlobalFree(copy);
        return nullptr;
    }

    memcpy(copyData, sourceData, size);
    GlobalUnlock(source);
    GlobalUnlock(copy);
    return copy;
}

HGLOBAL EncodePngToGlobalMemory(UINT width, UINT height, UINT stride, const std::vector<uint8_t>& pixels, std::wstring& error) {
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
        return nullptr;
    }

    winrt::com_ptr<IWICBitmap> source;
    hr = factory->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat32bppBGRA,
        stride,
        static_cast<UINT>(pixels.size()),
        const_cast<BYTE*>(pixels.data()),
        source.put());
    if (FAILED(hr)) {
        error = L"CreateBitmapFromMemory for PNG clipboard data failed: " + HResultMessage(hr);
        return nullptr;
    }

    winrt::com_ptr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
    if (FAILED(hr)) {
        error = L"CreateStreamOnHGlobal failed: " + HResultMessage(hr);
        return nullptr;
    }

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put());
    if (FAILED(hr)) {
        error = L"Create PNG encoder failed: " + HResultMessage(hr);
        return nullptr;
    }

    hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        error = L"PNG encoder initialization failed: " + HResultMessage(hr);
        return nullptr;
    }

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> propertyBag;
    hr = encoder->CreateNewFrame(frame.put(), propertyBag.put());
    if (FAILED(hr)) {
        error = L"Create PNG frame failed: " + HResultMessage(hr);
        return nullptr;
    }

    hr = frame->Initialize(propertyBag.get());
    if (FAILED(hr)) {
        error = L"PNG frame initialization failed: " + HResultMessage(hr);
        return nullptr;
    }

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) {
        error = L"PNG SetSize failed: " + HResultMessage(hr);
        return nullptr;
    }

    WICPixelFormatGUID acceptedFormat = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&acceptedFormat);
    if (FAILED(hr)) {
        error = L"PNG SetPixelFormat failed: " + HResultMessage(hr);
        return nullptr;
    }

    if (IsEqualGUID(acceptedFormat, GUID_WICPixelFormat32bppBGRA)) {
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
        error = L"PNG WriteSource failed: " + HResultMessage(hr);
        return nullptr;
    }

    hr = frame->Commit();
    if (FAILED(hr)) {
        error = L"PNG frame commit failed: " + HResultMessage(hr);
        return nullptr;
    }

    hr = encoder->Commit();
    if (FAILED(hr)) {
        error = L"PNG commit failed: " + HResultMessage(hr);
        return nullptr;
    }

    HGLOBAL memory = nullptr;
    hr = GetHGlobalFromStream(stream.get(), &memory);
    if (FAILED(hr) || !memory) {
        error = L"GetHGlobalFromStream failed: " + HResultMessage(hr);
        return nullptr;
    }

    HGLOBAL clipboardMemory = DuplicateGlobalMemory(memory);
    if (!clipboardMemory) {
        error = L"Could not copy encoded PNG clipboard data.";
        return nullptr;
    }

    return clipboardMemory;
}

HGLOBAL CreateFileDropData(const std::filesystem::path& path) {
    if (path.empty()) {
        return nullptr;
    }

    const std::wstring absolutePath = std::filesystem::absolute(path).wstring();
    const size_t pathCharacters = absolutePath.size() + 2;
    const size_t totalBytes = sizeof(DROPFILES) + pathCharacters * sizeof(wchar_t);

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (!memory) {
        return nullptr;
    }

    auto* data = static_cast<uint8_t*>(GlobalLock(memory));
    if (!data) {
        GlobalFree(memory);
        return nullptr;
    }

    auto* dropFiles = reinterpret_cast<DROPFILES*>(data);
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;

    auto* fileList = reinterpret_cast<wchar_t*>(data + sizeof(DROPFILES));
    memcpy(fileList, absolutePath.c_str(), (absolutePath.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    return memory;
}

HGLOBAL CreatePreferredDropEffectData() {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!memory) {
        return nullptr;
    }

    auto* effect = static_cast<DWORD*>(GlobalLock(memory));
    if (!effect) {
        GlobalFree(memory);
        return nullptr;
    }

    *effect = DROPEFFECT_COPY;
    GlobalUnlock(memory);
    return memory;
}

bool CopyBgraToClipboard(
    HWND hwnd,
    UINT width,
    UINT height,
    const std::vector<uint8_t>& bgra,
    const std::filesystem::path& imageFilePath,
    std::wstring& error) {
    const size_t pixelBytes = static_cast<size_t>(width) * height * 4;
    if (width == 0 || height == 0 || bgra.size() < pixelBytes) {
        error = L"Invalid clipboard bitmap dimensions.";
        return false;
    }

    const auto makeDib = [&]() -> HGLOBAL {
        const size_t totalBytes = sizeof(BITMAPINFOHEADER) + pixelBytes;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
        if (!memory) {
            return nullptr;
        }

        auto* data = static_cast<uint8_t*>(GlobalLock(memory));
        if (!data) {
            GlobalFree(memory);
            return nullptr;
        }

        auto* header = reinterpret_cast<BITMAPINFOHEADER*>(data);
        header->biSize = sizeof(BITMAPINFOHEADER);
        header->biWidth = static_cast<LONG>(width);
        header->biHeight = -static_cast<LONG>(height);
        header->biPlanes = 1;
        header->biBitCount = 32;
        header->biCompression = BI_RGB;
        header->biSizeImage = static_cast<DWORD>(pixelBytes);

        memcpy(data + sizeof(BITMAPINFOHEADER), bgra.data(), pixelBytes);
        GlobalUnlock(memory);
        return memory;
    };

    const auto makeDibV5 = [&]() -> HGLOBAL {
        const size_t totalBytes = sizeof(BITMAPV5HEADER) + pixelBytes;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, totalBytes);
        if (!memory) {
            return nullptr;
        }

        auto* data = static_cast<uint8_t*>(GlobalLock(memory));
        if (!data) {
            GlobalFree(memory);
            return nullptr;
        }

        auto* header = reinterpret_cast<BITMAPV5HEADER*>(data);
        header->bV5Size = sizeof(BITMAPV5HEADER);
        header->bV5Width = static_cast<LONG>(width);
        header->bV5Height = -static_cast<LONG>(height);
        header->bV5Planes = 1;
        header->bV5BitCount = 32;
        header->bV5Compression = BI_BITFIELDS;
        header->bV5SizeImage = static_cast<DWORD>(pixelBytes);
        header->bV5RedMask = 0x00ff0000;
        header->bV5GreenMask = 0x0000ff00;
        header->bV5BlueMask = 0x000000ff;
        header->bV5AlphaMask = 0xff000000;
        header->bV5CSType = LCS_sRGB;
        header->bV5Intent = LCS_GM_IMAGES;

        memcpy(data + sizeof(BITMAPV5HEADER), bgra.data(), pixelBytes);
        GlobalUnlock(memory);
        return memory;
    };

    HGLOBAL dib = makeDib();
    HGLOBAL dibV5 = makeDibV5();
    HGLOBAL png = EncodePngToGlobalMemory(width, height, width * 4, bgra, error);
    HGLOBAL imagePng = png ? DuplicateGlobalMemory(png) : nullptr;
    if (png && !imagePng) {
        Log(L"Could not duplicate PNG clipboard data for image/png format.");
    } else if (!png) {
        Log(L"PNG clipboard data was not created: " + error);
    }

    HGLOBAL fileDrop = nullptr;
    HGLOBAL dropEffect = nullptr;
    if (!imageFilePath.empty() && std::filesystem::exists(imageFilePath)) {
        fileDrop = CreateFileDropData(imageFilePath);
        dropEffect = CreatePreferredDropEffectData();
        if (!fileDrop) {
            Log(L"Could not create CF_HDROP clipboard data for " + imageFilePath.wstring());
        }
    }

    void* bits = nullptr;
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        memcpy(bits, bgra.data(), pixelBytes);
    }

    if (!OpenClipboard(hwnd)) {
        error = L"OpenClipboard failed: " + LastErrorMessage(GetLastError());
        if (dib) {
            GlobalFree(dib);
        }
        if (dibV5) {
            GlobalFree(dibV5);
        }
        if (png) {
            GlobalFree(png);
        }
        if (imagePng) {
            GlobalFree(imagePng);
        }
        if (fileDrop) {
            GlobalFree(fileDrop);
        }
        if (dropEffect) {
            GlobalFree(dropEffect);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return false;
    }

    EmptyClipboard();

    bool copied = false;
    if (fileDrop) {
        if (SetClipboardData(CF_HDROP, fileDrop)) {
            copied = true;
            fileDrop = nullptr;
        } else {
            Log(L"SetClipboardData CF_HDROP failed: " + LastErrorMessage(GetLastError()));
        }
    }

    const UINT preferredDropEffectFormat = RegisterClipboardFormatW(L"Preferred DropEffect");
    if (preferredDropEffectFormat != 0 && dropEffect) {
        if (SetClipboardData(preferredDropEffectFormat, dropEffect)) {
            dropEffect = nullptr;
        } else {
            Log(L"SetClipboardData Preferred DropEffect failed: " + LastErrorMessage(GetLastError()));
        }
    }

    const UINT pngFormat = RegisterClipboardFormatW(L"PNG");
    if (pngFormat != 0 && png) {
        if (SetClipboardData(pngFormat, png)) {
            copied = true;
            png = nullptr;
        } else {
            Log(L"SetClipboardData PNG failed: " + LastErrorMessage(GetLastError()));
        }
    }

    const UINT imagePngFormat = RegisterClipboardFormatW(L"image/png");
    if (imagePngFormat != 0 && imagePng) {
        if (SetClipboardData(imagePngFormat, imagePng)) {
            copied = true;
            imagePng = nullptr;
        } else {
            Log(L"SetClipboardData image/png failed: " + LastErrorMessage(GetLastError()));
        }
    }

    if (dibV5) {
        if (SetClipboardData(CF_DIBV5, dibV5)) {
            copied = true;
            dibV5 = nullptr;
        } else {
            Log(L"SetClipboardData CF_DIBV5 failed: " + LastErrorMessage(GetLastError()));
        }
    }

    if (dib) {
        if (SetClipboardData(CF_DIB, dib)) {
            copied = true;
            dib = nullptr;
        } else {
            Log(L"SetClipboardData CF_DIB failed: " + LastErrorMessage(GetLastError()));
        }
    }

    if (bitmap) {
        if (SetClipboardData(CF_BITMAP, bitmap)) {
            copied = true;
            bitmap = nullptr;
        } else {
            Log(L"SetClipboardData CF_BITMAP failed: " + LastErrorMessage(GetLastError()));
        }
    }

    if (!copied) {
        error = L"SetClipboardData failed for all bitmap formats.";
        CloseClipboard();
        if (dib) {
            GlobalFree(dib);
        }
        if (dibV5) {
            GlobalFree(dibV5);
        }
        if (png) {
            GlobalFree(png);
        }
        if (imagePng) {
            GlobalFree(imagePng);
        }
        if (fileDrop) {
            GlobalFree(fileDrop);
        }
        if (dropEffect) {
            GlobalFree(dropEffect);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return false;
    }

    CloseClipboard();
    if (dib) {
        GlobalFree(dib);
    }
    if (dibV5) {
        GlobalFree(dibV5);
    }
    if (png) {
        GlobalFree(png);
    }
    if (imagePng) {
        GlobalFree(imagePng);
    }
    if (fileDrop) {
        GlobalFree(fileDrop);
    }
    if (dropEffect) {
        GlobalFree(dropEffect);
    }
    if (bitmap) {
        DeleteObject(bitmap);
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

        if (!RegisterClassExW(&mirrorClass)) {
            Log(L"RegisterClassExW mirror failed: " + LastErrorMessage(GetLastError()));
            return false;
        }

        return true;
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

        if (!RegisterHotKey(hiddenHwnd_, kHotkeyMirror, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H')) {
            Log(L"Could not register Ctrl+Alt+H: " + LastErrorMessage(GetLastError()));
        }
    }

    void AddTrayIcon() {
        NOTIFYICONDATAW data = {};
        data.cbSize = sizeof(data);
        data.hWnd = hiddenHwnd_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
        data.uFlags = NIF_TIP;
        CopyTruncated(data.szTip, TrayTooltip());
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    std::wstring TrayTooltip() const {
        return mirrorVisible_ ? L"HDR Corrector - mirror visible" : L"HDR Corrector";
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
        AppendMenuW(
            menu,
            MF_STRING | (mirrorVisible_ ? MF_CHECKED : MF_UNCHECKED),
            kCmdMirror,
            L"Show stream mirror\tCtrl+Alt+H");
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
        case kCmdMirror:
            ToggleMirrorWindow();
            break;
        case kCmdSelectMonitor:
            SelectMonitorUnderCursor();
            break;
        case kCmdStartup:
            ToggleStartup();
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
        case kHotkeyMirror:
            ToggleMirrorWindow();
            break;
        default:
            break;
        }
    }

    bool StartCapture(HMONITOR monitor) {
        StopCapture();

        try {
            if (frameReadyEvent_) {
                ResetEvent(frameReadyEvent_);
            }

            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            GraphicsCaptureItem item = nullptr;
            winrt::check_hresult(interop->CreateForMonitor(
                monitor,
                winrt::guid_of<GraphicsCaptureItem>(),
                winrt::put_abi(item)));

            captureItem_ = item;
            captureSize_ = item.Size();
            capturedMonitorName_ = MonitorName(monitor);
            sdrWhiteScale_ = SdrWhiteScaleForMonitor(monitor);
            framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3DDevice_,
                kFramePoolFormat,
                2,
                captureSize_);
            frameArrivedToken_ = framePool_.FrameArrived({this, &App::OnFrameArrived});
            captureSession_ = framePool_.CreateCaptureSession(captureItem_);
            captureSession_.IsCursorCaptureEnabled(true);
            captureSession_.StartCapture();

            {
                std::scoped_lock lock(frameMutex_);
                latestTexture_ = nullptr;
                latestWidth_ = 0;
                latestHeight_ = 0;
            }

            std::wstringstream stream;
            stream << L"Capturing " << capturedMonitorName_ << L" at " << captureSize_.Width << L"x" << captureSize_.Height
                   << L", SDR white scale " << sdrWhiteScale_;
            ShowNotification(kAppName, stream.str());
            return true;
        } catch (const winrt::hresult_error& error) {
            Log(L"StartCapture failed: " + std::wstring(error.message()));
            ShowNotification(kAppName, L"Could not start HDR capture. See log for details.");
            return false;
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
                framePool_.Recreate(direct3DDevice_, kFramePoolFormat, 2, captureSize_);
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
        } else {
            StopCapture();
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
        const int width = 1280;
        const int height = 720;
        const int x = workArea.left + 80;
        const int y = workArea.top + 80;

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
        desc.BufferCount = 2;
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
                const float clearColor[] = {0.02f, 0.02f, 0.02f, 1.0f};
                d3dContext_->ClearRenderTargetView(renderTargetView_.get(), clearColor);
                swapChain_->Present(1, 0);
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

        const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
        d3dContext_->ClearRenderTargetView(renderTargetView_.get(), clearColor);
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
        d3dContext_->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
        swapChain_->Present(1, 0);
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

        const UINT destinationStride = width * 8;
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
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        const auto* source = reinterpret_cast<const uint16_t*>(halfRgba.data());
        const float whiteScale = std::max(1.0f, sdrWhiteScale_);

        for (size_t i = 0, pixelCount = static_cast<size_t>(width) * height; i < pixelCount; ++i) {
            const float r = std::clamp(HalfToFloat(source[i * 4 + 0]) / whiteScale, 0.0f, 1.0f);
            const float g = std::clamp(HalfToFloat(source[i * 4 + 1]) / whiteScale, 0.0f, 1.0f);
            const float b = std::clamp(HalfToFloat(source[i * 4 + 2]) / whiteScale, 0.0f, 1.0f);

            bgra[i * 4 + 0] = ToSrgb8(b);
            bgra[i * 4 + 1] = ToSrgb8(g);
            bgra[i * 4 + 2] = ToSrgb8(r);
            bgra[i * 4 + 3] = 255;
        }

        return bgra;
    }

    void CaptureScreenshot() {
        ShowNotification(kAppName, L"Capturing HDR screenshot...");

        const bool wasCapturing = captureSession_ != nullptr;
        if (!wasCapturing && !StartCapture(selectedMonitor_)) {
            ShowNotification(kAppName, L"Could not start capture. See log for details.");
            return;
        }

        if (!WaitForFirstFrame(wasCapturing ? 500 : 2000)) {
            Log(L"Timed out waiting for first capture frame.");
            ShowNotification(kAppName, L"Screenshot failed waiting for capture frame.");
            if (!mirrorVisible_) {
                StopCapture();
            }
            return;
        }

        std::wstring error;
        UINT width = 0;
        UINT height = 0;
        std::vector<uint8_t> halfRgba;
        if (!CopyLatestFrameToCpu(halfRgba, width, height, error)) {
            Log(L"Screenshot capture failed: " + error);
            ShowNotification(kAppName, L"Screenshot failed. See log for details.");
            if (!mirrorVisible_) {
                StopCapture();
            }
            return;
        }

        const auto hdrPath = MakeScreenshotPath(L".jxr");
        const UINT halfStride = width * 8;
        if (!SaveWicBitmapFromMemory(
                hdrPath,
                GUID_ContainerFormatWmp,
                GUID_WICPixelFormat64bppRGBAHalf,
                width,
                height,
                halfStride,
                halfRgba,
                error)) {
            Log(L"HDR JXR save failed: " + error);
            ShowNotification(kAppName, L"HDR screenshot save failed. See log for details.");
            if (!mirrorVisible_) {
                StopCapture();
            }
            return;
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
                width * 4,
                sdrPreview,
                error);
        if (!previewSaved) {
            Log(L"Preview PNG save failed: " + error);
        }

        if (!CopyBgraToClipboard(hiddenHwnd_, width, height, sdrPreview, previewSaved ? pngPath : std::filesystem::path(), error)) {
            Log(L"Clipboard copy failed: " + error);
            ShowNotification(kAppName, L"HDR screenshot saved. Clipboard preview failed.");
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
        UnregisterHotKey(hiddenHwnd_, kHotkeyMirror);
        StopCapture();

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
