// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "HeadlessStatus.h"
#include "base/Log.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
// Process-lifetime writer. A stalled stdout reader can hold one write and one
// latest snapshot, never block the input loop or grow an unbounded queue.
struct Status {
  std::mutex mutex;
  std::condition_variable changed;
  QString role, state = "starting";
  QByteArray pending;
  bool enabled = false;
  int peers = 0;
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
        s.changed.wait(lock, [&] { return !s.pending.isEmpty(); });
        record.swap(s.pending);
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
  if (command == "connectedClients") s.peers = value.split(',', Qt::SkipEmptyParts).size();
}
void HeadlessStatus::heartbeat()
{
  auto &s = status();
  std::scoped_lock lock(s.mutex);
  if (!s.enabled) return;
  s.pending = QJsonDocument(QJsonObject{
    {"schema", 1}, {"type", "status"}, {"role", s.role}, {"state", s.state},
    {"peers", s.peers}, {"sequence", qint64(++s.sequence)}, {"uptimeMs", qint64(GetTickCount64() - s.startedAt)}
  }).toJson(QJsonDocument::Compact) + '\n';
  s.changed.notify_one();
}
