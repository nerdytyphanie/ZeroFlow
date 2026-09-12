// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QString>
#include <QJsonObject>
namespace HeadlessStatus {
void start(const QString &role, bool enabled);
void update(const QString &command, const QString &value);
void heartbeat();
void layout(const QJsonObject &value);
void transfer(const QJsonObject &progress);
}
