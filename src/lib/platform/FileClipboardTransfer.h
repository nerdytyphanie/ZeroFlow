/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include "platform/MSWindowsClipboard.h"
#include <functional>
#include <QtGlobal>
#include <QJsonObject>

// File bytes never use the input connection. Only an ephemeral, TLS-pinned offer
// is carried by its existing clipboard protocol.
namespace FileClipboardTransfer {
void configure(quint64 speedMiB, quint64 selectionMiB, quint32 files = 128, bool filesEnabled = true);
void start();
void stop();
void cancelReceive();
// Explicit captures reuse the TLS stream and retry machinery without the file
// clipboard's selection-size cap. Speed limits still apply.
// The local service pipe sends through the existing negotiated sharing link.
QJsonObject captureCommand(const QJsonObject &command);
void setCaptureSender(std::function<void(const QJsonObject &)> send);
void captureResponse(const QJsonObject &response);
std::string offer(HANDLE fileDrop, HWND window = nullptr);
std::string offerImage(HWND window, DWORD sequence);
void receiveAsync(HWND window, const std::string &offer, bool image = false);
// Also used by the integration test; does not touch the system clipboard.
HANDLE receiveToTemp(const std::string &offer, const std::function<bool()> &cancelled, bool image = false, bool capture = false);
}
