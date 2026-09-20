/*
 * SPDX-FileCopyrightText: (C) 2026 ZeroFlow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/FileClipboardTransfer.h"
#include "platform/ClipboardUserBridge.h"
#include "platform/ClipboardImage.h"
#include "base/HeadlessStatus.h"
#include "platform/MSWindowsClipboardFilesConverter.h"
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <deque>
#include <map>

namespace {
namespace fs = std::filesystem;
using Converter = MSWindowsClipboardFilesConverter;
using Clock = std::chrono::steady_clock;
constexpr qsizetype ChunkBytes = 1024 * 1024;
constexpr qsizetype PauseBytes = 64 * ChunkBytes;
constexpr quint32 ReadyIndex = UINT32_MAX - 1;
constexpr quint32 PastedIndex = UINT32_MAX - 2;
constexpr quint64 UnlimitedBytes = INT64_MAX;
std::atomic<quint64> speedLimit = 0;
std::atomic<quint64> selectionLimit = Converter::MaxSelectionBytes;
std::atomic<quint32> fileLimit = Converter::MaxFiles;
std::atomic<bool> configured = false;
struct Limits { quint64 bytes; quint32 files; };
Limits receiveLimits(const QJsonObject &offer, bool capture = false) {
  if (capture) return {UnlimitedBytes, 1};
  if (!configured && offer["policy"].toBool()) {
    auto bytes = offer["maximum"].toInteger(-1), files = offer["files"].toInteger(-1);
    if (bytes >= 0 && files > 0 && files < PastedIndex) return {quint64(bytes), quint32(files)};
  }
  return {selectionLimit.load(), fileLimit.load()};
}
struct NetworkError : std::runtime_error { using std::runtime_error::runtime_error; };
void networkCheck(bool ok, const char *reason) { if (!ok) throw NetworkError(reason); }
struct Progress {
  QString id, direction;
  quint64 total = 0, done = 0;
  int retries = 0;
  Clock::time_point started = Clock::now(), last = {};
  bool image = false;
  bool capture = false;
  void report(const char *state, const QString &file = {}) {
    if (image) return;
    auto now = Clock::now();
    if (QStringView(u"transferring") == QLatin1StringView(state) && now - last < std::chrono::milliseconds(250)) return;
    last = now;
    double seconds = std::chrono::duration<double>(now - started).count();
    HeadlessStatus::transfer({{"id", id}, {"direction", direction}, {"state", state},
      {"bytes", qint64(done)}, {"total", qint64(total)}, {"bytesPerSecond", seconds > 0 ? done / seconds : 0},
      {"retries", retries}, {"file", file.left(160)}, {"capture", capture}});
  }
};
struct RateLimit {
  quint64 peerMiB = 0;
  Clock::time_point next = Clock::now();
  void wait(qsizetype bytes, const std::function<bool()> &cancelled) {
    auto cap = speedLimit.load();
    if (peerMiB && (!cap || peerMiB < cap)) cap = peerMiB;
    if (!cap) return;
    next = std::max(next, Clock::now()) + std::chrono::nanoseconds(qint64(1e9 * bytes / (double(cap) * ChunkBytes)));
    while (Clock::now() < next) {
      if (cancelled()) throw std::runtime_error("file transfer cancelled");
      std::this_thread::sleep_for(std::min(next - Clock::now(), Clock::duration(std::chrono::milliseconds(10))));
    }
  }
};
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
struct DiskFile {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~DiskFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void put32(QByteArray &data, quint32 n)
{ for (int shift = 24; shift >= 0; shift -= 8) data.append(char(n >> shift)); }
quint32 get32(const QByteArray &data)
{ quint32 n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | static_cast<unsigned char>(data[i]); return n; }
void put64(QByteArray &data, quint64 n)
{ for (int shift = 56; shift >= 0; shift -= 8) data.append(char(n >> shift)); }
quint64 get64(const QByteArray &data)
{ quint64 n = 0; for (int i = 0; i < 8; ++i) n = (n << 8) | static_cast<unsigned char>(data[i]); return n; }
bool local(const QHostAddress &address)
{
  bool ok = false; auto ip = address.toIPv4Address(&ok);
  return ok && ((ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 16) == 0xc0a8 ||
                (ip >> 20) == 0xac1 || (ip >> 16) == 0xa9fe);
}
QByteArray readBytes(QSslSocket &socket, qsizetype length, const std::function<bool()> &cancelled)
{
  QByteArray result; auto deadline = Clock::now() + std::chrono::seconds(15);
  while (result.size() < length) {
    check(!cancelled(), "file transfer cancelled");
    if (!socket.bytesAvailable()) {
      socket.waitForReadyRead(100);
      networkCheck(socket.bytesAvailable() || (socket.state() != QAbstractSocket::UnconnectedState && Clock::now() < deadline), "file transfer timed out");
      continue;
    }
    result += socket.read(std::min<qsizetype>(ChunkBytes, length - result.size()));
    deadline = Clock::now() + std::chrono::seconds(15);
  }
  return result;
}
void sendBytes(QSslSocket &socket, const QByteArray &data, const std::function<bool()> &cancelled, qsizetype &bytesSincePause)
{
  for (qsizetype offset = 0; offset < data.size();) {
    check(!cancelled(), "file transfer cancelled");
    auto count = std::min<qsizetype>(ChunkBytes, data.size() - offset);
    networkCheck(socket.write(data.constData() + offset, count) == count, "file socket write failed");
    while (socket.bytesToWrite()) {
      check(!cancelled(), "file transfer cancelled");
      networkCheck(socket.waitForBytesWritten(1000), "file socket stopped accepting data");
    }
    offset += count;
    bytesSincePause += count;
    // One MiB buffers on the separate file connection; pacing is accumulated
    // across files so a selection of small files follows the same policy.
    if (bytesSincePause >= PauseBytes) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      bytesSincePause -= PauseBytes;
    }
  }
}
struct Drop {
  HANDLE value = nullptr;
  ~Drop() { if (value) GlobalFree(value); }
};
HANDLE makeDrop(const std::vector<fs::path> &paths)
{
  std::wstring list;
  for (const auto &path : paths) { list += path.wstring(); list.push_back(0); }
  list.push_back(0);
  auto result = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
  check(result != nullptr, "clipboard allocation failed");
  auto header = static_cast<DROPFILES *>(GlobalLock(result));
  if (!header) { GlobalFree(result); throw std::runtime_error("clipboard lock failed"); }
  header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
  memcpy(reinterpret_cast<char *>(header) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(result); return result;
}
fs::path dropPath(HANDLE drop, UINT index = 0)
{
  wchar_t path[32768];
  check(DragQueryFileW(static_cast<HDROP>(drop), index, path, 32768) != 0, "missing clipboard path");
  return fs::path(path);
}
fs::path knownFolder(REFKNOWNFOLDERID id)
{
  HANDLE token = nullptr;
  check(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &token) || GetLastError() == ERROR_NO_TOKEN,
        "capture user identity unavailable");
  PWSTR value = nullptr;
  auto result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, token, &value);
  if (token) CloseHandle(token);
  check(SUCCEEDED(result), "capture folder unavailable");
  fs::path path(value); CoTaskMemFree(value); return path;
}
fs::path captureRelative(const QString &value)
{
  auto path = fs::path(value.toStdWString());
  check(!path.empty() && !path.is_absolute() && !path.has_root_name() && !path.has_root_directory(), "capture path must be relative");
  for (const auto &part : path) {
    const auto text = QString::fromStdWString(part.wstring());
    const auto base = text.section('.', 0, 0).toUpper();
    check(!text.isEmpty() && text != "." && text != ".." && !text.endsWith('.') && !text.endsWith(' ') &&
          !text.contains(':') && !text.contains(QChar(0)) && !text.contains('*') && !text.contains('?') &&
          !text.contains('<') && !text.contains('>') && !text.contains('|') && !text.contains('"') &&
          base != "CON" && base != "PRN" && base != "AUX" && base != "NUL" &&
          !(base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) && base[3] >= '0' && base[3] <= '9'),
          "unsafe capture path");
  }
  check(value.size() < 2048, "capture path too long");
  return path;
}
void noLinks(const fs::path &path, const fs::path &root)
{
  for (auto current = path; !current.empty();) {
    // Windows may redirect the trusted known-folder root (for example OneDrive).
    // Reject links introduced beneath it, not the user's configured root itself.
    if (_wcsicmp(current.lexically_normal().c_str(), root.lexically_normal().c_str()) == 0) break;
    auto attributes = GetFileAttributesW(current.c_str());
    check(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "capture path uses a link");
    auto parent = current.parent_path(); if (parent == current) break; current = parent;
  }
}
fs::path saveCapture(const fs::path &source, const fs::path &videos, const fs::path &relative)
{
  auto destination = videos / relative;
  noLinks(destination.parent_path(), videos);
  fs::create_directories(destination.parent_path());
  noLinks(destination.parent_path(), videos);
  // Never overwrite an existing capture, including a same-name capture from another device.
  for (int index = 0; index < 10000; ++index) {
    auto candidate = index == 0 ? destination : destination.parent_path() /
      (destination.stem().wstring() + L" (" + std::to_wstring(index) + L")" + destination.extension().wstring());
    if (MoveFileExW(source.c_str(), candidate.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return candidate;
    auto error = GetLastError();
    check(error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS, "could not save capture in Videos");
  }
  throw std::runtime_error("too many same-name captures in Videos");
}
struct Staging {
  fs::path directory;
  ~Staging() { if (!directory.empty()) { std::error_code ignored; fs::remove_all(directory, ignored); } }
};
struct Snapshot {
  DWORD sequence = 0;
  HWND window = nullptr;
  bool pasted = false;
  bool image = false;
  bool capture = false;
  Staging imageStaging;
  QByteArray token;
  std::vector<fs::path> roots;
  std::vector<fs::path> files;
  std::vector<quint64> sizes;
  QByteArray manifest;
  std::vector<BY_HANDLE_FILE_INFORMATION> identities;
  Progress progress;
};
void clearSourceClipboard(const Snapshot &snapshot, const std::function<bool()> &cancelled)
{
  if (ClipboardUserBridge::required()) {
    networkCheck(ClipboardUserBridge::clearFiles(snapshot.sequence), "source clipboard helper unavailable");
    return;
  }
  // A newer copy, even of the same paths, belongs to the user and must survive.
  if (GetClipboardSequenceNumber() != snapshot.sequence) return;
  bool opened = false;
  for (int attempt = 0; attempt < 50 && !cancelled(); ++attempt) {
    if (OpenClipboard(snapshot.window)) { opened = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  networkCheck(opened, "source clipboard is busy");
  struct Close { ~Close() { CloseClipboard(); } } close;
  if (GetClipboardSequenceNumber() != snapshot.sequence) return;
  auto drop = GetClipboardData(CF_HDROP);
  if (!drop || DragQueryFileW(static_cast<HDROP>(drop), 0xffffffff, nullptr, 0) != snapshot.roots.size()) return;
  for (UINT i = 0; i < snapshot.roots.size(); ++i) if (dropPath(drop, i) != snapshot.roots[i]) return;
  // Keep the empty clipboard local; broadcasting it could erase a newer remote copy.
  Drop marker{GlobalAlloc(GMEM_MOVEABLE, 1)};
  auto format = RegisterClipboardFormatW(L"Deskflow Ownership");
  check(snapshot.window && marker.value && format, "source clipboard owner unavailable");
  networkCheck(EmptyClipboard() != 0, "source clipboard could not be cleared");
  check(SetClipboardData(format, marker.value) != nullptr, "source clipboard ownership failed");
  marker.value = nullptr;
}
void prepare(Snapshot &snapshot, const std::function<bool()> &cancelled, Limits limits)
{
  snapshot.progress.image = snapshot.image;
  snapshot.progress.capture = snapshot.capture;
  if (!snapshot.manifest.isEmpty()) return;
  snapshot.files.clear(); snapshot.sizes.clear();
  ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
  if (snapshot.image && snapshot.roots.empty()) {
    check(!cancelled(), "image transfer cancelled");
    snapshot.roots.push_back(ClipboardUserBridge::required() ? ClipboardUserBridge::captureImage(snapshot.sequence)
        : ClipboardImage::capture(snapshot.window, snapshot.sequence));
    snapshot.imageStaging.directory = snapshot.roots.front().parent_path();
  }
  std::vector<std::pair<fs::path, std::wstring>> pending;
  for (auto it = snapshot.roots.rbegin(); it != snapshot.roots.rend(); ++it)
    pending.push_back({*it, it->filename().wstring()});
  QJsonArray entries; size_t metadata = 0; quint64 total = 0;
  while (!pending.empty()) {
    check(!cancelled(), "file selection cancelled");
    auto [path, relative] = pending.back(); pending.pop_back();
    auto attributes = GetFileAttributesW(path.c_str());
    check(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE)),
          "linked, offline or unavailable files cannot be transferred");
    bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto size = directory ? 0 : fs::file_size(path);
    check(size <= limits.bytes, "a selected file exceeds the configured size limit");
    QJsonObject entry{{"path", QString::fromStdWString(relative)}, {"directory", directory}, {"size", qint64(size)}};
    metadata += QJsonDocument(entry).toJson(QJsonDocument::Compact).size() + 1;
    check(metadata < Converter::MaxMetadataBytes && relative.size() <= 30000, "file selection metadata is too large");
    entries.append(entry);
    if (directory) {
      for (const auto &child : fs::directory_iterator(path))
        pending.push_back({child.path(), relative + L'/' + child.path().filename().wstring()});
    } else {
      check(snapshot.files.size() < limits.files, "selection exceeds the configured file count");
      check(total <= limits.bytes && size <= limits.bytes - total, "selection exceeds the configured size limit");
      total += size;
      snapshot.files.push_back(path); snapshot.sizes.push_back(size);
    }
  }
  snapshot.identities.resize(snapshot.files.size());
  snapshot.progress = Progress{QString::fromLatin1(snapshot.token.toHex()), "send", total};
  snapshot.progress.image = snapshot.image;
  snapshot.progress.capture = snapshot.capture;
  snapshot.manifest = QJsonDocument(entries).toJson(QJsonDocument::Compact);
  check(!entries.empty() && snapshot.manifest.size() <= Converter::MaxMetadataBytes, "empty or oversized manifest");
}
struct Listener : QTcpServer {
  qintptr accepted = -1;
  void incomingConnection(qintptr descriptor) override { accepted = descriptor; }
};
struct Service {
  std::mutex mutex;
  std::condition_variable ready;
  std::shared_ptr<Snapshot> current;
  std::map<QByteArray, std::shared_ptr<Snapshot>> captures;
  std::function<void(const QJsonObject &)> captureSender;
  std::map<quint64, QJsonObject> captureReplies;
  quint64 captureRequest = 0;
  QByteArray fingerprint;
  std::atomic<bool> listening = false;
  std::atomic<quint64> generation = 0;
  std::jthread server;
  struct Job { HWND window; std::string offer; quint64 generation; bool image; fs::path videos; };
  std::unique_ptr<Job> job;
  std::deque<std::unique_ptr<Job>> captureJobs;
  std::condition_variable work;
  std::jthread receiver;
  std::jthread control;
  Service();
  void serve(std::stop_token stop);
};
Service &service() { static Service value; return value; }
QJsonObject requestCapture(QJsonObject command)
{
  auto &s = service();
  std::unique_lock lock(s.mutex);
  check(bool(s.captureSender), "the connected PC does not support capture transfer; enable sharing and update ZeroFlow on both devices");
  const auto request = ++s.captureRequest;
  command["request"] = qint64(request);
  s.captureReplies[request] = {};
  s.captureSender(command); // Enqueues on the existing sharing connection's event thread.
  s.ready.wait_for(lock, std::chrono::seconds(4), [&] { return !s.captureSender || !s.captureReplies[request].isEmpty(); });
  auto result = std::move(s.captureReplies[request]); s.captureReplies.erase(request);
  check(!result.isEmpty(), "the receiving PC did not answer the capture request");
  return result;
}

void captureControl(std::stop_token stop)
{
  // The existing SYSTEM worker owns the offer. WHService only supplies metadata;
  // file bytes still travel through serve()/fetch(), never this local control pipe.
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return;
  SECURITY_ATTRIBUTES security{sizeof security, descriptor, FALSE};
  DiskFile pipe;
  pipe.value = CreateNamedPipeW(LR"(\\.\pipe\ZeroFlow-Captures)", PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security);
  LocalFree(descriptor);
  if (pipe.value == INVALID_HANDLE_VALUE) return;
  while (!stop.stop_requested()) {
    if (!ConnectNamedPipe(pipe.value, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { Sleep(20); continue; }
    QByteArray request;
    auto deadline = GetTickCount64() + 5000;
    while (!stop.stop_requested() && GetTickCount64() < deadline && !request.contains('\n') && request.size() <= 32768) {
      DWORD available = 0, read = 0; char bytes[4096];
      if (!PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) break;
      if (!available) { Sleep(10); continue; }
      if (!ReadFile(pipe.value, bytes, qMin<DWORD>(available, sizeof bytes), &read, nullptr) || !read) break;
      request.append(bytes, int(read));
    }
    if (request.contains('\n') && request.size() <= 32768) {
      auto command = QJsonDocument::fromJson(request.left(request.indexOf('\n'))).object();
      QJsonObject response;
      command["remote"] = true;
      if (command["command"] == "capture-send" ||
          command["command"] == "capture-status" || command["command"] == "capture-limits")
        response = FileClipboardTransfer::captureCommand(command);
      else response = {{"ok", false}, {"error", "unsupported capture command"}};
      auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n'; DWORD written = 0;
      if (WriteFile(pipe.value, bytes.constData(), DWORD(bytes.size()), &written, nullptr) && written == DWORD(bytes.size())) {
        // Reader acknowledges before disconnect so unread response bytes are not discarded.
        deadline = GetTickCount64() + 5000;
        while (!stop.stop_requested() && GetTickCount64() < deadline) {
          char ack; DWORD read = 0;
          if (ReadFile(pipe.value, &ack, 1, &read, nullptr) && read) break;
          if (GetLastError() != ERROR_NO_DATA) break;
          Sleep(10);
        }
      }
    }
    DisconnectNamedPipe(pipe.value);
  }
}

void certificate(QSslCertificate &certificate, QSslKey &key)
{
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY *raw = nullptr;
  check(context && EVP_PKEY_keygen_init(context.get()) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) > 0 &&
        EVP_PKEY_keygen(context.get(), &raw) > 0, "file TLS key generation failed");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> privateKey(raw, EVP_PKEY_free);
  std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
  check(cert != nullptr, "file TLS certificate allocation failed");
  X509_set_version(cert.get(), 2); ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1);
  X509_gmtime_adj(X509_get_notBefore(cert.get()), -60); X509_gmtime_adj(X509_get_notAfter(cert.get()), 365 * 86400L);
  X509_set_pubkey(cert.get(), privateKey.get());
  auto subject = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("ZeroFlow file clipboard"), -1, -1, 0);
  X509_set_issuer_name(cert.get(), subject);
  check(X509_sign(cert.get(), privateKey.get(), EVP_sha256()) > 0, "file TLS signing failed");
  std::unique_ptr<BIO, decltype(&BIO_free)> certBio(BIO_new(BIO_s_mem()), BIO_free), keyBio(BIO_new(BIO_s_mem()), BIO_free);
  check(certBio && keyBio && PEM_write_bio_X509(certBio.get(), cert.get()) &&
        PEM_write_bio_PrivateKey(keyBio.get(), privateKey.get(), nullptr, nullptr, 0, nullptr, nullptr), "file TLS encoding failed");
  char *bytes = nullptr; auto length = BIO_get_mem_data(certBio.get(), &bytes);
  certificate = QSslCertificate(QByteArray(bytes, length));
  length = BIO_get_mem_data(keyBio.get(), &bytes); key = QSslKey(QByteArray(bytes, length), QSsl::Rsa);
}

void Service::serve(std::stop_token stop)
{
  try {
    QSslCertificate cert; QSslKey key; certificate(cert, key);
    Listener listener; check(listener.listen(QHostAddress::AnyIPv4, 24802), "file clipboard port 24802 is unavailable");
    { std::lock_guard lock(mutex); fingerprint = cert.digest(QCryptographicHash::Sha256).toHex(); listening = true; }
    ready.notify_all();
    qsizetype bytesSincePause = 0;
    while (!stop.stop_requested()) {
      listener.waitForNewConnection(100);
      if (listener.accepted == -1) continue;
      QSslSocket socket;
      socket.setSocketDescriptor(listener.accepted); listener.accepted = -1;
      if (!local(socket.peerAddress())) continue;
      socket.setReadBufferSize(ChunkBytes); socket.setLocalCertificate(cert); socket.setPrivateKey(key);
      socket.setPeerVerifyMode(QSslSocket::VerifyNone); socket.startServerEncryption();
      if (!socket.waitForEncrypted(5000)) continue;
      auto cancelled = [&] { return stop.stop_requested(); };
      std::shared_ptr<Snapshot> snapshot;
      try {
        ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
        auto request = readBytes(socket, 40, cancelled);
        bool wide = request.first(4) == "ZFR3";
        bool resume = wide || request.first(4) == "ZFR2";
        check(resume || request.first(4) == "ZFR1", "unknown file clipboard request");
        quint64 offset = 0, peerSpeed = 0, peerMaximum = Converter::MaxSelectionBytes;
        quint32 peerFiles = Converter::MaxFiles;
        if (wide) {
          auto options = readBytes(socket, 28, cancelled);
          offset = get64(options.first(8)); peerSpeed = get64(options.mid(8, 8)); peerMaximum = get64(options.mid(16, 8));
          peerFiles = get32(options.last(4));
          check(peerMaximum <= UnlimitedBytes && peerFiles > 0, "invalid transfer limits");
        } else if (resume) {
          auto options = readBytes(socket, 12, cancelled);
          offset = get32(options.first(4)); peerSpeed = get32(options.mid(4, 4)); peerMaximum = get32(options.last(4));
          check(peerMaximum <= Converter::MaxSelectionBytes && peerSpeed <= 1024, "invalid transfer limits");
        }
        { std::lock_guard lock(mutex);
          if (current && request.mid(4, 32) == current->token) snapshot = current;
          else if (auto it = captures.find(request.mid(4, 32)); it != captures.end()) snapshot = it->second;
        }
        check(snapshot != nullptr, "expired file clipboard offer");
        auto index = get32(request.last(4));
        if (resume && index == PastedIndex) {
          check(!snapshot->image && !snapshot->capture, "this transfer does not use move-on-paste");
          if (!snapshot->pasted) {
            clearSourceClipboard(*snapshot, cancelled);
            snapshot->pasted = true;
            snapshot->progress.done = snapshot->progress.total;
            snapshot->progress.report("pasted");
          }
          // Keep the completed token for idempotent retries if this reply is lost.
          sendBytes(socket, QByteArray(wide ? 8 : 4, '\0'), cancelled, bytesSincePause);
          continue;
        }
        check(!snapshot->pasted, "completed file clipboard offer");
        Limits limits{configured ? std::min(selectionLimit.load(), peerMaximum) : peerMaximum,
                      configured ? std::min(fileLimit.load(), peerFiles) : peerFiles};
        if (snapshot->capture) limits = {peerMaximum, 1};
        prepare(*snapshot, cancelled, limits);
        if (resume && index == ReadyIndex) {
          snapshot->progress.done = snapshot->progress.total;
          snapshot->progress.report(snapshot->capture ? "saved" : "ready");
          // Retain the snapshot for other peers and idempotent acknowledgements.
          // Image staging is removed when this offer is replaced or stopped.
          sendBytes(socket, QByteArray(wide ? 8 : 4, '\0'), cancelled, bytesSincePause);
          continue;
        }
        check(snapshot->progress.total <= limits.bytes && snapshot->files.size() <= limits.files, "selection exceeds configured limit");
        if (index == UINT32_MAX) {
          QByteArray length; if (wide) put64(length, snapshot->manifest.size()); else put32(length, static_cast<quint32>(snapshot->manifest.size()));
          sendBytes(socket, length, cancelled, bytesSincePause);
          sendBytes(socket, snapshot->manifest, cancelled, bytesSincePause);
        }
        else {
          check(index < snapshot->files.size(), "invalid file index");
          DiskFile file;
          file.value = CreateFileW(snapshot->files[index].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
          check(file.value != INVALID_HANDLE_VALUE && GetFileType(file.value) == FILE_TYPE_DISK, "source file could not be opened");
          BY_HANDLE_FILE_INFORMATION info{};
          check(GetFileInformationByHandle(file.value, &info) &&
                ((quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == snapshot->sizes[index] &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DIRECTORY)),
                "source file changed or is unavailable");
          auto &identity = snapshot->identities[index];
          if (identity.dwVolumeSerialNumber || identity.nFileIndexLow || identity.nFileIndexHigh) {
            check(identity.dwVolumeSerialNumber == info.dwVolumeSerialNumber && identity.nFileIndexHigh == info.nFileIndexHigh &&
                  identity.nFileIndexLow == info.nFileIndexLow &&
                  CompareFileTime(&identity.ftLastWriteTime, &info.ftLastWriteTime) == 0, "source changed during retry");
          } else identity = info;
          const auto fileSize = snapshot->sizes[index];
          check(offset <= fileSize && offset % ChunkBytes == 0, "invalid resume offset");
          LARGE_INTEGER position{}; position.QuadPart = offset;
          check(SetFilePointerEx(file.value, position, nullptr, FILE_BEGIN), "source seek failed");
          auto &progress = snapshot->progress;
          progress.done = offset;
          for (quint32 i = 0; i < index; ++i) progress.done += snapshot->sizes[i];
          if (index == 0 && offset == 0) { progress.started = Clock::now(); progress.retries = 0; }
          if (offset) ++progress.retries;
          auto filename = QString::fromStdWString(snapshot->files[index].filename().wstring());
          progress.report("transferring", filename);
          RateLimit rate{peerSpeed};
          QByteArray length; if (wide) put64(length, fileSize - offset); else put32(length, static_cast<quint32>(fileSize - offset));
          sendBytes(socket, length, cancelled, bytesSincePause);
          QByteArray buffer(ChunkBytes, Qt::Uninitialized);
          for (quint64 remaining = fileSize - offset; remaining;) {
            check(!cancelled(), "file transfer cancelled");
            auto count = static_cast<DWORD>(std::min<quint64>(remaining, ChunkBytes)); DWORD bytes = 0;
            check(ReadFile(file.value, buffer.data(), count, &bytes, nullptr) && bytes == count, "source file read failed");
            rate.wait(count, cancelled);
            sendBytes(socket, QByteArray::fromRawData(buffer.constData(), count), cancelled, bytesSincePause);
            remaining -= count;
            progress.done += count;
            progress.report("transferring", filename);
          }
          if (index + 1 == snapshot->files.size()) progress.report("sent");
        }
      } catch (const NetworkError &) {
        if (snapshot && !snapshot->pasted) snapshot->progress.report("retrying");
        socket.abort();
      } catch (const std::exception &) {
        if (snapshot && !snapshot->pasted) snapshot->progress.report("failed");
        socket.abort();
      }
    }
  } catch (const std::exception &) { listening = false; ready.notify_all(); }
}

QByteArray fetch(const QJsonObject &offer, quint32 index, quint64 maximum, const std::function<bool()> &cancelled,
                const std::function<void(const QByteArray &)> &consume = {}, quint64 expected = UINT64_MAX,
                Progress *progress = nullptr)
{
  auto token = QByteArray::fromHex(offer["token"].toString().toLatin1());
  auto fingerprint = offer["sha256"].toString().toLatin1();
  check(token.size() == 32 && fingerprint.size() == 64, "invalid file offer identity");
  auto hosts = offer["hosts"].toArray(); check(!hosts.empty() && hosts.size() <= 16, "invalid file offer addresses");
  const bool wide = offer["version"].toInt() >= 3;
  const bool resume = wide || offer["resume"].toBool();
  const auto limits = receiveLimits(offer, progress && progress->capture);
  quint64 committed = 0;
  std::string failure = "file clipboard endpoint is unavailable";
  int preferred = -1;
  // Three reconnect attempts after the initial attempt. Only complete chunks
  // committed to disk advance the resume offset; a partial read is discarded.
  for (int attempt = 0; attempt < 4; ++attempt) {
    check(!cancelled(), "file transfer cancelled");
    if (attempt) {
      if (progress) { ++progress->retries; progress->report("retrying"); }
      for (int i = 0; i < attempt * 10; ++i) {
        check(!cancelled(), "file transfer cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    }
    try {
      std::unique_ptr<QSslSocket> connection;
      for (int i = 0; i < hosts.size(); ++i) {
        int hostIndex = preferred >= 0 ? (preferred + i) % hosts.size() : i;
        check(!cancelled(), "file transfer cancelled");
        QHostAddress address(hosts[hostIndex].toString()); if (!local(address)) continue;
        auto candidate = std::make_unique<QSslSocket>(); candidate->setReadBufferSize(ChunkBytes);
        candidate->setPeerVerifyMode(QSslSocket::VerifyNone);
        candidate->connectToHostEncrypted(address.toString(), 24802);
        if (!candidate->waitForEncrypted(1500)) continue;
        // Never disclose the offer token before verifying its pinned certificate.
        if (candidate->peerCertificate().digest(QCryptographicHash::Sha256).toHex() != fingerprint) continue;
        connection = std::move(candidate); preferred = hostIndex; break;
      }
      networkCheck(connection != nullptr, "file clipboard endpoint is unavailable");
      auto &socket = *connection;
      QByteArray request(wide ? "ZFR3" : resume ? "ZFR2" : "ZFR1"); request += token; put32(request, index);
      if (wide) { put64(request, committed); put64(request, speedLimit.load()); put64(request, limits.bytes); put32(request, limits.files); }
      else if (resume) {
        put32(request, static_cast<quint32>(committed)); put32(request, static_cast<quint32>(std::min<quint64>(speedLimit.load(), 1024)));
        put32(request, static_cast<quint32>(std::min<quint64>(selectionLimit.load(), Converter::MaxSelectionBytes)));
      }
      qsizetype bytesSincePause = 0;
      sendBytes(socket, request, cancelled, bytesSincePause);
      auto size = wide ? get64(readBytes(socket, 8, cancelled)) : quint64(get32(readBytes(socket, 4, cancelled)));
      check(size <= maximum, "oversized file response");
      if (!consume) return readBytes(socket, size, cancelled);
      check(size == expected - (resume ? committed : 0), "file response does not match manifest");
      // Older senders can be retried safely by skipping the already staged prefix.
      if (!resume) {
        for (quint64 skip = committed; skip;) {
          auto count = std::min<quint64>(skip, ChunkBytes);
          readBytes(socket, count, cancelled); skip -= count; size -= count;
        }
      }
      RateLimit rate;
      while (size) {
        auto count = std::min<quint64>(size, ChunkBytes);
        auto chunk = readBytes(socket, count, cancelled);
        if (!resume) rate.wait(count, cancelled);
        check(!cancelled(), "file transfer cancelled");
        consume(chunk); // Disk errors are permanent and must not be retried.
        committed += count; size -= count;
        if (progress) { progress->done += count; progress->report("transferring"); }
      }
      return {};
    } catch (const NetworkError &error) { failure = error.what(); }
  }
  throw NetworkError(failure);
}

Service::Service() : server([this](std::stop_token stop) { serve(stop); }), receiver([this](std::stop_token stop) {
  HWND publishedWindow = nullptr;
  std::vector<fs::path> publishedPaths;
  QJsonObject publishedOffer;
  auto nextPasteCheck = Clock::now();
  while (!stop.stop_requested()) {
    std::unique_ptr<Job> next;
    { std::unique_lock lock(mutex); work.wait_for(lock, std::chrono::milliseconds(100), [&] { return job != nullptr || !captureJobs.empty(); });
      if (!captureJobs.empty()) { next = std::move(captureJobs.front()); captureJobs.pop_front(); }
      else next = std::move(job);
    }
    if (!next) {
      if (publishedPaths.empty() || Clock::now() < nextPasteCheck) continue;
      nextPasteCheck = Clock::now() + std::chrono::milliseconds(500);
      ClipboardDesktopUser user;
      if (!user.valid) continue;
      bool moved = true;
      for (const auto &path : publishedPaths) {
        std::error_code error;
        if (fs::exists(path, error) || error) { moved = false; break; }
      }
      if (!moved) {
        if (GetClipboardOwner() != publishedWindow) publishedPaths.clear();
        continue;
      }
      if (!OpenClipboard(publishedWindow)) continue;
      bool pasted = false;
      // Recheck under the clipboard lock: a newer copy must never be cleared.
      try {
        auto drop = GetClipboardData(CF_HDROP);
        bool same = GetClipboardOwner() == publishedWindow && drop &&
            DragQueryFileW(static_cast<HDROP>(drop), 0xffffffff, nullptr, 0) == publishedPaths.size();
        for (UINT i = 0; same && i < publishedPaths.size(); ++i) same = dropPath(drop, i) == publishedPaths[i];
        if (!same) pasted = true; // Paste completed, but preserve the receiver's newer clipboard.
        else if (EmptyClipboard()) {
          pasted = true;
          auto marker = GlobalAlloc(GMEM_MOVEABLE, 1);
          if (marker && !SetClipboardData(RegisterClipboardFormatW(L"Deskflow Ownership"), marker)) GlobalFree(marker);
        }
      } catch (const std::exception &) { }
      CloseClipboard();
      if (pasted) {
        HeadlessStatus::transfer({{"id", publishedOffer["token"]}, {"direction", "receive"}, {"state", "pasted"}});
        if (publishedOffer["resume"].toBool()) {
          try { fetch(publishedOffer, PastedIndex, 0, [&] { return stop.stop_requested(); }); }
          catch (const std::exception &) { }
        }
      }
      if (pasted) publishedPaths.clear();
      continue;
    }
    const bool capture = !next->videos.empty();
    if (!capture) publishedPaths.clear();
    auto cancelled = [&] { return stop.stop_requested() || (!capture &&
      (generation != next->generation || GetClipboardOwner() != next->window)); };
    try {
      ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
      Drop staged{FileClipboardTransfer::receiveToTemp(next->offer, cancelled, next->image, capture)};
      check(staged.value != nullptr, "file transfer failed");
      Staging cleanup{dropPath(staged.value).parent_path()};
      std::vector<fs::path> paths;
      auto count = DragQueryFileW(static_cast<HDROP>(staged.value), 0xffffffff, nullptr, 0);
      for (UINT i = 0; i < count; ++i) paths.push_back(dropPath(staged.value, i));
      if (capture) {
        auto descriptor = QJsonDocument::fromJson(QByteArray::fromStdString(next->offer.substr(4))).object();
        check(paths.size() == 1 && fs::is_regular_file(paths.front()), "capture offer must contain one file");
        auto relative = captureRelative(descriptor["relativePath"].toString());
        check(paths.front().filename() == relative.filename(), "capture filename does not match destination");
        ClipboardDesktopUser savingUser; check(savingUser.valid, "capture save identity unavailable");
        auto saved = saveCapture(paths.front(), next->videos, relative);
        HeadlessStatus::transfer({{"id", descriptor["token"]}, {"direction", "receive"}, {"capture", true},
          {"state", "saved"}, {"path", QString::fromStdWString(saved.wstring())}});
        try { fetch(descriptor, ReadyIndex, 0, [&] { return stop.stop_requested(); }); }
        catch (const std::exception &) { /* The saved capture remains available if the sender disconnects. */ }
        continue;
      }
      Drop image;
      if (next->image) {
        check(paths.size() == 1 && paths.front().extension() == L".dib", "invalid clipboard image selection");
        image.value = ClipboardImage::load(paths.front());
      }
      bool opened = false;
      for (int attempt = 0; attempt < 50 && !cancelled(); ++attempt) {
        if (OpenClipboard(next->window)) { opened = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!opened) throw std::runtime_error("clipboard is busy");
      bool published = false;
      if (!cancelled() && EmptyClipboard()) {
        if (next->image) {
          if (SetClipboardData(CF_DIB, image.value)) {
            image.value = nullptr; published = true;
            auto marker = GlobalAlloc(GMEM_MOVEABLE, 1);
            if (marker && !SetClipboardData(RegisterClipboardFormatW(L"Deskflow Ownership"), marker)) GlobalFree(marker);
          }
        } else {
        auto effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        auto value = effect ? static_cast<DWORD *>(GlobalLock(effect)) : nullptr;
        if (value) {
          *value = DROPEFFECT_MOVE; GlobalUnlock(effect);
          if (SetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"), effect)) {
            if (SetClipboardData(CF_HDROP, staged.value)) {
              published = true;
              staged.value = nullptr; cleanup.directory.clear();
              publishedWindow = next->window; publishedPaths = std::move(paths);
              auto marker = GlobalAlloc(GMEM_MOVEABLE, 1);
              if (marker && !SetClipboardData(RegisterClipboardFormatW(L"Deskflow Ownership"), marker)) GlobalFree(marker);
            }
          } else GlobalFree(effect);
        } else if (effect) GlobalFree(effect);
        }
      }
      CloseClipboard();
      check(published, "clipboard publication cancelled or failed");
      auto descriptor = QJsonDocument::fromJson(QByteArray::fromStdString(next->offer.substr(4))).object();
      publishedOffer = descriptor;
      if (!next->image) HeadlessStatus::transfer({{"id", descriptor["token"]}, {"direction", "receive"}, {"state", "ready"}});
      if (descriptor["resume"].toBool()) {
        try { fetch(descriptor, ReadyIndex, 0, [&] { return stop.stop_requested(); }); }
        catch (const std::exception &) { /* Ready locally even if acknowledgement cannot be delivered. */ }
      }
    } catch (const std::exception &error) {
      auto descriptor = QJsonDocument::fromJson(QByteArray::fromStdString(next->offer.substr(4))).object();
      if (!next->image) HeadlessStatus::transfer({{"id", descriptor["token"]}, {"direction", "receive"}, {"capture", capture},
        {"state", cancelled() ? "cancelled" : "failed"}, {"error", QString::fromUtf8(error.what())}});
      OutputDebugStringW(L"ZeroFlow file clipboard transfer failed; input connection is unaffected.\n"); }
  }
}) {}
} // namespace

void FileClipboardTransfer::configure(quint64 speedMiB, quint64 selectionMiB, quint32 files)
{
  check(selectionMiB <= UnlimitedBytes / ChunkBytes && speedMiB <= UnlimitedBytes / ChunkBytes && files < PastedIndex, "invalid transfer limits");
  speedLimit = speedMiB;
  selectionLimit = selectionMiB ? selectionMiB * ChunkBytes : UnlimitedBytes;
  fileLimit = files ? files : PastedIndex - 1;
  configured = true;
}
void FileClipboardTransfer::start()
{
  auto &s = service();
  if (ClipboardUserBridge::required() && !s.control.joinable()) s.control = std::jthread(captureControl);
}
void FileClipboardTransfer::stop()
{
  auto &s = service(); s.generation++; s.receiver.request_stop(); s.work.notify_all(); s.server.request_stop();
  s.control.request_stop(); if (s.control.joinable()) s.control.join();
  if (s.receiver.joinable()) s.receiver.join(); if (s.server.joinable()) s.server.join();
  s.current.reset();
  s.captures.clear(); s.captureJobs.clear();
  ClipboardUserBridge::stop();
}
QJsonObject FileClipboardTransfer::captureCommand(const QJsonObject &command)
{
  try {
    const auto action = command["command"].toString();
    if (command["remote"].toBool() && (action == "capture-status" || action == "capture-limits")) {
      auto request = command; request.remove("remote");
      auto result = requestCapture(request);
      if (result["ok"].toBool() && action == "capture-limits") {
        // Local clipboard limits do not cap captures. Preserve an older peer's
        // reported capture cap until that peer has also been updated.
        const auto peerSpeed = result["bytesPerSecond"].toInteger();
        const auto ownSpeed = qint64(speedLimit.load() * ChunkBytes);
        result["bytesPerSecond"] = peerSpeed && ownSpeed ? std::min(peerSpeed, ownSpeed) : std::max(peerSpeed, ownSpeed);
        const auto peerFiles = result["maxFiles"].toInteger();
        result["maxFiles"] = std::min(peerFiles, qint64(1));
      } else if (result["ok"].toBool() && action == "capture-status") {
        auto progress = result; progress.remove("request"); progress.remove("ok"); progress["direction"] = "send";
        HeadlessStatus::transfer(progress);
      }
      return result;
    }
    if (action == "capture-send") {
      // Capability is negotiated on the existing ZeroFlow connection before any
      // metadata is sent, so old peers never interpret captures as clipboard data.
      { auto &s = service(); std::lock_guard lock(s.mutex); check(bool(s.captureSender), "the PC is offline or its ZeroFlow needs updating"); }
      auto source = command; source["command"] = "capture-offer"; source.remove("remote");
      auto offer = captureCommand(source);
      if (!offer["ok"].toBool()) return offer;
      QJsonObject response;
      try { response = requestCapture({{"command", "capture-receive"}, {"id", offer["id"]}, {"offer", offer["offer"]}}); }
      catch (const std::exception &error) { response = {{"ok", false}, {"error", QString::fromUtf8(error.what())}}; }
      if (!response["ok"].toBool()) {
        HeadlessStatus::transfer({{"id", offer["id"]}, {"direction", "send"}, {"capture", true}, {"state", "failed"}, {"error", response["error"]}});
        response["id"] = offer["id"]; return response;
      }
      offer.remove("offer"); // The service/overlay never needs the transfer secret.
      return offer;
    }
    if (action == "capture-limits") return {{"ok", true}, {"maximumBytes", qint64(0)},
      {"maxFiles", qint64(1)}, {"bytesPerSecond", qint64(speedLimit.load() * ChunkBytes)}};
    if (action == "capture-status") {
      auto progress = HeadlessStatus::captureProgress(command["id"].toString());
      check(!progress.isEmpty(), "capture transfer is unknown or its runtime restarted");
      progress["ok"] = true; return progress;
    }
    ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
    auto &s = service();
    if (action == "capture-offer") {
      check(s.listening, "ZeroFlow file transfer is not ready");
      const auto path = fs::path(command["path"].toString().toStdWString());
      const auto relative = captureRelative(command["relativePath"].toString());
      check(path.is_absolute(), "capture source must be absolute");
      const auto videos = knownFolder(FOLDERID_Videos);
      const auto screenshots = knownFolder(FOLDERID_Pictures) / L"Screenshots";
      auto same = [](const fs::path &a, const fs::path &b) {
        return _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
      };
      const bool screenshot = relative.begin()->wstring() == L"Screenshots" &&
        same(path, screenshots.parent_path() / relative);
      const bool video = same(path, videos / relative);
      check(video || screenshot, "capture source does not match its Videos-relative location");
      noLinks(path, video ? videos : screenshots);
      check(fs::is_regular_file(path), "capture source is unavailable");
      auto snapshot = std::make_shared<Snapshot>(); snapshot->capture = true;
      snapshot->roots.push_back(path); snapshot->token.resize(32);
      check(RAND_bytes(reinterpret_cast<unsigned char *>(snapshot->token.data()), 32) == 1, "capture identity failed");
      const auto id = QString::fromLatin1(snapshot->token.toHex());
      prepare(*snapshot, [] { return false; }, {UnlimitedBytes, 1});
      QJsonArray hosts;
      for (const auto &address : QNetworkInterface::allAddresses())
        if (local(address) && !address.isLoopback() && hosts.size() < 16) hosts.append(address.toString());
      check(!hosts.empty(), "no local-network address for capture transfer");
      QJsonObject offer;
      { std::lock_guard lock(s.mutex);
        for (auto it = s.captures.begin(); it != s.captures.end();) {
          auto state = HeadlessStatus::captureProgress(QString::fromLatin1(it->first.toHex()))["state"].toString();
          if (state == "saved" || state == "failed" || state == "cancelled") it = s.captures.erase(it); else ++it;
        }
        check(s.captures.size() < 8, "too many pending capture transfers");
        s.captures[snapshot->token] = snapshot;
        offer = {{"token", id}, {"sha256", QString::fromLatin1(s.fingerprint)}, {"hosts", hosts}, {"resume", true},
          {"version", 3}, {"policy", true}, {"maximum", qint64(UnlimitedBytes)},
          {"files", qint64(1)}, {"capture", true}, {"relativePath", command["relativePath"]}};
      }
      snapshot->progress.report("preparing", QString::fromStdWString(path.filename().wstring()));
      return {{"ok", true}, {"id", id}, {"offer", QString::fromStdString("ZFR1" + QJsonDocument(offer).toJson(QJsonDocument::Compact).toStdString())},
        {"relativePath", command["relativePath"]}, {"total", qint64(snapshot->progress.total)}, {"state", "preparing"}};
    }
    if (action == "capture-receive") {
      const auto text = command["offer"].toString().toStdString();
      check(text.starts_with("ZFR1") && text.size() <= 8192, "invalid capture offer");
      auto offer = QJsonDocument::fromJson(QByteArray::fromStdString(text.substr(4))).object();
      check(offer["capture"].toBool() && offer["version"].toInt() >= 3 && offer["token"] == command["id"], "invalid capture identity");
      check(offer["token"].toString().size() == 64 && QByteArray::fromHex(offer["token"].toString().toLatin1()).size() == 32, "invalid capture token");
      captureRelative(offer["relativePath"].toString());
      auto videos = knownFolder(FOLDERID_Videos);
      { std::lock_guard lock(s.mutex);
        if (!HeadlessStatus::captureProgress(command["id"].toString()).isEmpty())
          return {{"ok", true}, {"id", command["id"]}, {"state", "accepted"}};
        check(s.captureJobs.size() < 8, "capture receive queue is full");
        HeadlessStatus::transfer({{"id", command["id"]}, {"capture", true}, {"direction", "receive"}, {"state", "preparing"}});
        s.captureJobs.push_back(std::make_unique<Service::Job>(Service::Job{nullptr, text, 0, false, videos}));
      }
      s.work.notify_one();
      return {{"ok", true}, {"id", command["id"]}, {"state", "preparing"}};
    }
    throw std::runtime_error("unsupported capture command");
  } catch (const std::exception &error) { return {{"ok", false}, {"error", QString::fromUtf8(error.what())}}; }
}
void FileClipboardTransfer::setCaptureSender(std::function<void(const QJsonObject &)> send)
{
  auto &s = service();
  { std::lock_guard lock(s.mutex); s.captureSender = std::move(send); }
  s.ready.notify_all();
}
void FileClipboardTransfer::captureResponse(const QJsonObject &response)
{
  auto &s = service();
  { std::lock_guard lock(s.mutex);
    auto it = s.captureReplies.find(quint64(response["request"].toInteger()));
    if (it != s.captureReplies.end()) it->second = response;
  }
  s.ready.notify_all();
}
void FileClipboardTransfer::cancelReceive() { service().generation++; }
std::string FileClipboardTransfer::offer(HANDLE drop, HWND window)
{
  auto &s = service(); if (!s.listening) return {};
  auto snapshot = std::make_shared<Snapshot>(); snapshot->token.resize(32);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(snapshot->token.data()), 32) != 1) return {};
  auto count = DragQueryFileW(static_cast<HDROP>(drop), 0xffffffff, nullptr, 0);
  if (!count || count > Converter::MaxMetadataBytes / 9) return {};
  for (UINT i = 0; i < count; ++i) snapshot->roots.push_back(dropPath(drop, i));
  snapshot->sequence = GetClipboardSequenceNumber();
  snapshot->window = window;
  QJsonArray hosts;
  for (const auto &address : QNetworkInterface::allAddresses())
    if (local(address) && !address.isLoopback() && hosts.size() < 16) hosts.append(address.toString());
  if (hosts.empty()) hosts.append("127.0.0.1");
  QJsonObject result;
  { std::lock_guard lock(s.mutex);
    if (s.current && !s.current->image && s.current->sequence == snapshot->sequence && s.current->roots == snapshot->roots) snapshot = s.current;
    else s.current = snapshot;
    result = {{"token", QString::fromLatin1(snapshot->token.toHex())}, {"sha256", QString::fromLatin1(s.fingerprint)}, {"hosts", hosts}, {"resume", true}, {"version", 3}, {"policy", configured.load()},
              {"maximum", qint64(selectionLimit.load())}, {"files", qint64(fileLimit.load())}}; }
  return "ZFR1" + QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
std::string FileClipboardTransfer::offerImage(HWND window, DWORD sequence)
{
  auto &s = service(); if (!s.listening || !sequence) return {};
  auto snapshot = std::make_shared<Snapshot>(); snapshot->token.resize(32);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(snapshot->token.data()), 32) != 1) return {};
  snapshot->sequence = sequence; snapshot->window = window; snapshot->image = true;
  QJsonArray hosts;
  for (const auto &address : QNetworkInterface::allAddresses())
    if (local(address) && !address.isLoopback() && hosts.size() < 16) hosts.append(address.toString());
  if (hosts.empty()) hosts.append("127.0.0.1");
  QJsonObject result;
  { std::lock_guard lock(s.mutex);
    if (s.current && s.current->image && s.current->sequence == sequence) snapshot = s.current;
    else s.current = snapshot;
    result = {{"token", QString::fromLatin1(snapshot->token.toHex())}, {"sha256", QString::fromLatin1(s.fingerprint)}, {"hosts", hosts},
              {"resume", true}, {"version", 3}, {"policy", configured.load()},
              {"maximum", qint64(selectionLimit.load())}, {"files", qint64(fileLimit.load())}}; }
  return "ZFR1" + QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
void FileClipboardTransfer::receiveAsync(HWND window, const std::string &offer, bool image)
{
  if (!offer.starts_with("ZFR1") || offer.size() > 8192) return;
  auto &s = service();
  { std::lock_guard lock(s.mutex); s.job = std::make_unique<Service::Job>(Service::Job{window, offer, ++s.generation, image}); }
  s.work.notify_one();
}
HANDLE FileClipboardTransfer::receiveToTemp(const std::string &descriptor, const std::function<bool()> &cancelled, bool image, bool capture)
{
  check(descriptor.starts_with("ZFR1") && descriptor.size() <= 8192, "invalid file offer");
  ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
  auto offer = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.substr(4))).object();
  check(!offer["capture"].toBool() || capture, "capture offers require an explicit capture request");
  const auto limits = receiveLimits(offer, capture);
  Progress progress{offer["token"].toString(), "receive"};
  progress.image = image;
  progress.capture = capture;
  progress.report("preparing");
  try {
  auto manifest = QJsonDocument::fromJson(fetch(offer, UINT32_MAX, Converter::MaxMetadataBytes, cancelled, {}, UINT32_MAX, &progress));
  check(manifest.isArray() && !manifest.array().empty(), "invalid file manifest");
  auto entries = manifest.array(); QByteArray skeleton("ZFC1"); put32(skeleton, static_cast<quint32>(entries.size()));
  if (progress.capture) check(entries.size() == 1 && !entries[0].toObject()["directory"].toBool(), "capture offer must contain one file");
  std::vector<std::pair<QString, quint64>> files; quint64 total = 0;
  for (const auto &value : entries) {
    check(value.isObject(), "invalid file entry"); auto entry = value.toObject();
    check(entry["path"].isString() && entry["directory"].isBool() && entry["size"].isDouble(), "invalid file metadata");
    auto size = entry["size"].toInteger(-1); bool directory = entry["directory"].toBool();
    check(size >= 0 && quint64(size) <= limits.bytes && (!directory || size == 0), "invalid file size");
    auto name = entry["path"].toString().toUtf8(); put32(skeleton, static_cast<quint32>(name.size()));
    put32(skeleton, directory ? UINT32_MAX : 0); skeleton += name;
    if (!directory) {
      check(total <= limits.bytes && quint64(size) <= limits.bytes - total, "file selection exceeds size limit");
      total += size; files.push_back({entry["path"].toString(), quint64(size)});
    }
    check(files.size() <= limits.files, "file selection exceeds file count limit");
  }
  progress.total = total;
  progress.report("transferring");
  Drop staged{Converter(limits.files).fromIClipboard(skeleton.toStdString())};
  check(staged.value != nullptr, "unsafe file paths or failed staging");
  Staging cleanup{dropPath(staged.value).parent_path()};
  for (quint32 i = 0; i < files.size(); ++i) {
    check(!cancelled(), "file transfer cancelled");
    auto destination = cleanup.directory / files[i].first.toStdWString();
    DiskFile file;
    file.value = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    check(file.value != INVALID_HANDLE_VALUE, "could not open staged file");
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(file.value, &info) &&
          !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "staged file changed");
    fetch(offer, i, limits.bytes, cancelled, [&](const QByteArray &bytes) {
      DWORD written = 0;
      check(WriteFile(file.value, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size(), "staged file write failed");
    }, files[i].second, &progress);
  }
  check(!cancelled(), "file transfer cancelled");
  progress.report("staged");
  auto result = staged.value; staged.value = nullptr; cleanup.directory.clear(); return result;
  } catch (const std::exception &) {
    progress.report(cancelled() ? "cancelled" : "failed");
    throw;
  }
}
