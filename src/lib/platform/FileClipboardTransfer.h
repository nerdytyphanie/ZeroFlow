/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include "platform/MSWindowsClipboard.h"
#include <functional>
#include <QtGlobal>

// File bytes never use the input connection. Only an ephemeral, TLS-pinned offer
// is carried by its existing clipboard protocol.
namespace FileClipboardTransfer {
void configure(quint64 speedMiB, quint64 selectionMiB, quint32 files = 128);
void start();
void stop();
void cancelReceive();
std::string offer(HANDLE fileDrop, HWND window = nullptr);
std::string offerImage(HWND window, DWORD sequence);
void receiveAsync(HWND window, const std::string &offer, bool image = false);
// Also used by the integration test; does not touch the system clipboard.
HANDLE receiveToTemp(const std::string &offer, const std::function<bool()> &cancelled, bool image = false);
}
