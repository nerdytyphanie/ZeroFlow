/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */
#pragma once
#include <Windows.h>

// Explorer's file clipboard can be hidden from a SYSTEM process even while
// impersonating its user. Read it in a process with that user's primary token.
namespace ClipboardUserBridge {
bool required();
// Returns a caller-owned CF_HDROP handle and the sequence it was captured from.
HANDLE readFiles(DWORD &sequence);
// Clears only the last captured selection, if that sequence is still current.
bool clearFiles(DWORD sequence);
int dispatch(int argc, char **argv);
void stop();
}
