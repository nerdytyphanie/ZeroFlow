// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QTimer>
#include <QByteArray>
#include <functional>
// Local, optional supervisor channel; never part of the sharing network protocol.
class HeadlessControl {
public:
  explicit HeadlessControl(std::function<void()> stop);
private:
  QTimer m_timer;
  QByteArray m_input;
};
