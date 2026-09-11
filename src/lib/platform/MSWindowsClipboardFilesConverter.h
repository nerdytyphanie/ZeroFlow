/*
 * ZeroFlow -- mouse, keyboard and clipboard sharing
 * SPDX-FileCopyrightText: (C) 2026 ZeroFlow contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "platform/MSWindowsClipboard.h"
#include <filesystem>

// Restrict file clipboard work in a SYSTEM input worker to its desktop user's rights.
class ClipboardDesktopUser
{
public:
  ClipboardDesktopUser();
  ~ClipboardDesktopUser();
  bool valid = false;
  std::filesystem::path temp() const;
private:
  HANDLE token = nullptr;
  bool impersonating = false;
};

class MSWindowsClipboardFilesConverter : public IMSWindowsClipboardConverter
{
public:
  static constexpr size_t MaxSelectionBytes = 384 * 1024 * 1024;
  static constexpr size_t MaxFileBytes = MaxSelectionBytes;
  static constexpr size_t MaxMetadataBytes = 3 * 1024 * 1024;
  // Used only on the background file channel, never the input/clipboard connection.
  static constexpr size_t MaxPayloadBytes = MaxFileBytes + MaxMetadataBytes;
  static constexpr size_t MaxFiles = 128;

  IClipboard::Format getFormat() const override { return IClipboard::Format::Files; }
  UINT getWin32Format() const override { return CF_HDROP; }
  HANDLE fromIClipboard(const std::string &data) const override;
  std::string toIClipboard(HANDLE data) const override;
};
