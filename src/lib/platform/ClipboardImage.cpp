/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#include "platform/ClipboardImage.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardFilesConverter.h"
#include <QFile>
#include <QUuid>
#include <cstring>
#include <stdexcept>

namespace {
void check(bool ok, const char *reason) { if (!ok) throw std::runtime_error(reason); }
void validate(const std::string &data) {
  check(data.size() >= sizeof(BITMAPINFOHEADER), "truncated clipboard image");
  BITMAPINFOHEADER header{}; memcpy(&header, data.data(), sizeof(header));
  check(header.biSize >= sizeof(header) && header.biSize <= data.size() && header.biWidth > 0 && header.biHeight != 0 &&
        header.biPlanes == 1 && (header.biBitCount == 24 || header.biBitCount == 32) && header.biCompression == BI_RGB,
        "unsupported clipboard image");
  const auto height = header.biHeight < 0 ? -int64_t(header.biHeight) : int64_t(header.biHeight);
  const auto stride = ((uint64_t(header.biWidth) * header.biBitCount + 31) / 32) * 4;
  const auto offset = uint64_t(header.biSize) + uint64_t(header.biClrUsed) * 4;
  check(offset <= data.size() && uint64_t(height) <= (data.size() - offset) / stride, "truncated clipboard pixels");
}
}
std::filesystem::path ClipboardImage::capture(HWND window, DWORD sequence) {
  ClipboardDesktopUser user; check(user.valid, "desktop identity unavailable");
  bool opened = false;
  for (int i = 0; i < 50; ++i) { if (OpenClipboard(window)) { opened = true; break; } Sleep(10); }
  check(opened, "image clipboard is busy");
  std::string data;
  try {
    check(GetClipboardSequenceNumber() == sequence, "image clipboard changed");
    auto handle = GetClipboardData(CF_DIB);
    check(handle != nullptr, "clipboard image unavailable");
    data = MSWindowsClipboardBitmapConverter().toIClipboard(handle);
    check(GetClipboardSequenceNumber() == sequence, "image clipboard changed");
  } catch (...) { CloseClipboard(); throw; }
  CloseClipboard();
  validate(data);
  const auto temp = user.temp(); check(!temp.empty(), "image temp folder unavailable");
  const auto directory = temp / (L"ZeroFlow-Image-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdWString());
  check(std::filesystem::create_directory(directory), "image temp folder creation failed");
  const auto path = directory / L"Clipboard image.dib";
  try {
    QFile file(QString::fromStdWString(path.wstring()));
    check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "image temp file creation failed");
    check(file.write(data.data(), qint64(data.size())) == qint64(data.size()) && file.flush(), "image temp write failed");
    return path;
  } catch (...) { std::error_code error; std::filesystem::remove_all(directory, error); throw; }
}
HANDLE ClipboardImage::load(const std::filesystem::path &path) {
  QFile file(QString::fromStdWString(path.wstring()));
  check(file.open(QIODevice::ReadOnly), "received image unavailable");
  auto bytes = file.readAll();
  check(file.error() == QFileDevice::NoError, "received image read failed");
  auto data = bytes.toStdString(); validate(data);
  auto handle = MSWindowsClipboardBitmapConverter().fromIClipboard(data);
  check(handle != nullptr, "received image allocation failed");
  return handle;
}
