// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
// Returns -1 for a normal interactive invocation; otherwise the service exit code.
int dispatchWindowsService(int &argc, char **argv);
