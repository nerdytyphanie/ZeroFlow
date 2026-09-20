// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "HeadlessStatus.h"
#include "base/Log.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <QMap>

namespace {
// Process-lifetime writer. A stalled stdout reader can hold one write and one
// latest snapshot, never block the input loop or grow an unbounded queue.
struct Status {
  std::mutex mutex;
  std::condition_variable changed;
  QString role, state = "starting";
  QByteArray pending, pendingTransfer;
  QJsonObject sending, receiving, layout;
  QMap<QString, QJsonObject> captures;
  bool enabled = false;
  int peers = 0;
  QStringList connected;
  quint64 sequence = 0;
  ULONGLONG startedAt = GetTickCount64();
};
Status &status() { static auto *value = new Status; return *value; }
}
void HeadlessStatus::start(const QString &role, bool enabled)
{
  auto &s = status();
  s.role = role;
  s.enabled = enabled;
  if (!enabled) return;
  std::thread([] {
    auto &s = status();
    for (;;) {
      QByteArray record;
      {
        std::unique_lock lock(s.mutex);
        s.changed.wait(lock, [&] { return !s.pending.isEmpty() || !s.pendingTransfer.isEmpty(); });
        record.swap(s.pending.isEmpty() ? s.pendingTransfer : s.pending);
      }
      DWORD written = 0;
      if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), record.constData(), DWORD(record.size()), &written, nullptr)
          || written != DWORD(record.size())) return;
    }
  }).detach();
  heartbeat();
}
void HeadlessStatus::update(const QString &command, const QString &value)
{
  auto &s = status();
  std::scoped_lock lock(s.mutex);
  if (command == "connectionState") {
    const auto next = value.toLower();
    if (next == "connected" && s.state != next)
      LOG_PRINT("ZeroFlow %s: connection established", qPrintable(s.role));
    s.state = next;
  }
  if (command == "connectedClients") { s.connected = value.split(',', Qt::SkipEmptyParts); s.peers = s.connected.size(); }
}
void HeadlessStatus::heartbeat()
{
  auto &s = status();
  std::scoped_lock lock(s.mutex);
  if (!s.enabled) return;
  s.pending = QJsonDocument(QJsonObject{
    {"schema", 1}, {"type", "status"}, {"role", s.role}, {"state", s.state},
    {"connectedClients", QJsonArray::fromStringList(s.connected)}, {"layout", s.layout}, {"sending", s.sending}, {"receiving", s.receiving},
    {"peers", s.peers}, {"sequence", qint64(++s.sequence)}, {"uptimeMs", qint64(GetTickCount64() - s.startedAt)}
  }).toJson(QJsonDocument::Compact) + '\n';
  s.changed.notify_one();
}

void HeadlessStatus::transfer(const QJsonObject &progress)
{
  auto &s = status();
  std::scoped_lock lock(s.mutex);
  auto &saved = progress["direction"] == "send" ? s.sending : s.receiving;
  if (saved["id"] != progress["id"]) saved = progress["capture"].toBool() ? s.captures.value(progress["id"].toString()) : QJsonObject{};
  for (auto it = progress.begin(); it != progress.end(); ++it) saved[it.key()] = it.value();
  if (saved["capture"].toBool()) {
    s.captures[saved["id"].toString()] = saved;
    while (s.captures.size() > 32) {
      auto old = s.captures.begin();
      while (old != s.captures.end()) {
        auto state = old.value()["state"].toString();
        if (old.key() != saved["id"].toString() && (state == "saved" || state == "failed" || state == "cancelled")) break;
        ++old;
      }
      if (old == s.captures.end()) break;
      s.captures.erase(old);
    }
  }
  if (!s.enabled) return;
  auto record = saved;
  record["schema"] = 1; record["type"] = "transfer"; record["role"] = s.role;
  s.pendingTransfer = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
  s.changed.notify_one();
}

void HeadlessStatus::layout(const QJsonObject &value)
{ auto &s = status(); std::scoped_lock lock(s.mutex); s.layout = value; }

QJsonObject HeadlessStatus::captureProgress(const QString &id)
{ auto &s = status(); std::scoped_lock lock(s.mutex); return s.captures.value(id); }
