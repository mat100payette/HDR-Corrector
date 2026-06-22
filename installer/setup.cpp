#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <appmodel.h>
#include <commctrl.h>
#include <cryptuiapi.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"
#include "setup_version.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"HDRCorrectorSetupWindow";
constexpr wchar_t kPackageName[] = L"mat100payette.HDRCorrector";
constexpr wchar_t kApplicationId[] = L"HDRCorrector";
constexpr UINT kInstallStatusMessage = WM_APP + 1;
constexpr UINT kInstallCompleteMessage = WM_APP + 2;
constexpr UINT kInstallProgressMessage = WM_APP + 3;

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
HWND gStatusLabel = nullptr;
HWND gDetailsLabel = nullptr;
HWND gProgress = nullptr;
HWND gPrimaryButton = nullptr;
HWND gCloseButton = nullptr;
HFONT gTitleFont = nullptr;
HFONT gBodyFont = nullptr;
std::atomic_bool gInstalling = false;
bool gSetupCompleted = false;
std::wstring gInstalledAppUserModelId;

struct ResourceBytes {
    const BYTE* data = nullptr;
    DWORD size = 0;
};

struct TempFiles {
    std::wstring directory;
    std::wstring msixPath;
    std::wstring certificatePath;
    bool keepDirectory = false;

    ~TempFiles() {
        if (keepDirectory) {
            return;
        }
        if (!certificatePath.empty()) {
            DeleteFileW(certificatePath.c_str());
        }
        if (!msixPath.empty()) {
            DeleteFileW(msixPath.c_str());
        }
        if (!directory.empty()) {
            RemoveDirectoryW(directory.c_str());
        }
    }
};

std::wstring FormatHresult(HRESULT result) {
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);

    std::wstringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    if (length > 0 && message) {
        std::wstring text(message, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
            text.pop_back();
        }
        stream << L" - " << text;
        LocalFree(message);
    }

    return stream.str();
}

std::wstring LastErrorMessage(DWORD error = GetLastError()) {
    return FormatHresult(HRESULT_FROM_WIN32(error));
}

[[noreturn]] void ThrowWindowsError(const wchar_t* action, DWORD error = GetLastError()) {
    throw std::runtime_error(winrt::to_string(std::wstring(action) + L": " + LastErrorMessage(error)));
}

[[noreturn]] void ThrowHresult(const wchar_t* action, HRESULT result, const std::wstring& detail = L"") {
    std::wstring message = std::wstring(action) + L": " + FormatHresult(result);
    if (!detail.empty()) {
        message += L"\r\n\r\n" + detail;
    }
    throw std::runtime_error(winrt::to_string(message));
}

std::wstring ExceptionMessage() {
    try {
        throw;
    } catch (const winrt::hresult_error& error) {
        return std::wstring(error.message());
    } catch (const std::exception& error) {
        return winrt::to_hstring(error.what()).c_str();
    } catch (...) {
        return L"Unexpected installer error.";
    }
}

ResourceBytes FindResourceBytes(WORD id) {
    HRSRC resource = FindResourceW(gInstance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) {
        return {};
    }

    HGLOBAL loaded = LoadResource(gInstance, resource);
    if (!loaded) {
        ThrowWindowsError(L"Could not load embedded installer resource");
    }

    const DWORD size = SizeofResource(gInstance, resource);
    const auto* data = static_cast<const BYTE*>(LockResource(loaded));
    if (!data || size == 0) {
        throw std::runtime_error("The embedded installer resource is empty.");
    }

    return {data, size};
}

void WriteBytesToFile(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ThrowWindowsError(L"Could not create installer file");
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(file);

    if (!ok || written != size) {
        ThrowWindowsError(L"Could not write installer file", ok ? ERROR_WRITE_FAULT : error);
    }
}

std::wstring CreateTempDirectory() {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath) == 0) {
        ThrowWindowsError(L"Could not find the temporary folder");
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        std::wstringstream name;
        name << tempPath << L"HDRCorrectorSetup-" << GetCurrentProcessId() << L"-" << GetTickCount64() << L"-" << attempt;
        std::wstring directory = name.str();
        if (CreateDirectoryW(directory.c_str(), nullptr)) {
            return directory;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            ThrowWindowsError(L"Could not create a temporary installer folder");
        }
    }

    throw std::runtime_error("Could not create a unique temporary installer folder.");
}

TempFiles ExtractEmbeddedPackage() {
    TempFiles files;
    files.directory = CreateTempDirectory();
    files.msixPath = files.directory + L"\\HDRCorrector.msix";
    files.certificatePath = files.directory + L"\\HDRCorrector.cer";

    const ResourceBytes msix = FindResourceBytes(IDR_SETUP_MSIX);
    if (!msix.data) {
        throw std::runtime_error("The installer does not contain an MSIX package.");
    }
    WriteBytesToFile(files.msixPath, msix.data, msix.size);

    const ResourceBytes certificate = FindResourceBytes(IDR_SETUP_CERTIFICATE);
    if (certificate.data) {
        WriteBytesToFile(files.certificatePath, certificate.data, certificate.size);
    } else {
        files.certificatePath.clear();
    }

    return files;
}

void TrustCertificateForCurrentUser(const TempFiles& files) {
    if (files.certificatePath.empty()) {
        return;
    }

    const ResourceBytes certificate = FindResourceBytes(IDR_SETUP_CERTIFICATE);
    if (!certificate.data) {
        return;
    }

    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        0,
        CERT_SYSTEM_STORE_CURRENT_USER,
        L"TrustedPeople");
    if (!store) {
        ThrowWindowsError(L"Could not open the current user's Trusted People certificate store");
    }

    const BOOL ok = CertAddEncodedCertificateToStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        certificate.data,
        certificate.size,
        CERT_STORE_ADD_REPLACE_EXISTING,
        nullptr);
    const DWORD error = GetLastError();
    CertCloseStore(store, 0);

    if (!ok) {
        ThrowWindowsError(L"Could not trust the local package signing certificate", error);
    }
}

std::wstring FileUriFromPath(const std::wstring& path) {
    wchar_t uri[4096] = {};
    DWORD length = static_cast<DWORD>(std::size(uri));
    const HRESULT result = UrlCreateFromPathW(path.c_str(), uri, &length, 0);
    if (FAILED(result)) {
        ThrowHresult(L"Could not create a package URI", result);
    }
    return uri;
}

uint64_t VersionValue(const winrt::Windows::ApplicationModel::PackageVersion& version) {
    return (static_cast<uint64_t>(version.Major) << 48) |
        (static_cast<uint64_t>(version.Minor) << 32) |
        (static_cast<uint64_t>(version.Build) << 16) |
        static_cast<uint64_t>(version.Revision);
}

uint64_t InstallerVersionValue() {
    return (static_cast<uint64_t>(HDRCORRECTOR_INSTALLER_VERSION_MAJOR) << 48) |
        (static_cast<uint64_t>(HDRCORRECTOR_INSTALLER_VERSION_MINOR) << 32) |
        (static_cast<uint64_t>(HDRCORRECTOR_INSTALLER_VERSION_PATCH) << 16) |
        static_cast<uint64_t>(HDRCORRECTOR_INSTALLER_VERSION_BUILD);
}

std::optional<winrt::Windows::ApplicationModel::Package> FindInstalledPackage(
    const winrt::Windows::Management::Deployment::PackageManager& manager) {
    for (const auto& package : manager.FindPackagesForUser(L"")) {
        if (std::wstring(package.Id().Name().c_str()) == kPackageName) {
            return package;
        }
    }
    return std::nullopt;
}

std::wstring AppUserModelIdForPackage(const winrt::Windows::ApplicationModel::Package& package) {
    return std::wstring(package.Id().FamilyName().c_str()) + L"!" + kApplicationId;
}

std::wstring InstallMsixPackage(const std::wstring& msixPath, HWND window) {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;

        winrt::Windows::Management::Deployment::PackageManager manager;
        const auto existingPackage = FindInstalledPackage(manager);
        const uint64_t installerVersion = InstallerVersionValue();

        if (existingPackage && VersionValue(existingPackage->Id().Version()) >= installerVersion) {
            winrt::uninit_apartment();
            return AppUserModelIdForPackage(*existingPackage);
        }

        const auto packageUri = winrt::Windows::Foundation::Uri(FileUriFromPath(msixPath));
        auto dependencies = winrt::single_threaded_vector<winrt::Windows::Foundation::Uri>();
        const auto options = winrt::Windows::Management::Deployment::DeploymentOptions::ForceTargetApplicationShutdown;

        auto operation = existingPackage
            ? manager.UpdatePackageAsync(packageUri, dependencies.GetView(), options)
            : manager.AddPackageAsync(packageUri, dependencies.GetView(), options);

        operation.Progress([window](auto const&, const winrt::Windows::Management::Deployment::DeploymentProgress& progress) {
            PostMessageW(window, kInstallProgressMessage, progress.percentage, 0);
        });

        const auto result = operation.get();
        const HRESULT extendedError = static_cast<HRESULT>(result.ExtendedErrorCode());
        if (FAILED(extendedError)) {
            ThrowHresult(L"Windows could not install HDR Corrector", extendedError, std::wstring(result.ErrorText().c_str()));
        }

        const auto installedPackage = FindInstalledPackage(manager);
        if (!installedPackage) {
            throw std::runtime_error("Windows reported success, but the HDR Corrector package was not found afterward.");
        }

        const std::wstring appUserModelId = AppUserModelIdForPackage(*installedPackage);
        if (apartmentInitialized) {
            winrt::uninit_apartment();
        }
        return appUserModelId;
    } catch (...) {
        if (apartmentInitialized) {
            winrt::uninit_apartment();
        }
        throw;
    }
}

bool OpenMsixWithWindowsInstaller(const std::wstring& msixPath) {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", msixPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void LaunchInstalledApp() {
    if (gInstalledAppUserModelId.empty()) {
        return;
    }

    winrt::com_ptr<IApplicationActivationManager> activationManager;
    HRESULT result = CoCreateInstance(
        CLSID_ApplicationActivationManager,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(activationManager.put()));
    if (FAILED(result)) {
        ThrowHresult(L"Could not prepare app launch", result);
    }

    DWORD processId = 0;
    result = activationManager->ActivateApplication(gInstalledAppUserModelId.c_str(), nullptr, AO_NONE, &processId);
    if (FAILED(result)) {
        ThrowHresult(L"Could not launch HDR Corrector", result);
    }
}

void PostStatus(HWND window, const std::wstring& text) {
    PostMessageW(window, kInstallStatusMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void PostComplete(HWND window, bool success, const std::wstring& text) {
    PostMessageW(window, kInstallCompleteMessage, success ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void InstallWorker(HWND window) {
    try {
        PostStatus(window, L"Preparing installer files...");
        TempFiles files = ExtractEmbeddedPackage();

        PostStatus(window, L"Trusting the package certificate...");
        TrustCertificateForCurrentUser(files);

        PostStatus(window, L"Installing HDR Corrector for this Windows user...");
        gInstalledAppUserModelId = InstallMsixPackage(files.msixPath, window);
        PostComplete(window, true, L"HDR Corrector is installed. You can launch it now or close this installer.");
    } catch (...) {
        const std::wstring directInstallError = ExceptionMessage();
        try {
            TempFiles fallbackFiles = ExtractEmbeddedPackage();
            TrustCertificateForCurrentUser(fallbackFiles);
            if (OpenMsixWithWindowsInstaller(fallbackFiles.msixPath)) {
                fallbackFiles.keepDirectory = true;
                PostComplete(window, true, L"Windows App Installer opened. Click Install there to finish installing HDR Corrector.");
                return;
            }
        } catch (...) {
            const std::wstring fallbackError = ExceptionMessage();
            PostComplete(window, false, directInstallError + L"\r\n\r\nFallback installer failed:\r\n" + fallbackError);
            return;
        }

        PostComplete(window, false, directInstallError);
    }
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height, HFONT font, DWORD extraStyle = 0) {
    HWND label = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | extraStyle,
        x,
        y,
        width,
        height,
        parent,
        nullptr,
        gInstance,
        nullptr);
    SetControlFont(label, font);
    return label;
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height) {
    HWND button = CreateWindowExW(
        0,
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        gInstance,
        nullptr);
    SetControlFont(button, gBodyFont);
    return button;
}

void StartInstall(HWND window) {
    if (gInstalling.exchange(true)) {
        return;
    }

    gSetupCompleted = false;
    EnableWindow(gPrimaryButton, FALSE);
    SetWindowTextW(gPrimaryButton, L"Installing...");
    SetWindowTextW(gStatusLabel, L"Starting installer...");
    SetWindowTextW(gDetailsLabel, L"Please keep this window open while setup runs.");
    SendMessageW(gProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(gProgress, PBM_SETPOS, 0, 0);

    std::thread(InstallWorker, window).detach();
}

void OnInstallComplete(HWND window, bool success, const std::wstring& text) {
    gInstalling = false;
    gSetupCompleted = success;
    SendMessageW(gProgress, PBM_SETPOS, success ? 100 : 0, 0);
    SetWindowTextW(gStatusLabel, success ? L"Setup complete" : L"Setup failed");
    SetWindowTextW(gDetailsLabel, text.c_str());
    EnableWindow(gPrimaryButton, TRUE);
    SetWindowTextW(gPrimaryButton, success ? (!gInstalledAppUserModelId.empty() ? L"Launch HDR Corrector" : L"Done") : L"Try again");
    SetWindowTextW(gCloseButton, L"Close");
    if (!success) {
        MessageBoxW(window, text.c_str(), L"HDR Corrector Setup", MB_ICONERROR | MB_OK);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HICON icon = LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_SETUP_ICON));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));

        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        gBodyFont = CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfHeight = -24;
        titleFont.lfWeight = FW_SEMIBOLD;
        wcscpy_s(titleFont.lfFaceName, L"Segoe UI");
        gTitleFont = CreateFontIndirectW(&titleFont);

        CreateLabel(window, L"HDR Corrector Setup", 28, 24, 460, 36, gTitleFont);
        CreateLabel(
            window,
            L"Installs the packaged version of HDR Corrector so Windows can allow borderless capture for the stream mirror.",
            30,
            70,
            476,
            44,
            gBodyFont,
            SS_LEFT);
        CreateLabel(window, L"What setup will do:", 30, 128, 180, 22, gBodyFont);
        CreateLabel(
            window,
            L"1. Install HDR Corrector for the current Windows user.\r\n2. Trust the included package certificate if this build is self-signed.\r\n3. Keep the portable zip available only as an advanced fallback.",
            46,
            154,
            460,
            66,
            gBodyFont,
            SS_LEFT);

        gStatusLabel = CreateLabel(window, L"Ready to install", 30, 238, 460, 22, gBodyFont);
        gDetailsLabel = CreateLabel(window, L"No command line is required.", 30, 264, 460, 42, gBodyFont, SS_LEFT);

        gProgress = CreateWindowExW(
            0,
            PROGRESS_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE,
            30,
            318,
            460,
            18,
            window,
            nullptr,
            gInstance,
            nullptr);

        gPrimaryButton = CreateButton(window, 1, L"Install", 274, 360, 104, 32);
        gCloseButton = CreateButton(window, 2, L"Cancel", 390, 360, 100, 32);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            if (!gInstalling && gSetupCompleted && !gInstalledAppUserModelId.empty()) {
                try {
                    LaunchInstalledApp();
                    DestroyWindow(window);
                } catch (...) {
                    MessageBoxW(window, ExceptionMessage().c_str(), L"HDR Corrector Setup", MB_ICONERROR | MB_OK);
                }
            } else if (!gInstalling && gSetupCompleted) {
                DestroyWindow(window);
            } else {
                StartInstall(window);
            }
            return 0;
        }
        if (LOWORD(wParam) == 2) {
            if (gInstalling) {
                MessageBoxW(window, L"Setup is still running. Please wait for it to finish.", L"HDR Corrector Setup", MB_ICONINFORMATION | MB_OK);
            } else {
                DestroyWindow(window);
            }
            return 0;
        }
        break;
    case kInstallStatusMessage: {
        const auto text = std::unique_ptr<std::wstring>(reinterpret_cast<std::wstring*>(lParam));
        SetWindowTextW(gStatusLabel, text->c_str());
        return 0;
    }
    case kInstallProgressMessage:
        SendMessageW(gProgress, PBM_SETPOS, wParam, 0);
        return 0;
    case kInstallCompleteMessage: {
        const auto text = std::unique_ptr<std::wstring>(reinterpret_cast<std::wstring*>(lParam));
        OnInstallComplete(window, wParam != 0, *text);
        return 0;
    }
    case WM_CLOSE:
        if (gInstalling) {
            MessageBoxW(window, L"Setup is still running. Please wait for it to finish.", L"HDR Corrector Setup", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (gTitleFont) {
            DeleteObject(gTitleFont);
        }
        if (gBodyFont) {
            DeleteObject(gBodyFont);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    gInstance = instance;

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETUP_ICON));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETUP_ICON));

    auto uninitializeCom = [&]() {
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
    };

    if (!RegisterClassExW(&windowClass)) {
        uninitializeCom();
        return 1;
    }

    RECT windowRect = {0, 0, 530, 430};
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    gMainWindow = CreateWindowExW(
        0,
        kWindowClassName,
        L"HDR Corrector Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x,
        y,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!gMainWindow) {
        uninitializeCom();
        return 1;
    }

    ShowWindow(gMainWindow, showCommand);
    UpdateWindow(gMainWindow);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    uninitializeCom();
    return static_cast<int>(message.wParam);
}
