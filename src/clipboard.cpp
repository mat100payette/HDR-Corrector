#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "clipboard.h"

#include "platform.h"

#include <objidl.h>
#include <shellapi.h>
#include <shlobj_core.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <cstdint>
#include <cstring>

namespace {

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

}  // namespace

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
