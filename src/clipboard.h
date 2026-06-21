#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

bool CopyBgraToClipboard(
    HWND hwnd,
    UINT width,
    UINT height,
    const std::vector<uint8_t>& bgra,
    const std::filesystem::path& imageFilePath,
    std::wstring& error);
