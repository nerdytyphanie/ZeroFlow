// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WindowsService.h"
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUuid>
#include <atomic>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>

namespace {
constexpr wchar_t serviceName[] = L"ZeroFlow Embedded";
struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
Handle stopped, changed;
SERVICE_STATUS_HANDLE statusHandle;
std::vector<std::wstring> workerArgs;
std::atomic<bool> resumed = false;
void fail(const char *operation) { throw std::runtime_error(std::string(operation) + ": " + std::to_string(GetLastError())); }
void report(const std::string &message) {
  auto text = QString::fromStdString(message).toStdWString();
  auto source = RegisterEventSourceW(nullptr, serviceName);
  if (source) { const wchar_t *lines[] = {text.c_str()}; ReportEventW(source, EVENTLOG_WARNING_TYPE, 0, 1, nullptr, 1, 0, lines, nullptr); DeregisterEventSource(source); }
}
void status(DWORD state, DWORD error = 0) {
  SERVICE_STATUS s{}; s.dwServiceType = SERVICE_WIN32_OWN_PROCESS; s.dwCurrentState = state;
  s.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_POWEREVENT : 0;
  s.dwWin32ExitCode = error;
  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) { s.dwCheckPoint = 1; s.dwWaitHint = 10000; }
  SetServiceStatus(statusHandle, &s);
}
DWORD WINAPI control(DWORD code, DWORD event, LPVOID, LPVOID) {
  if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) { status(SERVICE_STOP_PENDING); SetEvent(stopped.value); }
  if (code == SERVICE_CONTROL_SESSIONCHANGE) SetEvent(changed.value);
  if (code == SERVICE_CONTROL_POWEREVENT && (event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND)) { resumed = true; SetEvent(changed.value); }
  return NO_ERROR;
}
ULONGLONG awakeMs() { ULONGLONG value = 0; QueryUnbiasedInterruptTime(&value); return value / 10000; }
DWORD activeSession() {
  PWTS_SESSION_INFOW sessions = nullptr; DWORD count = 0, id = 0xffffffff;
  if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    for (DWORD i = 0; i < count; ++i) if (sessions[i].State == WTSActive) { id = sessions[i].SessionId; break; }
    WTSFreeMemory(sessions);
  }
  // The console also supplies the secure desktop before login.
  return id == 0xffffffff ? WTSGetActiveConsoleSessionId() : id;
}
HANDLE sessionSystemToken(DWORD session) {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof entry;
  if (Process32FirstW(snapshot.value, &entry)) do {
    DWORD id = 0;
    if (_wcsicmp(entry.szExeFile, L"winlogon.exe") || !ProcessIdToSessionId(entry.th32ProcessID, &id) || id != session) continue;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
    Handle token; HANDLE duplicate = nullptr;
    if (process.value && OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &token.value) &&
        DuplicateTokenEx(token.value, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &duplicate)) return duplicate;
  } while (Process32NextW(snapshot.value, &entry));
  fail("Find interactive SYSTEM token"); return nullptr;
}
std::wstring quote(const std::wstring &arg) {
  std::wstring result = L"\""; size_t slashes = 0;
  for (auto c : arg) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(slashes * (c == L'\"' ? 2 : 1), L'\\'); slashes = 0;
    if (c == L'\"') result += L'\\'; result += c;
  }
  result.append(slashes * 2, L'\\'); return result + L"\"";
}
struct Pipe {
  Handle handle, event;
  OVERLAPPED io{}; char buffer[4096]; std::string pending;
  bool connecting = true;
  Pipe(const std::wstring &name) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) fail("Pipe security");
    SECURITY_ATTRIBUTES security{sizeof security, descriptor, FALSE};
    handle.value = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &security);
    LocalFree(descriptor);
    if (handle.value == INVALID_HANDLE_VALUE) fail("Create status pipe");
    event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); io.hEvent = event.value;
    if (!ConnectNamedPipe(handle.value, &io)) {
      if (GetLastError() == ERROR_PIPE_CONNECTED) SetEvent(event.value);
      else if (GetLastError() != ERROR_IO_PENDING) fail("Connect status pipe");
    }
  }
  ~Pipe() { CancelIoEx(handle.value, nullptr); DWORD ignored; GetOverlappedResult(handle.value, &io, &ignored, TRUE); }
  void readNext() {
    ResetEvent(event.value); io = {}; io.hEvent = event.value;
    if (!ReadFile(handle.value, buffer, sizeof buffer, nullptr, &io) && GetLastError() != ERROR_IO_PENDING) fail("Read worker pipe");
  }
  template<class F> void consume(F line) {
    if (connecting) { connecting = false; readNext(); return; }
    DWORD bytes = 0;
    if (!GetOverlappedResult(handle.value, &io, &bytes, FALSE) || !bytes) fail("Worker pipe closed");
    pending.append(buffer, bytes);
    size_t end;
    while ((end = pending.find('\n')) != std::string::npos) { line(pending.substr(0, end)); pending.erase(0, end + 1); }
    if (pending.size() > 8192) throw std::runtime_error("Oversized worker record");
    readNext();
  }
};
void runWorker(DWORD session) {
  wchar_t path[32768]; auto length = GetModuleFileNameW(nullptr, path, 32768);
  if (!length || length == 32768) fail("Executable path");
  auto prefix = LR"(\\.\pipe\ZeroFlow-)" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString();
  Pipe output(prefix + L"-out"), errors(prefix + L"-err"), input(prefix + L"-in");
  auto command = quote(path);
  for (const auto &arg : workerArgs) command += L" " + quote(arg);
  command += L" --status-json --control-stdin --service-pipe " + quote(prefix);
  Handle token(sessionSystemToken(session));
  void *environment = nullptr;
  if (!CreateEnvironmentBlock(&environment, token.value, FALSE)) fail("Worker environment");
  STARTUPINFOW startup{}; startup.cb = sizeof startup; wchar_t desktop[] = LR"(winsta0\default)"; startup.lpDesktop = desktop;
  PROCESS_INFORMATION child{};
  BOOL created = CreateProcessAsUserW(token.value, path, command.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, environment, nullptr, &startup, &child);
  DestroyEnvironmentBlock(environment);
  if (!created) fail("Start interactive worker");
  Handle process(child.hProcess), thread(child.hThread), job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof limits) || !AssignProcessToJobObject(job.value, process.value)) {
    TerminateProcess(process.value, 1); fail("Worker job");
  }
  ResumeThread(thread.value);
  ULONGLONG last = awakeMs(), grace = last + 30000; qint64 sequence = 0;
  bool shuttingDown = false;
  try {
    HANDLE waits[] = {stopped.value, changed.value, process.value, output.event.value, errors.event.value, input.event.value};
    for (;;) {
      auto result = WaitForMultipleObjects(6, waits, FALSE, 2000);
      if (result == WAIT_OBJECT_0) { shuttingDown = true; break; }
      if (result == WAIT_OBJECT_0 + 1) {
        if (activeSession() != session) { shuttingDown = true; break; }
        if (resumed.exchange(false)) grace = awakeMs() + 30000;
      } else if (result == WAIT_OBJECT_0 + 2) throw std::runtime_error("Sharing worker exited");
      else if (result == WAIT_OBJECT_0 + 3) output.consume([&](const std::string &line) {
        const auto record = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        auto next = record.value("sequence").toInteger();
        if (record.value("schema").toInt() == 1 && record.value("type").toString() == "status" && next > sequence) {
          sequence = next; last = awakeMs();
        }
      });
      else if (result == WAIT_OBJECT_0 + 4) errors.consume([](const std::string &line) { if (!line.empty()) report(line); });
      else if (result == WAIT_OBJECT_0 + 5) { input.connecting = false; ResetEvent(input.event.value); }
      else if (result == WAIT_FAILED) fail("Worker wait");
      auto now = awakeMs();
      if (now > grace && now - last > 10000) throw std::runtime_error("Sharing worker heartbeat timed out");
    }
  } catch (...) {
    TerminateProcess(process.value, 1); WaitForSingleObject(process.value, 4000); throw;
  }
  if (shuttingDown && !input.connecting) {
    constexpr char stop[] = "{\"command\":\"stop\"}\n";
    OVERLAPPED write{}; Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)); write.hEvent = ready.value;
    WriteFile(input.handle.value, stop, sizeof(stop) - 1, nullptr, &write);
    WaitForSingleObject(ready.value, 1000); CancelIoEx(input.handle.value, &write); DWORD ignored; GetOverlappedResult(input.handle.value, &write, &ignored, TRUE);
  }
  if (WaitForSingleObject(process.value, 4000) != WAIT_OBJECT_0) { TerminateProcess(process.value, 1); WaitForSingleObject(process.value, 4000); }
}
void WINAPI serviceMain(DWORD, LPWSTR *) {
  stopped.value = CreateEventW(nullptr, TRUE, FALSE, nullptr); changed.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  statusHandle = RegisterServiceCtrlHandlerExW(serviceName, control, nullptr);
  if (!statusHandle) return;
  status(SERVICE_RUNNING);
  unsigned failures = 0;
  while (WaitForSingleObject(stopped.value, 0) != WAIT_OBJECT_0) {
    const auto before = awakeMs();
    try {
      auto session = activeSession();
      if (session == 0xffffffff) { HANDLE waits[] = {stopped.value, changed.value}; WaitForMultipleObjects(2, waits, FALSE, 30000); continue; }
      runWorker(session); failures = 0;
    } catch (const std::exception &e) {
      report(e.what()); if (awakeMs() - before > 60000) failures = 0;
      const DWORD delays[] = {1000, 2000, 5000, 10000, 30000};
      WaitForSingleObject(stopped.value, delays[std::min(failures++, 4u)]);
    }
  }
  status(SERVICE_STOPPED);
}
}
int dispatchWindowsService(int &argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--service-pipe" && i + 1 < argc) {
      auto prefix = QString::fromLocal8Bit(argv[i+1]).toStdWString();
      const wchar_t *suffixes[] = {L"-in", L"-out", L"-err"};
      const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
      for (int j = 0; j < 3; ++j) {
        auto name = prefix + suffixes[j];
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return 6;
        SetStdHandle(ids[j], pipe); // Process lifetime; the service owns the peer.
      }
      for (int j = i; j + 2 < argc; ++j) argv[j] = argv[j + 2];
      argc -= 2; argv[argc] = nullptr; return -1;
    }
  }
  bool service = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--service") service = true;
    else workerArgs.push_back(QString::fromLocal8Bit(argv[i]).toStdWString());
  }
  if (!service) return -1;
  SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(serviceName), serviceMain}, {nullptr, nullptr}};
  return StartServiceCtrlDispatcherW(table) ? 0 : int(GetLastError());
}
