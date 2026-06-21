#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

std::wstring LastErrorMessage(DWORD error);
std::wstring HResultMessage(HRESULT hr);

std::filesystem::path AppDataDirectory();
std::filesystem::path ScreenshotDirectory();
std::filesystem::path MakeScreenshotPath(std::wstring_view extension);

void Log(std::wstring_view message);
