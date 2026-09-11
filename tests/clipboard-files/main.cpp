/*
 * SPDX-FileCopyrightText: (C) 2026 ZeroFlow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "platform/MSWindowsClipboardFilesConverter.h"
#include "platform/FileClipboardTransfer.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <chrono>
#include <thread>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
struct Drop {
  HANDLE value;
  ~Drop() { if (value) GlobalFree(value); }
};
HANDLE drop(const std::vector<fs::path> &paths)
{
  std::wstring list;
  for (const auto &path : paths) { list += path.wstring(); list.push_back(0); }
  list.push_back(0);
  auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
  auto header = static_cast<DROPFILES *>(GlobalLock(memory));
  header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
  memcpy(reinterpret_cast<char *>(header) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  GlobalUnlock(memory);
  return memory;
}
fs::path first(HANDLE value)
{
  wchar_t path[32768];
  require(DragQueryFileW(static_cast<HDROP>(value), 0, path, 32768) != 0, "missing staged path");
  return path;
}
void put32(std::string &value, uint32_t n)
{ for (int shift = 24; shift >= 0; shift -= 8) value.push_back(static_cast<char>(n >> shift)); }
std::string packet(std::string name, std::string bytes)
{
  std::string value = "ZFC1"; put32(value, 1); put32(value, static_cast<uint32_t>(name.size()));
  put32(value, static_cast<uint32_t>(bytes.size())); return value + name + bytes;
}
std::string read(const fs::path &path)
{ std::ifstream file(path, std::ios::binary); return {std::istreambuf_iterator<char>(file), {}}; }
QByteArray hash(const fs::path &path)
{
  std::ifstream file(path, std::ios::binary);
  QCryptographicHash digest(QCryptographicHash::Sha256);
  std::vector<char> buffer(1024 * 1024);
  while (file) { file.read(buffer.data(), buffer.size()); digest.addData(QByteArrayView(buffer.data(), file.gcount())); }
  return digest.result();
}
std::string networkOffer(HANDLE selection)
{
  std::string offer;
  for (int attempt = 0; attempt < 100 && offer.empty(); ++attempt) {
    offer = FileClipboardTransfer::offer(selection);
    if (offer.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  require(!offer.empty(), "file TLS server did not start");
  auto descriptor = QJsonDocument::fromJson(QByteArray::fromStdString(offer.substr(4))).object();
  descriptor["hosts"] = QJsonArray{"127.0.0.1"};
  return "ZFR1" + QJsonDocument(descriptor).toJson(QJsonDocument::Compact).toStdString();
}

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  FileClipboardTransfer::start();
  struct Stop { ~Stop() { FileClipboardTransfer::stop(); } } stop;
  try {
    wchar_t temp[32768]; GetTempPathW(32768, temp);
    GUID id; CoCreateGuid(&id); wchar_t guid[40]; StringFromGUID2(id, guid, 40);
    auto root = fs::path(temp) / (std::wstring(L"ZeroFlow-Clipboard-Test-") + guid);
    fs::create_directories(root / "source" / "nested" / "empty");
    std::ofstream(root / "source" / "nested" / "file.txt", std::ios::binary) << "clipboard file bytes";
    std::ofstream(root / "standalone.txt", std::ios::binary) << "second file";
    MSWindowsClipboardFilesConverter converter;
    Drop source{drop({root / "source", root / "standalone.txt"})};
    auto encoded = converter.toIClipboard(source.value);
    require(!encoded.empty(), "folder capture failed");
    Drop staged{converter.fromIClipboard(encoded)};
    require(staged.value != nullptr, "folder staging failed");
    require(DragQueryFileW(static_cast<HDROP>(staged.value), 0xffffffff, nullptr, 0) == 2, "only selection roots belong on clipboard");
    auto folder = first(staged.value);
    require(read(folder / "nested" / "file.txt") == "clipboard file bytes", "file contents changed");
    require(fs::is_directory(folder / "nested" / "empty"), "empty directory lost");
    require(fs::equivalent(folder.parent_path().parent_path(), fs::path(temp)), "staging is not in ordinary temp");
    require(MoveFileW(folder.c_str(), (root / "pasted").c_str()) != 0, "move from staging failed");
    require(!fs::exists(folder) && fs::exists(root / "pasted" / "nested" / "file.txt"), "paste did not move staged folder");
    require(fs::exists(root / "source" / "nested" / "file.txt"), "sender original was removed");
    fs::remove_all(folder.parent_path());

    for (const auto &name : {"../escape", "C:/escape", "/absolute", "bad:stream", "NUL.txt", "bad.", "bad/../escape", "missing/parent"})
      require(converter.fromIClipboard(packet(name, "x")) == nullptr, "unsafe path accepted");
    auto duplicate = packet("a.txt", "x"); duplicate[7] = 2;
    duplicate += packet("A.TXT", "y").substr(8);
    require(converter.fromIClipboard(duplicate) == nullptr, "case-insensitive duplicate accepted");
    require(converter.fromIClipboard(encoded.substr(0, encoded.size() - 1)) == nullptr, "truncated payload accepted");
    require(converter.fromIClipboard(encoded + "extra") == nullptr, "trailing bytes accepted");

    auto boundary = packet("a", std::string(3 * 1024 * 1024, 'x'));
    Drop exact{converter.fromIClipboard(boundary)};
    require(exact.value != nullptr, "exact 3 MiB transfer rejected");
    fs::remove_all(first(exact.value).parent_path());

    std::vector<fs::path> many;
    for (int i = 0; i < 129; ++i) {
      auto path = root / ("small-" + std::to_string(i)); std::ofstream(path) << ""; many.push_back(path);
    }
    Drop tooMany{drop(many)};
    require(converter.toIClipboard(tooMany.value).empty(), "129-file selection accepted");
    many.pop_back(); Drop allowed{drop(many)};
    auto allowedPacket = converter.toIClipboard(allowed.value);
    require(!allowedPacket.empty(), "128-file selection rejected");
    Drop allowedReceive{converter.fromIClipboard(allowedPacket)};
    require(allowedReceive.value != nullptr, "128-file receive failed");
    fs::remove_all(first(allowedReceive.value).parent_path());
    // Exercise the real TLS file channel with a selection larger than the normal
    // 3 MiB clipboard limit, while every individual file remains within its limit.
    fs::create_directories(root / "network" / "empty");
    for (int i = 0; i < 2; ++i) {
      std::ofstream output(root / "network" / ("large-" + std::to_string(i)), std::ios::binary);
      output << std::string(3000000, char('a' + i));
    }
    Drop network{drop({root / "network"})}; auto offer = networkOffer(network.value);
    Drop transferred{FileClipboardTransfer::receiveToTemp(offer, [] { return false; })};
    auto received = first(transferred.value);
    require(read(received / "large-0") == std::string(3000000, 'a') && read(received / "large-1") == std::string(3000000, 'b'), "TLS transfer changed file bytes");
    require(fs::is_directory(received / "empty"), "TLS transfer lost empty folder");
    fs::remove_all(received.parent_path());
    bool cancelled = false;
    try { Drop unused{FileClipboardTransfer::receiveToTemp(offer, [] { return true; })}; }
    catch (const std::exception &) { cancelled = true; }
    require(cancelled, "cancelled transfer completed");

    // Stream the exact aggregate boundary without constructing a full-file packet.
    auto large = root / "large.bin";
    { std::ofstream output(large, std::ios::binary); output << "first chunk"; }
    fs::resize_file(large, converter.MaxSelectionBytes);
    { std::fstream output(large, std::ios::binary | std::ios::in | std::ios::out);
      output.seekp(converter.MaxSelectionBytes - 10); output << "last chunk"; }
    Drop largeDrop{drop({large})};
    auto largeOffer = networkOffer(largeDrop.value);
    Drop largeReceived{FileClipboardTransfer::receiveToTemp(largeOffer, [] { return false; })};
    auto largePath = first(largeReceived.value);
    require(fs::file_size(largePath) == converter.MaxSelectionBytes && hash(largePath) == hash(large), "384 MiB streamed file changed");
    fs::remove_all(largePath.parent_path());
    { std::ofstream(root / "one-byte") << 'x'; }
    Drop overTotal{drop({large, root / "one-byte"})};
    bool rejected = false;
    try { Drop unused{FileClipboardTransfer::receiveToTemp(networkOffer(overTotal.value), [] { return false; })}; }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "selection exceeding 384 MiB accepted");
    fs::resize_file(large, converter.MaxSelectionBytes + 1);
    rejected = false;
    try { Drop unused{FileClipboardTransfer::receiveToTemp(networkOffer(largeDrop.value), [] { return false; })}; }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "single file exceeding 384 MiB accepted");
    fs::remove_all(root);
    std::cout << "PASS: folders, move, originals, malformed paths, 128 files, real TLS multi-file transfer, cancellation, streamed 384 MiB boundary and aggregate overflow\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
