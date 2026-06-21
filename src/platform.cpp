#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "platform.h"

#include <knownfolders.h>
#include <shlobj_core.h>

#include <fstream>
#include <iterator>
#include <sstream>

namespace {

std::filesystem::path KnownFolderPath(REFKNOWNFOLDERID folderId) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_CREATE, nullptr, &raw)) && raw) {
        std::filesystem::path path(raw);
        CoTaskMemFree(raw);
        return path;
    }
    return {};
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

}  // namespace

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

std::filesystem::path MakeScreenshotPath(std::wstring_view extension) {
    return ScreenshotDirectory() / (L"HDRCorrector_" + TimestampForFile() + std::wstring(extension));
}

void Log(std::wstring_view message) {
    const auto path = AppDataDirectory() / L"hdr-corrector.log";
    std::wofstream file(path, std::ios::app);
    if (file) {
        file << L"[" << TimestampForLog() << L"] " << message << L"\n";
    }
}
