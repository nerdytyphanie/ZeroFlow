/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include "platform/MSWindowsClipboard.h"
#include <functional>

// File bytes never use the input connection. Only an ephemeral, TLS-pinned offer
// is carried by its existing clipboard protocol.
namespace FileClipboardTransfer {
void start();
void stop();
void cancelReceive();
std::string offer(HANDLE fileDrop);
void receiveAsync(HWND window, const std::string &offer);
// Also used by the integration test; does not touch the system clipboard.
HANDLE receiveToTemp(const std::string &offer, const std::function<bool()> &cancelled);
}
