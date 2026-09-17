/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include <Windows.h>
#include <filesystem>

// Called only by the file worker/user helper, never by the input event loop.
namespace ClipboardImage {
std::filesystem::path capture(HWND window, DWORD sequence);
// Caller owns the returned CF_DIB allocation until SetClipboardData succeeds.
HANDLE load(const std::filesystem::path &path);
}
