/*
 * SPDX-FileCopyrightText: (C) 2026 ZeroFlow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/FileClipboardTransfer.h"
#include "platform/MSWindowsClipboardFilesConverter.h"
#include <shellapi.h>
#include <shlobj.h>
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

namespace {
namespace fs = std::filesystem;
using Converter = MSWindowsClipboardFilesConverter;
using Clock = std::chrono::steady_clock;
constexpr qsizetype ChunkBytes = 1024 * 1024;
constexpr qsizetype PauseBytes = 64 * ChunkBytes;
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
struct DiskFile {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~DiskFile() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void put32(QByteArray &data, quint32 n)
{ for (int shift = 24; shift >= 0; shift -= 8) data.append(char(n >> shift)); }
quint32 get32(const QByteArray &data)
{ quint32 n = 0; for (int i = 0; i < 4; ++i) n = (n << 8) | static_cast<unsigned char>(data[i]); return n; }
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
      check(socket.bytesAvailable() || (socket.state() != QAbstractSocket::UnconnectedState && Clock::now() < deadline), "file transfer timed out");
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
    check(socket.write(data.constData() + offset, count) == count, "file socket write failed");
    while (socket.bytesToWrite()) {
      check(!cancelled() && socket.waitForBytesWritten(1000), "file socket stopped accepting data");
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
struct Staging {
  fs::path directory;
  ~Staging() { if (!directory.empty()) { std::error_code ignored; fs::remove_all(directory, ignored); } }
};
struct Snapshot {
  DWORD sequence = 0;
  QByteArray token;
  std::vector<fs::path> roots;
  std::vector<fs::path> files;
  std::vector<quint32> sizes;
  QByteArray manifest;
};
void prepare(Snapshot &snapshot, const std::function<bool()> &cancelled)
{
  if (!snapshot.manifest.isEmpty()) return;
  snapshot.files.clear(); snapshot.sizes.clear();
  ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
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
    check(size <= Converter::MaxFileBytes, "a selected file exceeds 384 MiB");
    QJsonObject entry{{"path", QString::fromStdWString(relative)}, {"directory", directory}, {"size", qint64(size)}};
    metadata += QJsonDocument(entry).toJson(QJsonDocument::Compact).size() + 1;
    check(metadata < Converter::MaxMetadataBytes && relative.size() <= 30000, "file selection metadata is too large");
    entries.append(entry);
    if (directory) {
      for (const auto &child : fs::directory_iterator(path))
        pending.push_back({child.path(), relative + L'/' + child.path().filename().wstring()});
    } else {
      check(snapshot.files.size() < Converter::MaxFiles, "a selection exceeds 128 files");
      total += size;
      check(total <= Converter::MaxSelectionBytes, "selection exceeds 384 MiB");
      snapshot.files.push_back(path); snapshot.sizes.push_back(static_cast<quint32>(size));
    }
  }
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
  QByteArray fingerprint;
  std::atomic<bool> listening = false;
  std::atomic<quint64> generation = 0;
  std::jthread server;
  struct Job { HWND window; std::string offer; quint64 generation; };
  std::unique_ptr<Job> job;
  std::condition_variable work;
  std::jthread receiver;
  Service();
  void serve(std::stop_token stop);
};
Service &service() { static Service value; return value; }

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
      try {
        ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
        auto request = readBytes(socket, 40, cancelled);
        check(request.first(4) == "ZFR1", "unknown file clipboard request");
        std::shared_ptr<Snapshot> snapshot;
        { std::lock_guard lock(mutex); snapshot = current; }
        check(snapshot && request.mid(4, 32) == snapshot->token, "expired file clipboard offer");
        prepare(*snapshot, cancelled);
        auto index = get32(request.last(4));
        if (index == UINT32_MAX) {
          QByteArray length; put32(length, static_cast<quint32>(snapshot->manifest.size()));
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
          check(GetFileInformationByHandle(file.value, &info) && !info.nFileSizeHigh &&
                info.nFileSizeLow == snapshot->sizes[index] &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_DIRECTORY)),
                "source file changed or is unavailable");
          QByteArray length; put32(length, info.nFileSizeLow);
          sendBytes(socket, length, cancelled, bytesSincePause);
          QByteArray buffer(ChunkBytes, Qt::Uninitialized);
          for (quint32 remaining = info.nFileSizeLow; remaining;) {
            check(!cancelled(), "file transfer cancelled");
            auto count = std::min<quint32>(remaining, static_cast<quint32>(ChunkBytes)); DWORD bytes = 0;
            check(ReadFile(file.value, buffer.data(), count, &bytes, nullptr) && bytes == count, "source file read failed");
            sendBytes(socket, QByteArray::fromRawData(buffer.constData(), count), cancelled, bytesSincePause);
            remaining -= count;
          }
        }
      } catch (const std::exception &) { socket.abort(); }
    }
  } catch (const std::exception &) { listening = false; ready.notify_all(); }
}

QByteArray fetch(const QJsonObject &offer, quint32 index, qsizetype maximum, const std::function<bool()> &cancelled,
                const std::function<void(const QByteArray &)> &consume = {}, quint32 expected = UINT32_MAX)
{
  auto token = QByteArray::fromHex(offer["token"].toString().toLatin1());
  auto fingerprint = offer["sha256"].toString().toLatin1();
  check(token.size() == 32 && fingerprint.size() == 64, "invalid file offer identity");
  auto hosts = offer["hosts"].toArray(); check(!hosts.empty() && hosts.size() <= 16, "invalid file offer addresses");
  for (const auto &host : hosts) {
    check(!cancelled(), "file transfer cancelled");
    QHostAddress address(host.toString()); if (!local(address)) continue;
    QSslSocket socket; socket.setReadBufferSize(ChunkBytes);
    socket.setPeerVerifyMode(QSslSocket::VerifyNone);
    socket.connectToHostEncrypted(address.toString(), 24802);
    if (!socket.waitForEncrypted(1500)) continue;
    // The random token is disclosed only after checking the certificate delivered
    // over the existing TLS clipboard connection. No public CA or automatic trust.
    if (socket.peerCertificate().digest(QCryptographicHash::Sha256).toHex() != fingerprint) continue;
    QByteArray request("ZFR1"); request += token; put32(request, index);
    qsizetype bytesSincePause = 0;
    sendBytes(socket, request, cancelled, bytesSincePause);
    auto size = get32(readBytes(socket, 4, cancelled));
    check(size <= maximum, "oversized file response");
    if (consume) {
      check(size == expected, "file response does not match manifest");
      while (size) {
        auto count = std::min<quint32>(size, static_cast<quint32>(ChunkBytes));
        consume(readBytes(socket, count, cancelled)); size -= count;
      }
      return {};
    }
    return readBytes(socket, size, cancelled);
  }
  throw std::runtime_error("file clipboard endpoint is unavailable");
}

Service::Service() : server([this](std::stop_token stop) { serve(stop); }), receiver([this](std::stop_token stop) {
  HWND publishedWindow = nullptr;
  std::vector<fs::path> publishedPaths;
  auto nextPasteCheck = Clock::now();
  while (!stop.stop_requested()) {
    std::unique_ptr<Job> next;
    { std::unique_lock lock(mutex); work.wait_for(lock, std::chrono::milliseconds(100), [&] { return job != nullptr; }); next = std::move(job); }
    if (!next) {
      if (publishedPaths.empty() || Clock::now() < nextPasteCheck) continue;
      nextPasteCheck = Clock::now() + std::chrono::milliseconds(500);
      if (GetClipboardOwner() != publishedWindow) { publishedPaths.clear(); continue; }
      ClipboardDesktopUser user;
      if (!user.valid) continue;
      bool moved = true;
      for (const auto &path : publishedPaths) {
        std::error_code error;
        if (fs::exists(path, error) || error) { moved = false; break; }
      }
      if (!moved || !OpenClipboard(publishedWindow)) continue;
      // Recheck under the clipboard lock: a newer copy must never be cleared.
      try {
        auto drop = GetClipboardData(CF_HDROP);
        bool same = GetClipboardOwner() == publishedWindow && drop &&
            DragQueryFileW(static_cast<HDROP>(drop), 0xffffffff, nullptr, 0) == publishedPaths.size();
        for (UINT i = 0; same && i < publishedPaths.size(); ++i) same = dropPath(drop, i) == publishedPaths[i];
        if (same && EmptyClipboard()) {
          auto marker = GlobalAlloc(GMEM_MOVEABLE, 1);
          if (marker && !SetClipboardData(RegisterClipboardFormatW(L"Deskflow Ownership"), marker)) GlobalFree(marker);
        }
      } catch (const std::exception &) { }
      CloseClipboard();
      publishedPaths.clear();
      continue;
    }
    publishedPaths.clear();
    auto cancelled = [&] { return stop.stop_requested() || generation != next->generation || GetClipboardOwner() != next->window; };
    try {
      ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
      Drop staged{FileClipboardTransfer::receiveToTemp(next->offer, cancelled)};
      check(staged.value != nullptr, "file transfer failed");
      Staging cleanup{dropPath(staged.value).parent_path()};
      std::vector<fs::path> paths;
      auto count = DragQueryFileW(static_cast<HDROP>(staged.value), 0xffffffff, nullptr, 0);
      for (UINT i = 0; i < count; ++i) paths.push_back(dropPath(staged.value, i));
      bool opened = false;
      for (int attempt = 0; attempt < 50 && !cancelled(); ++attempt) {
        if (OpenClipboard(next->window)) { opened = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!opened) continue;
      if (!cancelled() && EmptyClipboard()) {
        auto effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        auto value = effect ? static_cast<DWORD *>(GlobalLock(effect)) : nullptr;
        if (value) {
          *value = DROPEFFECT_MOVE; GlobalUnlock(effect);
          if (SetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"), effect)) {
            if (SetClipboardData(CF_HDROP, staged.value)) {
              staged.value = nullptr; cleanup.directory.clear();
              publishedWindow = next->window; publishedPaths = std::move(paths);
              auto marker = GlobalAlloc(GMEM_MOVEABLE, 1);
              if (marker && !SetClipboardData(RegisterClipboardFormatW(L"Deskflow Ownership"), marker)) GlobalFree(marker);
            }
          } else GlobalFree(effect);
        } else if (effect) GlobalFree(effect);
      }
      CloseClipboard();
    } catch (const std::exception &) { OutputDebugStringW(L"ZeroFlow file clipboard transfer failed; input connection is unaffected.\n"); }
  }
}) {}
} // namespace

void FileClipboardTransfer::start() { (void)service(); }
void FileClipboardTransfer::stop()
{
  auto &s = service(); s.generation++; s.receiver.request_stop(); s.work.notify_all(); s.server.request_stop();
  if (s.receiver.joinable()) s.receiver.join(); if (s.server.joinable()) s.server.join();
}
void FileClipboardTransfer::cancelReceive() { service().generation++; }
std::string FileClipboardTransfer::offer(HANDLE drop)
{
  auto &s = service(); if (!s.listening) return {};
  auto snapshot = std::make_shared<Snapshot>(); snapshot->token.resize(32);
  if (RAND_bytes(reinterpret_cast<unsigned char *>(snapshot->token.data()), 32) != 1) return {};
  auto count = DragQueryFileW(static_cast<HDROP>(drop), 0xffffffff, nullptr, 0);
  if (!count || count > Converter::MaxMetadataBytes / 9) return {};
  for (UINT i = 0; i < count; ++i) snapshot->roots.push_back(dropPath(drop, i));
  snapshot->sequence = GetClipboardSequenceNumber();
  QJsonArray hosts;
  for (const auto &address : QNetworkInterface::allAddresses())
    if (local(address) && !address.isLoopback() && hosts.size() < 16) hosts.append(address.toString());
  if (hosts.empty()) hosts.append("127.0.0.1");
  QJsonObject result;
  { std::lock_guard lock(s.mutex);
    if (s.current && s.current->sequence == snapshot->sequence && s.current->roots == snapshot->roots) snapshot = s.current;
    else s.current = snapshot;
    result = {{"token", QString::fromLatin1(snapshot->token.toHex())}, {"sha256", QString::fromLatin1(s.fingerprint)}, {"hosts", hosts}}; }
  return "ZFR1" + QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
}
void FileClipboardTransfer::receiveAsync(HWND window, const std::string &offer)
{
  if (!offer.starts_with("ZFR1") || offer.size() > 8192) return;
  auto &s = service();
  { std::lock_guard lock(s.mutex); s.job = std::make_unique<Service::Job>(Service::Job{window, offer, ++s.generation}); }
  s.work.notify_one();
}
HANDLE FileClipboardTransfer::receiveToTemp(const std::string &descriptor, const std::function<bool()> &cancelled)
{
  check(descriptor.starts_with("ZFR1") && descriptor.size() <= 8192, "invalid file offer");
  ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
  auto offer = QJsonDocument::fromJson(QByteArray::fromStdString(descriptor.substr(4))).object();
  auto manifest = QJsonDocument::fromJson(fetch(offer, UINT32_MAX, Converter::MaxMetadataBytes, cancelled));
  check(manifest.isArray() && !manifest.array().empty(), "invalid file manifest");
  auto entries = manifest.array(); QByteArray skeleton("ZFC1"); put32(skeleton, static_cast<quint32>(entries.size()));
  std::vector<std::pair<QString, quint32>> files; quint64 total = 0;
  for (const auto &value : entries) {
    check(value.isObject(), "invalid file entry"); auto entry = value.toObject();
    check(entry["path"].isString() && entry["directory"].isBool() && entry["size"].isDouble(), "invalid file metadata");
    auto size = entry["size"].toDouble(); bool directory = entry["directory"].toBool();
    check(size >= 0 && size <= Converter::MaxFileBytes && size == quint32(size) && (!directory || size == 0), "invalid file size");
    auto name = entry["path"].toString().toUtf8(); put32(skeleton, static_cast<quint32>(name.size()));
    put32(skeleton, directory ? UINT32_MAX : 0); skeleton += name;
    if (!directory) { total += quint32(size); files.push_back({entry["path"].toString(), quint32(size)}); }
    check(files.size() <= Converter::MaxFiles && total <= Converter::MaxSelectionBytes, "file selection exceeds limits");
  }
  Drop staged{Converter().fromIClipboard(skeleton.toStdString())};
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
    fetch(offer, i, Converter::MaxFileBytes, cancelled, [&](const QByteArray &bytes) {
      DWORD written = 0;
      check(WriteFile(file.value, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
            written == bytes.size(), "staged file write failed");
    }, files[i].second);
  }
  check(!cancelled(), "file transfer cancelled");
  auto result = staged.value; staged.value = nullptr; cleanup.directory.clear(); return result;
}
