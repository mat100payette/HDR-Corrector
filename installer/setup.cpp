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
#include <dwmapi.h>
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
constexpr int kPrimaryButtonId = 1;
constexpr int kCloseButtonId = 2;
constexpr UINT_PTR kSetupButtonSubclassId = 1;
constexpr UINT kOwnerDrawHotState = 0x0040;
constexpr wchar_t kButtonHotProp[] = L"HDRCorrectorSetupButtonHot";
constexpr wchar_t kButtonTrackingProp[] = L"HDRCorrectorSetupButtonTracking";

HINSTANCE gInstance = nullptr;
HWND gMainWindow = nullptr;
HWND gStatusLabel = nullptr;
HWND gDetailsLabel = nullptr;
HWND gPrimaryButton = nullptr;
HWND gCloseButton = nullptr;
HFONT gTitleFont = nullptr;
HFONT gBodyFont = nullptr;
HFONT gSmallFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
HBRUSH gPanelBrush = nullptr;
std::atomic_bool gInstalling = false;
bool gSetupCompleted = false;
int gInstallProgress = 0;
UINT gDpi = 96;
std::wstring gInstalledAppUserModelId;

struct SetupTheme {
    bool dark = false;
    COLORREF background = RGB(246, 248, 251);
    COLORREF panel = RGB(255, 255, 255);
    COLORREF panelBorder = RGB(218, 225, 233);
    COLORREF text = RGB(20, 28, 38);
    COLORREF mutedText = RGB(86, 99, 115);
    COLORREF accent = RGB(24, 201, 190);
    COLORREF accentHover = RGB(41, 218, 207);
    COLORREF accentPressed = RGB(15, 154, 148);
    COLORREF primaryText = RGB(4, 28, 33);
    COLORREF secondaryButton = RGB(235, 240, 246);
    COLORREF secondaryHover = RGB(225, 234, 244);
    COLORREF secondaryPressed = RGB(222, 229, 238);
    COLORREF progressTrack = RGB(226, 233, 241);
};

SetupTheme gTheme;

struct SetupLayout {
    int clientWidth = 530;
    int clientHeight = 430;
    RECT title = {};
    RECT subtitle = {};
    RECT panel = {};
    RECT section = {};
    RECT summary = {};
    RECT status = {};
    RECT details = {};
    RECT progress = {};
    RECT primaryButton = {};
    RECT closeButton = {};
};

SetupLayout gLayout;

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

int Scale(int value) {
    return MulDiv(value, static_cast<int>(gDpi), 96);
}

RECT RectFromDip(int x, int y, int width, int height) {
    return {Scale(x), Scale(y), Scale(x + width), Scale(y + height)};
}

RECT ProgressRect() {
    return gLayout.progress;
}

int MeasureTextHeight(HWND window, const wchar_t* text, int width, HFONT font, UINT format = DT_LEFT | DT_WORDBREAK) {
    HDC dc = GetDC(window);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    RECT rect = {0, 0, width, 0};
    DrawTextW(dc, text, -1, &rect, format | DT_CALCRECT);
    SelectObject(dc, oldFont);
    ReleaseDC(window, dc);
    return rect.bottom - rect.top;
}

void ResizeWindowToClient(HWND window, int clientWidth, int clientHeight) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT windowRect = {0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&windowRect, style, FALSE, exStyle, gDpi);

    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - windowWidth) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - windowHeight) / 2;
    SetWindowPos(window, nullptr, x, y, windowWidth, windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool WindowsAppsUseLightTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return status != ERROR_SUCCESS || value != 0;
}

void InitializeTheme() {
    gTheme.dark = !WindowsAppsUseLightTheme();
    if (gTheme.dark) {
        gTheme.background = RGB(13, 18, 26);
        gTheme.panel = RGB(22, 29, 40);
        gTheme.panelBorder = RGB(45, 56, 71);
        gTheme.text = RGB(241, 246, 252);
        gTheme.mutedText = RGB(164, 176, 190);
        gTheme.accent = RGB(35, 221, 205);
        gTheme.accentHover = RGB(63, 235, 222);
        gTheme.accentPressed = RGB(24, 177, 168);
        gTheme.primaryText = RGB(4, 23, 28);
        gTheme.secondaryButton = RGB(34, 44, 58);
        gTheme.secondaryHover = RGB(45, 57, 73);
        gTheme.secondaryPressed = RGB(47, 59, 76);
        gTheme.progressTrack = RGB(40, 50, 65);
    }

    if (gBackgroundBrush) {
        DeleteObject(gBackgroundBrush);
    }
    if (gPanelBrush) {
        DeleteObject(gPanelBrush);
    }
    gBackgroundBrush = CreateSolidBrush(gTheme.background);
    gPanelBrush = CreateSolidBrush(gTheme.panel);
}

void ApplyNativeWindowTheme(HWND window) {
    const BOOL dark = gTheme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
}

void FillRoundRect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

void StrokeRoundRect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawSetupBackground(HWND window, HDC dc) {
    RECT client = {};
    GetClientRect(window, &client);
    FillRect(dc, &client, gBackgroundBrush);

    RECT accent = {0, 0, client.right, 4};
    accent.bottom = Scale(4);
    HBRUSH accentBrush = CreateSolidBrush(gTheme.accent);
    FillRect(dc, &accent, accentBrush);
    DeleteObject(accentBrush);

    HICON icon = LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_SETUP_ICON));
    if (icon) {
        DrawIconEx(dc, Scale(30), Scale(28), icon, Scale(48), Scale(48), 0, nullptr, DI_NORMAL);
    }

    FillRoundRect(dc, gLayout.panel, Scale(18), gTheme.panel);
    StrokeRoundRect(dc, gLayout.panel, Scale(18), gTheme.panelBorder);

    RECT progress = ProgressRect();
    FillRoundRect(dc, progress, Scale(8), gTheme.progressTrack);
    if (gInstallProgress > 0) {
        RECT filled = progress;
        filled.right = filled.left + MulDiv(progress.right - progress.left, gInstallProgress, 100);
        if (filled.right - filled.left < Scale(8)) {
            filled.right = filled.left + Scale(8);
        }
        FillRoundRect(dc, filled, Scale(8), gTheme.accent);
    }
}

void SetInstallProgress(HWND window, int value) {
    gInstallProgress = max(0, min(100, value));
    RECT progress = ProgressRect();
    InvalidateRect(window, &progress, FALSE);
}

bool IsButtonHot(HWND button) {
    return GetPropW(button, kButtonHotProp) != nullptr;
}

void SetButtonHot(HWND button, bool hot) {
    if (IsButtonHot(button) == hot) {
        return;
    }

    if (hot) {
        SetPropW(button, kButtonHotProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    } else {
        RemovePropW(button, kButtonHotProp);
    }
    InvalidateRect(button, nullptr, FALSE);
}

bool IsTrackingButtonLeave(HWND button) {
    return GetPropW(button, kButtonTrackingProp) != nullptr;
}

void SetTrackingButtonLeave(HWND button, bool tracking) {
    if (tracking) {
        SetPropW(button, kButtonTrackingProp, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1)));
    } else {
        RemovePropW(button, kButtonTrackingProp);
    }
}

void TrackButtonLeave(HWND button) {
    if (IsTrackingButtonLeave(button)) {
        return;
    }

    TRACKMOUSEEVENT event = {sizeof(event), TME_LEAVE, button, 0};
    if (TrackMouseEvent(&event)) {
        SetTrackingButtonLeave(button, true);
    }
}

void ResetButtonHoverState(HWND button) {
    SetTrackingButtonLeave(button, false);
    SetButtonHot(button, false);
}

LRESULT CALLBACK SetupButtonSubclassProc(HWND button, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (IsWindowEnabled(button)) {
            TrackButtonLeave(button);
            SetButtonHot(button, true);
        }
        break;
    case WM_MOUSELEAVE:
        ResetButtonHoverState(button);
        break;
    case WM_ENABLE:
        if (!wParam) {
            ResetButtonHoverState(button);
        }
        InvalidateRect(button, nullptr, FALSE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_CAPTURECHANGED:
        InvalidateRect(button, nullptr, FALSE);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_KEYUP: {
        const LRESULT result = DefSubclassProc(button, message, wParam, lParam);
        InvalidateRect(button, nullptr, FALSE);
        return result;
    }
    case WM_NCDESTROY:
        RemovePropW(button, kButtonHotProp);
        RemovePropW(button, kButtonTrackingProp);
        RemoveWindowSubclass(button, SetupButtonSubclassProc, kSetupButtonSubclassId);
        break;
    default:
        break;
    }

    return DefSubclassProc(button, message, wParam, lParam);
}

void DrawSetupButton(const DRAWITEMSTRUCT& item) {
    HDC dc = item.hDC;
    const bool primary = item.CtlID == kPrimaryButtonId;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool hot = !disabled && (((item.itemState & kOwnerDrawHotState) != 0) || IsButtonHot(item.hwndItem));

    COLORREF fill = primary ? gTheme.accent : gTheme.secondaryButton;
    if (pressed) {
        fill = primary ? gTheme.accentPressed : gTheme.secondaryPressed;
    } else if (hot) {
        fill = primary ? gTheme.accentHover : gTheme.secondaryHover;
    }
    if (disabled) {
        fill = gTheme.progressTrack;
    }

    COLORREF text = primary ? gTheme.primaryText : gTheme.text;
    if (disabled) {
        text = gTheme.mutedText;
    }

    RECT rect = item.rcItem;
    HBRUSH parentBrush = CreateSolidBrush(gTheme.background);
    FillRect(dc, &rect, parentBrush);
    DeleteObject(parentBrush);

    RECT buttonRect = rect;
    if (pressed && !disabled) {
        OffsetRect(&buttonRect, 0, Scale(1));
    }

    FillRoundRect(dc, buttonRect, Scale(10), fill);
    if (hot && !disabled) {
        StrokeRoundRect(dc, buttonRect, Scale(10), primary ? gTheme.accentPressed : gTheme.accent);
    } else if (!primary) {
        StrokeRoundRect(dc, buttonRect, Scale(10), gTheme.panelBorder);
    }
    if (focused) {
        RECT focus = buttonRect;
        InflateRect(&focus, -Scale(3), -Scale(3));
        StrokeRoundRect(dc, focus, Scale(8), primary ? gTheme.primaryText : gTheme.accent);
    }

    wchar_t label[128] = {};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, gBodyFont));
    DrawTextW(dc, label, -1, &buttonRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

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

std::vector<BYTE> CertificateSha1Hash(PCCERT_CONTEXT certificateContext) {
    DWORD hashSize = 0;
    if (!CertGetCertificateContextProperty(certificateContext, CERT_SHA1_HASH_PROP_ID, nullptr, &hashSize)) {
        ThrowWindowsError(L"Could not read the package signing certificate thumbprint");
    }

    std::vector<BYTE> hash(hashSize);
    if (!CertGetCertificateContextProperty(certificateContext, CERT_SHA1_HASH_PROP_ID, hash.data(), &hashSize)) {
        ThrowWindowsError(L"Could not read the package signing certificate thumbprint");
    }
    hash.resize(hashSize);
    return hash;
}

bool CertificateExistsInStore(HCERTSTORE store, const std::vector<BYTE>& hash, const wchar_t* displayName) {
    CRYPT_HASH_BLOB hashBlob = {static_cast<DWORD>(hash.size()), const_cast<BYTE*>(hash.data())};
    PCCERT_CONTEXT existing = CertFindCertificateInStore(
        store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_SHA1_HASH,
        &hashBlob,
        nullptr);
    if (existing) {
        CertFreeCertificateContext(existing);
        return true;
    }

    const DWORD error = GetLastError();
    if (error != CRYPT_E_NOT_FOUND) {
        const std::wstring action = std::wstring(L"Could not search the current user's ") + displayName + L" certificate store";
        ThrowWindowsError(action.c_str(), error);
    }
    return false;
}

void AddCertificateToCurrentUserStore(const ResourceBytes& certificate, const wchar_t* storeName, const wchar_t* displayName) {
    PCCERT_CONTEXT certificateContext = CertCreateCertificateContext(
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        certificate.data,
        certificate.size);
    if (!certificateContext) {
        ThrowWindowsError(L"Could not read the embedded package signing certificate");
    }

    HCERTSTORE store = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        0,
        CERT_SYSTEM_STORE_CURRENT_USER,
        storeName);
    if (!store) {
        const DWORD error = GetLastError();
        CertFreeCertificateContext(certificateContext);
        const std::wstring action = std::wstring(L"Could not open the current user's ") + displayName + L" certificate store";
        ThrowWindowsError(action.c_str(), error);
    }

    const std::vector<BYTE> hash = CertificateSha1Hash(certificateContext);
    if (CertificateExistsInStore(store, hash, displayName)) {
        CertCloseStore(store, 0);
        CertFreeCertificateContext(certificateContext);
        return;
    }

    const BOOL ok = CertAddCertificateContextToStore(
        store,
        certificateContext,
        CERT_STORE_ADD_NEW,
        nullptr);
    const DWORD error = GetLastError();
    CertCloseStore(store, 0);
    CertFreeCertificateContext(certificateContext);

    if (!ok && error != CRYPT_E_EXISTS) {
        const std::wstring action = std::wstring(L"Could not add the local package signing certificate to the current user's ") + displayName + L" certificate store";
        ThrowWindowsError(action.c_str(), error);
    }
}

void TrustCertificateForCurrentUser(const TempFiles& files) {
    if (files.certificatePath.empty()) {
        return;
    }

    const ResourceBytes certificate = FindResourceBytes(IDR_SETUP_CERTIFICATE);
    if (!certificate.data) {
        return;
    }

    // MSIX validation needs a trusted chain root and a trusted package publisher.
    AddCertificateToCurrentUserStore(certificate, L"Root", L"Trusted Root");
    AddCertificateToCurrentUserStore(certificate, L"TrustedPeople", L"Trusted People");
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
        const auto options = winrt::Windows::Management::Deployment::DeploymentOptions::ForceApplicationShutdown;

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

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height, HFONT font, DWORD extraStyle = 0, bool panelLabel = false) {
    HWND label = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | extraStyle,
        Scale(x),
        Scale(y),
        Scale(width),
        Scale(height),
        parent,
        nullptr,
        gInstance,
        nullptr);
    SetControlFont(label, font);
    if (panelLabel) {
        SetPropW(label, L"HDRCorrectorSetupPanelLabel", reinterpret_cast<HANDLE>(1));
    }
    return label;
}

HWND CreateLabelInRect(HWND parent, const wchar_t* text, const RECT& rect, HFONT font, DWORD extraStyle = 0, bool panelLabel = false) {
    HWND label = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE | extraStyle,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        parent,
        nullptr,
        gInstance,
        nullptr);
    SetControlFont(label, font);
    if (panelLabel) {
        SetPropW(label, L"HDRCorrectorSetupPanelLabel", reinterpret_cast<HANDLE>(1));
    }
    return label;
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height) {
    HWND button = CreateWindowExW(
        0,
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
        Scale(x),
        Scale(y),
        Scale(width),
        Scale(height),
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        gInstance,
        nullptr);
    SetControlFont(button, gBodyFont);
    SetWindowSubclass(button, SetupButtonSubclassProc, kSetupButtonSubclassId, 0);
    return button;
}

HWND CreateButtonInRect(HWND parent, int id, const wchar_t* text, const RECT& rect) {
    HWND button = CreateWindowExW(
        0,
        L"BUTTON",
        text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        gInstance,
        nullptr);
    SetControlFont(button, gBodyFont);
    SetWindowSubclass(button, SetupButtonSubclassProc, kSetupButtonSubclassId, 0);
    return button;
}

void ComputeLayout(HWND window) {
    gLayout.clientWidth = Scale(560);

    gLayout.title = RectFromDip(92, 24, 420, 38);
    const wchar_t* subtitle = L"Install the packaged build for clean updates and borderless stream capture support.";
    const int subtitleWidth = gLayout.clientWidth - Scale(124);
    const int subtitleHeight = MeasureTextHeight(window, subtitle, subtitleWidth, gBodyFont);
    gLayout.subtitle = {Scale(94), Scale(64), Scale(94) + subtitleWidth, Scale(64) + subtitleHeight};

    const int panelX = Scale(28);
    const int panelY = max(Scale(126), gLayout.subtitle.bottom + Scale(18));
    const int panelWidth = gLayout.clientWidth - Scale(56);
    const int panelTextX = panelX + Scale(18);
    const int panelTextWidth = panelWidth - Scale(36);

    int y = panelY + Scale(20);
    const int sectionHeight = MeasureTextHeight(window, L"Setup Includes", panelTextWidth, gSmallFont, DT_LEFT | DT_SINGLELINE);
    gLayout.section = {panelTextX, y, panelTextX + panelTextWidth, y + sectionHeight};
    y += sectionHeight + Scale(16);

    const wchar_t* summary =
        L"Install HDR Corrector for your Windows account.\r\n"
        L"Trust the included package certificate when this build is self-signed.\r\n"
        L"Preserve the portable zip as an advanced fallback.";
    const int summaryHeight = MeasureTextHeight(window, summary, panelTextWidth, gBodyFont);
    gLayout.summary = {panelTextX, y, panelTextX + panelTextWidth, y + summaryHeight};
    y += summaryHeight + Scale(22);

    const int statusHeight = MeasureTextHeight(window, L"Ready to install", panelTextWidth, gBodyFont, DT_LEFT | DT_SINGLELINE);
    gLayout.status = {panelTextX, y, panelTextX + panelTextWidth, y + statusHeight};
    y += statusHeight + Scale(8);

    const wchar_t* details = L"HDR Corrector will be installed for the current user.";
    const wchar_t* successDetails = L"HDR Corrector is installed. You can launch it now or close this installer.";
    const int detailsHeight = max(
        max(MeasureTextHeight(window, details, panelTextWidth, gBodyFont), MeasureTextHeight(window, successDetails, panelTextWidth, gBodyFont)),
        Scale(40));
    gLayout.details = {panelTextX, y, panelTextX + panelTextWidth, y + detailsHeight};
    y += detailsHeight + Scale(18);

    gLayout.progress = {panelTextX, y, panelTextX + panelTextWidth, y + Scale(10)};
    y = gLayout.progress.bottom + Scale(18);
    gLayout.panel = {panelX, panelY, panelX + panelWidth, y};

    const int buttonTop = gLayout.panel.bottom + Scale(20);
    gLayout.closeButton = {gLayout.clientWidth - Scale(140), buttonTop, gLayout.clientWidth - Scale(30), buttonTop + Scale(36)};
    gLayout.primaryButton = {gLayout.closeButton.left - Scale(122), buttonTop, gLayout.closeButton.left - Scale(12), buttonTop + Scale(36)};
    gLayout.clientHeight = gLayout.closeButton.bottom + Scale(28);
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
    SetInstallProgress(window, 0);
    InvalidateRect(gPrimaryButton, nullptr, FALSE);

    std::thread(InstallWorker, window).detach();
}

void OnInstallComplete(HWND window, bool success, const std::wstring& text) {
    gInstalling = false;
    gSetupCompleted = success;
    SetInstallProgress(window, success ? 100 : 0);
    SetWindowTextW(gStatusLabel, success ? L"Setup complete" : L"Setup failed");
    SetWindowTextW(gDetailsLabel, text.c_str());
    EnableWindow(gPrimaryButton, TRUE);
    SetWindowTextW(gPrimaryButton, success ? (!gInstalledAppUserModelId.empty() ? L"Launch HDR Corrector" : L"Done") : L"Try again");
    SetWindowTextW(gCloseButton, L"Close");
    InvalidateRect(gPrimaryButton, nullptr, FALSE);
    InvalidateRect(gCloseButton, nullptr, FALSE);
    if (!success) {
        MessageBoxW(window, text.c_str(), L"HDR Corrector Setup", MB_ICONERROR | MB_OK);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        ApplyNativeWindowTheme(window);

        HICON icon = LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_SETUP_ICON));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));

        NONCLIENTMETRICSW metrics = {sizeof(metrics)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        metrics.lfMessageFont.lfHeight = -Scale(14);
        wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
        gBodyFont = CreateFontIndirectW(&metrics.lfMessageFont);

        LOGFONTW titleFont = metrics.lfMessageFont;
        titleFont.lfHeight = -Scale(28);
        titleFont.lfWeight = FW_SEMIBOLD;
        wcscpy_s(titleFont.lfFaceName, L"Segoe UI Variable Display");
        gTitleFont = CreateFontIndirectW(&titleFont);

        LOGFONTW smallFont = metrics.lfMessageFont;
        smallFont.lfHeight = -Scale(12);
        wcscpy_s(smallFont.lfFaceName, L"Segoe UI");
        gSmallFont = CreateFontIndirectW(&smallFont);

        ComputeLayout(window);
        ResizeWindowToClient(window, gLayout.clientWidth, gLayout.clientHeight);

        CreateLabelInRect(window, L"HDR Corrector Setup", gLayout.title, gTitleFont);
        CreateLabelInRect(
            window,
            L"Install the packaged build for clean updates and borderless stream capture support.",
            gLayout.subtitle,
            gBodyFont,
            SS_LEFT);
        CreateLabelInRect(window, L"Setup Includes", gLayout.section, gSmallFont, 0, true);
        CreateLabelInRect(
            window,
            L"Install HDR Corrector for your Windows account.\r\nTrust the included package certificate when this build is self-signed.\r\nPreserve the portable zip as an advanced fallback.",
            gLayout.summary,
            gBodyFont,
            SS_LEFT,
            true);

        gStatusLabel = CreateLabelInRect(window, L"Ready to install", gLayout.status, gBodyFont, 0, true);
        gDetailsLabel = CreateLabelInRect(window, L"HDR Corrector will be installed for the current user.", gLayout.details, gBodyFont, SS_LEFT, true);

        gPrimaryButton = CreateButtonInRect(window, kPrimaryButtonId, L"Install", gLayout.primaryButton);
        gCloseButton = CreateButtonInRect(window, kCloseButtonId, L"Cancel", gLayout.closeButton);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        DrawSetupBackground(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, gTheme.text);
        return reinterpret_cast<LRESULT>(
            GetPropW(control, L"HDRCorrectorSetupPanelLabel") ? gPanelBrush : gBackgroundBrush);
    }
    case WM_DRAWITEM:
        DrawSetupButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == kPrimaryButtonId) {
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
        if (LOWORD(wParam) == kCloseButtonId) {
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
        SetInstallProgress(window, static_cast<int>(wParam));
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
        if (gSmallFont) {
            DeleteObject(gSmallFont);
        }
        if (gBackgroundBrush) {
            DeleteObject(gBackgroundBrush);
            gBackgroundBrush = nullptr;
        }
        if (gPanelBrush) {
            DeleteObject(gPanelBrush);
            gPanelBrush = nullptr;
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

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    gDpi = GetDpiForSystem();
    InitializeTheme();

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SETUP_ICON));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = gBackgroundBrush;
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

    RECT windowRect = {0, 0, Scale(560), Scale(460)};
    AdjustWindowRectExForDpi(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, gDpi);

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
