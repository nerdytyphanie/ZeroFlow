// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QTimer>
#include <QJsonObject>
#include <QByteArray>
#include <functional>
// Local, optional supervisor channel; never part of the sharing network protocol.
class HeadlessControl {
public:
  explicit HeadlessControl(std::function<void()> stop, std::function<void(const QJsonObject &)> layout = {});
private:
  QTimer m_timer;
  QByteArray m_input;
};
