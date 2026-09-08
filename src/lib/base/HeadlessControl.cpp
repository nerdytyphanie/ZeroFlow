// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "HeadlessControl.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
HeadlessControl::HeadlessControl(std::function<void()> stop)
{
  QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this, stop = std::move(stop)] {
    DWORD available = 0;
    const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { m_timer.stop(); stop(); return; }
    if (!available) return;
    char buffer[512]; DWORD read = 0;
    if (!ReadFile(pipe, buffer, qMin<DWORD>(available, sizeof buffer), &read, nullptr) || !read) {
      m_timer.stop(); stop(); return;
    }
    m_input.append(buffer, int(read));
    if (m_input.size() > 1024) { m_timer.stop(); stop(); return; }
    int newline;
    while ((newline = m_input.indexOf('\n')) >= 0) {
      const auto command = QJsonDocument::fromJson(m_input.left(newline)).object();
      m_input.remove(0, newline + 1);
      if (command.value("command").toString() == "stop") { m_timer.stop(); stop(); return; }
    }
  });
  m_timer.start(100);
}
