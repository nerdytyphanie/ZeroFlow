/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "platform/ClipboardUserBridge.h"
#include "platform/ClipboardImage.h"
#include "base/Log.h"
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr DWORD MaxMetadata = 3 * 1024 * 1024;
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { reset(); }
  void reset(HANDLE next = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = next;
  }
};
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void writeAll(HANDLE pipe, const void *data, DWORD size) {
  auto bytes = static_cast<const char *>(data);
  while (size) {
    DWORD written = 0;
    check(WriteFile(pipe, bytes, size, &written, nullptr) && written, "clipboard helper pipe closed");
    bytes += written; size -= written;
  }
}
void readAll(HANDLE pipe, void *data, DWORD size, ULONGLONG deadline) {
  auto bytes = static_cast<char *>(data);
  while (size) {
    DWORD available = 0, received = 0;
    check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr), "clipboard helper pipe closed");
    if (!available) {
      check(GetTickCount64() < deadline, "clipboard helper timed out");
      Sleep(2); continue;
    }
    check(ReadFile(pipe, bytes, std::min(size, available), &received, nullptr) && received, "clipboard helper read failed");
    bytes += received; size -= received;
  }
}
std::vector<std::wstring> paths(HANDLE drop) {
  std::vector<std::wstring> result;
  auto count = DragQueryFileW(static_cast<HDROP>(drop), UINT_MAX, nullptr, 0);
  check(count <= MaxMetadata / sizeof(wchar_t), "clipboard selection metadata exceeds limit");
  size_t total = 0;
  for (UINT i = 0; i < count; ++i) {
    auto size = DragQueryFileW(static_cast<HDROP>(drop), i, nullptr, 0);
    check(size && size <= 32767 && (total += size + 1) <= MaxMetadata / sizeof(wchar_t), "clipboard selection metadata exceeds limit");
    std::wstring path(size + 1, L'\0');
    check(DragQueryFileW(static_cast<HDROP>(drop), i, path.data(), size + 1) == size, "clipboard file path unavailable");
    path.resize(size); result.push_back(std::move(path));
  }
  return result;
}
struct ClipboardLock {
  bool opened = false;
  explicit ClipboardLock(HWND window) {
    for (int i = 0; i < 5; ++i) {
      if (OpenClipboard(window)) { opened = true; break; }
      Sleep(5);
    }
  }
  ~ClipboardLock() { if (opened) CloseClipboard(); }
};

class Bridge {
public:
  QJsonObject request(DWORD operation, DWORD sequence = 0) {
    std::lock_guard lock(mutex);
    try {
      if (!process.value || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) launch();
      // One fixed-size request at a time; no unbounded writes on the input thread.
      std::array<DWORD, 2> request{operation, sequence};
      writeAll(input.value, request.data(), sizeof(request));
      auto deadline = GetTickCount64() + 1500;
      DWORD length = 0; readAll(output.value, &length, sizeof(length), deadline);
      check(length && length <= MaxMetadata, "invalid clipboard helper response size");
      QByteArray response(length, Qt::Uninitialized);
      readAll(output.value, response.data(), length, deadline);
      auto document = QJsonDocument::fromJson(response);
      check(document.isObject(), "invalid clipboard helper response");
      auto result = document.object();
      check(result["ok"].toBool(), "desktop clipboard capture failed");
      return result;
    } catch (...) { close(); throw; }
  }
  void stop() { std::lock_guard lock(mutex); close(); }
private:
  void close() {
    input.reset(); job.reset();
    if (process.value) WaitForSingleObject(process.value, 1500);
    process.reset(); output.reset();
    if (!helperExecutable.empty()) DeleteFileW(helperExecutable.c_str());
    if (!helperDirectory.empty()) RemoveDirectoryW(helperDirectory.c_str());
    helperExecutable.clear(); helperDirectory.clear();
  }
  void launch() {
    close();
    // File-serving threads may already impersonate the user for disk access.
    // Restore their token after launching this fixed, unprivileged helper.
    Handle previousToken;
    bool impersonating = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &previousToken.value) != FALSE;
    check(impersonating || GetLastError() == ERROR_NO_TOKEN, "clipboard helper thread identity unavailable");
    if (impersonating) check(RevertToSelf(), "clipboard helper identity reset failed");
    struct Restore { HANDLE token; ~Restore() { if (token) SetThreadToken(nullptr, token); } } restore{previousToken.value};
    DWORD session = 0;
    Handle token;
    check(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session && WTSQueryUserToken(session, &token.value), "desktop user token unavailable");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle childInput, childOutput;
    check(CreatePipe(&childInput.value, &input.value, &security, 65536) &&
          CreatePipe(&output.value, &childOutput.value, &security, 65536), "clipboard helper pipe creation failed");
    check(SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) &&
          SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0), "clipboard helper handle isolation failed");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    check(InitializeProcThreadAttributeList(attributes, 1, 0, &size), "clipboard helper attributes failed");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } guard{attributes};
    HANDLE inherited[]{childInput.value, childOutput.value};
    check(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr), "clipboard helper handle list failed");
    wchar_t executable[32768];
    auto length = GetModuleFileNameW(nullptr, executable, 32768);
    check(length && length < 32768, "clipboard helper executable unavailable");
    // The service installation can be readable only by SYSTEM/administrators.
    // Create a disposable copy inheriting the user's ordinary temp ACL; never
    // grant that user access to the protected service installation.
    wchar_t temp[32768], unique[MAX_PATH];
    check(ExpandEnvironmentStringsForUserW(token.value, L"%TEMP%", temp, 32768) &&
          !wcschr(temp, L'%') && GetTempFileNameW(temp, L"ZFC", 0, unique), "clipboard helper temp directory unavailable");
    check(DeleteFileW(unique) && CreateDirectoryW(unique, nullptr), "clipboard helper temp directory failed");
    helperDirectory = unique; helperExecutable = helperDirectory + L"\\ZeroFlowClipboard.exe";
    {
      Handle source, destination;
      source.value = CreateFileW(executable, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
      destination.value = CreateFileW(helperExecutable.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
      check(source.value != INVALID_HANDLE_VALUE && destination.value != INVALID_HANDLE_VALUE, "clipboard helper copy failed");
      std::array<char, 65536> buffer; DWORD count;
      for (;;) {
        check(ReadFile(source.value, buffer.data(), DWORD(buffer.size()), &count, nullptr), "clipboard helper source read failed");
        if (!count) break;
        writeAll(destination.value, buffer.data(), count);
      }
    }
    auto command = L"\"" + helperExecutable + L"\" --clipboard-user-helper " +
        std::to_wstring(reinterpret_cast<uintptr_t>(childInput.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(childOutput.value));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\default"; startup.StartupInfo.lpDesktop = desktop;
    startup.lpAttributeList = attributes;
    void *environment = nullptr;
    check(CreateEnvironmentBlock(&environment, token.value, FALSE), "clipboard helper environment failed");
    PROCESS_INFORMATION child{};
    auto created = CreateProcessAsUserW(token.value, helperExecutable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
        environment, nullptr, &startup.StartupInfo, &child);
    DestroyEnvironmentBlock(environment);
    check(created, "clipboard helper could not start");
    process.reset(child.hProcess); Handle thread; thread.value = child.hThread;
    job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job.value, process.value)) {
      TerminateProcess(process.value, 1); throw std::runtime_error("clipboard helper lifetime setup failed");
    }
    check(ResumeThread(thread.value) != DWORD(-1), "clipboard helper could not resume");
  }
  std::mutex mutex;
  Handle job, process, input, output;
  std::wstring helperDirectory, helperExecutable;
};
Bridge &bridge() { static Bridge value; return value; }
Bridge &imageBridge() { static Bridge value; return value; }

int helper(HANDLE input, HANDLE output) {
  check(GetFileType(input) == FILE_TYPE_PIPE && GetFileType(output) == FILE_TYPE_PIPE, "invalid clipboard helper pipes");
  auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW windowClass{}; windowClass.lpfnWndProc = DefWindowProcW; windowClass.hInstance = instance;
  windowClass.lpszClassName = L"ZeroFlowClipboardUser";
  check(RegisterClassW(&windowClass) != 0, "clipboard helper window class failed");
  HWND window = CreateWindowW(windowClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
  check(window != nullptr, "clipboard helper window failed");
  struct Window { HWND value; ~Window() { DestroyWindow(value); } } owner{window};
  DWORD capturedSequence = 0;
  std::vector<std::wstring> capturedPaths;
  for (;;) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) return 0;
    if (!available) { Sleep(5); continue; }
    std::array<DWORD, 2> request{};
    readAll(input, request.data(), sizeof(request), GetTickCount64() + 1500);
    QJsonObject result{{"ok", false}};
    try {
      if (request[0] == 3) {
        auto image = ClipboardImage::capture(window, request[1]);
        result = {{"ok", true}, {"path", QString::fromStdWString(image.wstring())}};
      } else {
      ClipboardLock lock(window); check(lock.opened, "desktop clipboard is busy");
      if (request[0] == 1) {
        QJsonArray files;
        auto sequence = GetClipboardSequenceNumber();
        auto drop = GetClipboardData(CF_HDROP);
        auto selection = drop ? paths(drop) : std::vector<std::wstring>{};
        for (const auto &path : selection) files.append(QString::fromStdWString(path));
        capturedSequence = sequence; capturedPaths = std::move(selection);
        result = {{"ok", true}, {"sequence", qint64(sequence)}, {"paths", files}};
      } else if (request[0] == 2) {
        bool same = request[1] && request[1] == capturedSequence && request[1] == GetClipboardSequenceNumber() && !capturedPaths.empty();
        auto drop = same ? GetClipboardData(CF_HDROP) : nullptr;
        if (drop && paths(drop) == capturedPaths) {
          HANDLE marker = GlobalAlloc(GMEM_MOVEABLE, 1);
          auto format = RegisterClipboardFormatW(L"Deskflow Ownership");
          check(marker && format, "clipboard ownership marker unavailable");
          if (!EmptyClipboard() || !SetClipboardData(format, marker)) {
            GlobalFree(marker); throw std::runtime_error("clipboard clear failed");
          }
          capturedSequence = 0; capturedPaths.clear();
        }
        result = {{"ok", true}};
      }
      }
    } catch (const std::exception &) { }
    auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxMetadata) bytes = "{\"ok\":false}";
    DWORD length = static_cast<DWORD>(bytes.size());
    writeAll(output, &length, sizeof(length)); writeAll(output, bytes.constData(), length);
  }
}
} // namespace

bool ClipboardUserBridge::required() {
  static const bool system = [] {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    std::array<unsigned char, 256> buffer{}; DWORD size = 0;
    return GetTokenInformation(token.value, TokenUser, buffer.data(), DWORD(buffer.size()), &size) &&
        IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, WinLocalSystemSid);
  }();
  return system;
}
HANDLE ClipboardUserBridge::readFiles(DWORD &sequence) {
  sequence = 0;
  if (!required()) return nullptr;
  try {
    auto result = bridge().request(1);
    auto captured = result["sequence"].toInteger();
    check(captured > 0 && captured <= UINT32_MAX && result["paths"].isArray(), "invalid clipboard helper snapshot");
    auto files = result["paths"].toArray();
    if (files.empty()) return nullptr;
    std::wstring names;
    for (const auto &file : files) {
      check(file.isString(), "invalid clipboard helper file");
      auto path = file.toString().toStdWString();
      check(!path.empty() && path.find(L'\0') == std::wstring::npos && path.size() <= 32767, "invalid clipboard helper path");
      names += path; names += L'\0';
      check(names.size() < MaxMetadata / sizeof(wchar_t), "clipboard selection metadata exceeds limit");
    }
    names += L'\0';
    HANDLE drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
    check(drop != nullptr, "clipboard snapshot allocation failed");
    auto header = static_cast<DROPFILES *>(GlobalLock(drop));
    if (!header) { GlobalFree(drop); throw std::runtime_error("clipboard snapshot lock failed"); }
    header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
    memcpy(reinterpret_cast<char *>(header) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(drop); sequence = static_cast<DWORD>(captured); return drop;
  } catch (const std::exception &error) { LOG_WARN("file clipboard helper: %s", error.what()); return nullptr; }
}
bool ClipboardUserBridge::clearFiles(DWORD sequence) {
  try { bridge().request(2, sequence); return true; }
  catch (const std::exception &error) { LOG_WARN("file clipboard helper: %s", error.what()); return false; }
}
std::filesystem::path ClipboardUserBridge::captureImage(DWORD sequence) {
  auto result = imageBridge().request(3, sequence);
  check(result["path"].isString() && !result["path"].toString().isEmpty(), "clipboard helper image unavailable");
  return std::filesystem::path(result["path"].toString().toStdWString());
}
void ClipboardUserBridge::stop() { if (required()) { bridge().stop(); imageBridge().stop(); } }
int ClipboardUserBridge::dispatch(int argc, char **argv) {
  if (argc < 2 || std::string(argv[1]) != "--clipboard-user-helper") return -1;
  if (argc != 4 || required()) return 2;
  uintptr_t handles[2]{};
  for (int i = 0; i < 2; ++i) {
    std::string value(argv[i + 2]);
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), handles[i]);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !handles[i]) return 2;
  }
  try { return helper(reinterpret_cast<HANDLE>(handles[0]), reinterpret_cast<HANDLE>(handles[1])); }
  catch (const std::exception &) { return 1; }
}
