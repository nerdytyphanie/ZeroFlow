/*
 * ZeroFlow -- mouse, keyboard and clipboard sharing
 * SPDX-FileCopyrightText: (C) 2026 ZeroFlow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/MSWindowsClipboardFilesConverter.h"

#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
#include <string_view>

namespace {
namespace fs = std::filesystem;
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

} // namespace

ClipboardDesktopUser::ClipboardDesktopUser()
{
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) { valid = true; return; }
  Handle processToken;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &processToken.value)) return;
  BYTE buffer[256]; DWORD length = 0;
  if (!GetTokenInformation(processToken.value, TokenUser, buffer, sizeof(buffer), &length)) return;
  auto user = reinterpret_cast<TOKEN_USER *>(buffer);
  if (!IsWellKnownSid(user->User.Sid, WinLocalSystemSid)) { valid = true; return; }
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0 || !WTSQueryUserToken(session, &token)) return;
  impersonating = ImpersonateLoggedOnUser(token) != FALSE;
  valid = impersonating;
}
ClipboardDesktopUser::~ClipboardDesktopUser()
{
  if (impersonating) RevertToSelf();
  if (token) CloseHandle(token);
}
std::filesystem::path ClipboardDesktopUser::temp() const
{
  wchar_t path[32768];
  if (token) {
    if (!ExpandEnvironmentStringsForUserW(token, L"%TEMP%", path, 32768) || wcschr(path, L'%')) return {};
  } else {
    auto size = GetTempPathW(32768, path);
    if (!size || size >= 32768) return {};
  }
  return std::filesystem::path(path);
}

namespace {
void put32(std::string &data, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) data.push_back(static_cast<char>(value >> shift));
}
bool take32(std::string_view &data, uint32_t &value)
{
  if (data.size() < 4) return false;
  value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | static_cast<unsigned char>(data[i]);
  data.remove_prefix(4);
  return true;
}
std::string utf8(const std::wstring &value)
{
  int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                               nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring wide(std::string_view value)
{
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
  return result;
}
bool safeName(const std::wstring &name)
{
  if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ') return false;
  for (auto c : name)
    if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
  auto stem = name.substr(0, name.find(L'.'));
  while (!stem.empty() && stem.back() == L' ') stem.pop_back();
  std::transform(stem.begin(), stem.end(), stem.begin(), towupper);
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
      stem == L"CONIN$" || stem == L"CONOUT$") return false;
  if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
      (wcschr(L"123456789\u00b9\u00b2\u00b3", stem[3]))) return false;
  return true;
}
struct CaseLess {
  bool operator()(const std::wstring &a, const std::wstring &b) const
  {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  }
};
bool safePath(const std::wstring &path)
{
  if (path.empty() || path.size() > 30000) return false;
  size_t start = 0;
  do {
    auto end = path.find(L'/', start);
    if (!safeName(path.substr(start, end == std::wstring::npos ? end : end - start))) return false;
    if (end == std::wstring::npos) return true;
    start = end + 1;
  } while (start < path.size());
  return false;
}
struct Entry { std::wstring name; std::string_view contents; bool directory; };

bool decode(std::string_view data, std::vector<Entry> &entries, size_t maxFiles)
{
  if (data.size() > MSWindowsClipboardFilesConverter::MaxPayloadBytes || !data.starts_with("ZFC1")) return false;
  data.remove_prefix(4);
  uint32_t count = 0;
  if (!take32(data, count) || !count || count > data.size() / 9) return false;
  std::set<std::wstring, CaseLess> names;
  std::set<std::wstring, CaseLess> directories;
  size_t files = 0;
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t nameSize = 0, fileSize = 0;
    if (!take32(data, nameSize) || !take32(data, fileSize) || nameSize > 120000 || nameSize > data.size()) return false;
    bool directory = fileSize == UINT32_MAX;
    if (directory) fileSize = 0;
    else if (++files > maxFiles || fileSize > MSWindowsClipboardFilesConverter::MaxFileBytes) return false;
    total += fileSize;
    if (total > MSWindowsClipboardFilesConverter::MaxSelectionBytes) return false;
    if (fileSize > data.size() - nameSize) return false;
    auto name = wide(data.substr(0, nameSize));
    if (!safePath(name) || !names.insert(name).second) return false;
    auto separator = name.rfind(L'/');
    if (separator != std::wstring::npos && !directories.contains(name.substr(0, separator))) return false;
    if (directory) directories.insert(name);
    data.remove_prefix(nameSize);
    entries.push_back({std::move(name), data.substr(0, fileSize), directory});
    data.remove_prefix(fileSize);
  }
  return data.empty();
}
} // namespace

std::string MSWindowsClipboardFilesConverter::toIClipboard(HANDLE data) const
{
  try {
  ClipboardDesktopUser desktop;
  if (!desktop.valid) return {};
  auto drop = static_cast<HDROP>(data);
  UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
  if (!count || count > MaxPayloadBytes / 9) return {};
  std::string packet = "ZFC1";
  put32(packet, 0);
  std::set<std::wstring, CaseLess> names;
  uint32_t entries = 0;
  size_t files = 0;
  size_t total = 0;
  std::function<bool(const fs::path &, const std::wstring &)> append = [&](const fs::path &path, const std::wstring &name) {
    if (!safePath(name) || !names.insert(name).second) return false;
    auto encoded = utf8(name);
    if (encoded.empty() || packet.size() + 8 + encoded.size() > MaxPayloadBytes) return false;
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) || info.nFileSizeHigh ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))) return false;
    bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!directory && (++files > MaxFiles || info.nFileSizeLow > MaxFileBytes || info.nFileSizeLow > MaxPayloadBytes - packet.size() - 8 - encoded.size())) return false;
    if (!directory) { total += info.nFileSizeLow; if (total > MaxSelectionBytes) return false; }
    put32(packet, static_cast<uint32_t>(encoded.size()));
    put32(packet, directory ? UINT32_MAX : info.nFileSizeLow);
    packet += encoded;
    ++entries;
    if (directory) {
      for (const auto &child : fs::directory_iterator(path))
        if (!append(child.path(), name + L'/' + child.path().filename().wstring())) return false;
    } else {
      size_t offset = packet.size();
      packet.resize(offset + info.nFileSizeLow);
      DWORD read = 0;
      if (!ReadFile(file.value, packet.data() + offset, info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) return false;
    }
    return true;
  };
  for (UINT i = 0; i < count; ++i) {
    UINT length = DragQueryFileW(drop, i, nullptr, 0);
    if (!length || length >= 32767) return {};
    std::wstring path(length + 1, L'\0');
    if (DragQueryFileW(drop, i, path.data(), length + 1) != length) return {};
    path.resize(length);
    if (!append(fs::path(path), fs::path(path).filename().wstring())) return {};
  }
  std::string encodedCount;
  put32(encodedCount, entries);
  packet.replace(4, 4, encodedCount);
  return packet;
  } catch (const std::exception &) { return {}; }
}

HANDLE MSWindowsClipboardFilesConverter::fromIClipboard(const std::string &data) const
{
  std::vector<Entry> entries;
  if (!decode(data, entries, m_maxFiles)) return nullptr;
  ClipboardDesktopUser desktop;
  if (!desktop.valid) return nullptr;
  auto temp = desktop.temp();
  if (temp.empty()) return nullptr;
  GUID id{};
  wchar_t text[40];
  if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, 40)) return nullptr;
  // Create a fresh direct child of the user's ordinary temp directory. The sender
  // supplies only validated basenames, never directories or destination paths.
  auto directory = temp / (std::wstring(L"ZeroFlow-Clipboard-") + text);
  if (!CreateDirectoryW(directory.c_str(), nullptr)) return nullptr;
  std::vector<std::pair<fs::path, bool>> created;
  auto fail = [&]() -> HANDLE {
    for (auto it = created.rbegin(); it != created.rend(); ++it) {
      if (it->second) RemoveDirectoryW(it->first.c_str());
      else DeleteFileW(it->first.c_str());
    }
    RemoveDirectoryW(directory.c_str());
    return nullptr;
  };
  std::wstring paths;
  for (const auto &entry : entries) {
    auto path = directory / entry.name;
    if (entry.directory) {
      if (!CreateDirectoryW(path.c_str(), nullptr)) return fail();
      created.push_back({path, true});
      if (entry.name.find(L'/') == std::wstring::npos) {
        paths += path.wstring(); paths.push_back(L'\0');
      }
      continue;
    }
    bool written = false;
    {
      Handle file;
      file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file.value != INVALID_HANDLE_VALUE) {
        created.push_back({path, false});
        DWORD bytes = 0;
        written = WriteFile(file.value, entry.contents.data(), static_cast<DWORD>(entry.contents.size()), &bytes, nullptr) &&
                  bytes == entry.contents.size();
      }
    }
    if (!written) return fail();
    if (entry.name.find(L'/') == std::wstring::npos) {
      paths += path.wstring(); paths.push_back(L'\0');
    }
  }
  paths.push_back(L'\0');
  auto result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + paths.size() * sizeof(wchar_t));
  if (!result) return fail();
  auto drop = static_cast<DROPFILES *>(GlobalLock(result));
  if (!drop) { GlobalFree(result); return fail(); }
  drop->pFiles = sizeof(DROPFILES);
  drop->fWide = TRUE;
  memcpy(reinterpret_cast<char *>(drop) + sizeof(DROPFILES), paths.data(), paths.size() * sizeof(wchar_t));
  GlobalUnlock(result);
  // Successful staging is deliberately left to paste (MOVE) or Windows temp cleanup.
  return result;
}
