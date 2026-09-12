// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "HeadlessControl.h"
#include "platform/FileClipboardTransfer.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <windows.h>
HeadlessControl::HeadlessControl(std::function<void()> stop, std::function<void(const QJsonObject &)> layout)
{
  QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this, stop = std::move(stop), layout = std::move(layout)] {
    DWORD available = 0;
    const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { m_timer.stop(); stop(); return; }
    if (!available) return;
    char buffer[512]; DWORD read = 0;
    if (!ReadFile(pipe, buffer, qMin<DWORD>(available, sizeof buffer), &read, nullptr) || !read) {
      m_timer.stop(); stop(); return;
    }
    m_input.append(buffer, int(read));
    if (m_input.size() > 32768) { m_timer.stop(); stop(); return; }
    int newline;
    while ((newline = m_input.indexOf('\n')) >= 0) {
      const auto command = QJsonDocument::fromJson(m_input.left(newline)).object();
      m_input.remove(0, newline + 1);
      if (command.value("command").toString() == "screen-layout" && layout) layout(command);
      if (command.value("command").toString() == "transfer-limits") {
        auto speed = command["speedMiB"].toInteger(-1), maximum = command["selectionMiB"].toInteger(-1);
        auto files = command["files"].toInteger(128);
        if (speed >= 0 && speed <= INT64_MAX / 1048576 && maximum >= 0 && maximum <= INT64_MAX / 1048576 && files >= 0 && files < UINT32_MAX - 2)
          FileClipboardTransfer::configure(speed, maximum, static_cast<quint32>(files));
      }
      if (command.value("command").toString() == "stop") { m_timer.stop(); stop(); return; }
    }
  });
  m_timer.start(100);
}
