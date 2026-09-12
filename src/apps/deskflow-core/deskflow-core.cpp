/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 * Headless supervisor entry point; no application-specific integration.
 */
#include "WindowsService.h"
#include "arch/Arch.h"
#include "arch/win32/ArchMiscWindows.h"
#include "base/EventQueue.h"
#include "base/HeadlessStatus.h"
#include "base/HeadlessControl.h"
#include "base/Log.h"
#include "common/Constants.h"
#include "common/ExitCodes.h"
#include "common/Settings.h"
#include "deskflow/ClientApp.h"
#include "deskflow/ServerApp.h"
#include "server/Server.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "net/SecureUtils.h"
#include "platform/FileClipboardTransfer.h"
#include "platform/ClipboardUserBridge.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QUdpSocket>
#include <QUuid>
#include <memory>

int main(int argc, char **argv)
{
  int clipboardResult = ClipboardUserBridge::dispatch(argc, argv);
  if (clipboardResult != -1) return clipboardResult;
  int serviceResult = dispatchWindowsService(argc, argv);
  if (serviceResult != -1) return serviceResult;
  ArchMiscWindows::setInstanceWin32(GetModuleHandle(nullptr));
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("ZeroFlow");
  FileClipboardTransfer::start();
  QObject::connect(&app, &QCoreApplication::aboutToQuit, [] { FileClipboardTransfer::stop(); });
  Arch arch;
  arch.init();
  Log log;
  log.setFilter(LogLevel::Level::Warning);
  // A GUI-subsystem executable has no console; use inherited handles directly.
  qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
    if (type == QtDebugMsg || type == QtInfoMsg) return;
    const auto bytes = message.toUtf8() + '\n';
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), bytes.constData(), DWORD(bytes.size()), &written, nullptr);
    if (type == QtFatalMsg) abort();
  });
  QCommandLineParser parser;
  parser.setApplicationDescription("Windows headless keyboard/mouse sharing. TLS peers are automatically trusted.");
  parser.addHelpOption();
  parser.addOptions({
    {"server", "Share this desktop's keyboard and mouse."},
    {"client", "Receive a server's keyboard and mouse."},
    {"headless", "Headless mode (always enabled in this executable)."},
    {"status-json", "Emit schema-1 newline-delimited JSON status on stdout."},
    {"control-stdin", "Accept a JSON stop command on stdin; stop on EOF."},
    {{"s", "settings"}, "Private settings path (required).", "file"},
    {"host", "Server address; otherwise use saved host or ZeroFlow discovery.", "host"},
    {"port", "Sharing port (default 24800).", "port"},
    {"name", "Screen name (default machine name).", "name"}
  });
  parser.addPositionalArgument("mode", "Optional legacy server/client mode.", "[mode]");
  if (!parser.parse(app.arguments())) { QTextStream(stderr) << parser.errorText() << '\n'; return s_exitArgs; }
  if (parser.isSet("help")) { QTextStream(stdout) << parser.helpText(); return 0; }
  auto modes = parser.positionalArguments();
  if (parser.isSet("server")) modes << "server";
  if (parser.isSet("client")) modes << "client";
  if (modes.size() != 1 || (modes[0] != "server" && modes[0] != "client") || !parser.isSet("settings") ||
      !QFileInfo(parser.value("settings")).isAbsolute()) {
    QTextStream(stderr) << "Choose exactly one --server/--client mode and an absolute --settings path.\n";
    return s_exitArgs;
  }
  const bool server = modes[0] == "server";
  bool portValid = true;
  const int port = parser.isSet("port") ? parser.value("port").toInt(&portValid) : 24800;
  if (!portValid || port < 1 || port > 65535 || (server && parser.isSet("host"))) return s_exitArgs;
  QSharedMemory instance(QString::fromUtf8(kCoreBinName));
  if (instance.attach()) instance.detach();
  if (!instance.create(1)) { QTextStream(stderr) << "A sharing core already owns this session.\n"; return s_exitDuplicate; }
  HeadlessStatus::start(modes[0], parser.isSet("status-json"));
  const QFileInfo settingsFile(parser.value("settings"));
  if (!QDir().mkpath(settingsFile.absolutePath())) return s_exitConfig;
  Settings::setSettingsFile(settingsFile.absoluteFilePath());
  Settings::setValue(Settings::Log::Level, "WARNING");
  Settings::setStateFile(settingsFile.absolutePath() + "/state.ini");
  Settings::setValue(Settings::Core::ProcessMode, Settings::Desktop);
  Settings::setValue(Settings::Core::CoreMode, server ? Settings::Server : Settings::Client);
  if (parser.isSet("name")) Settings::setValue(Settings::Core::ComputerName, parser.value("name"));
  const auto name = Settings::value(Settings::Core::ComputerName).toString();
  // Screen names enter the plain-text layout format.
  if (name.isEmpty() || name.contains(QRegularExpression("[^A-Za-z0-9_.-]"))) return s_exitArgs;
  if (parser.isSet("port")) Settings::setValue(Settings::Core::Port, port);
  if (parser.isSet("host")) Settings::setValue(Settings::Client::RemoteHost, parser.value("host"));
  Settings::setValue(Settings::Security::TlsEnabled, true);
  Settings::setValue(Settings::Security::CheckPeers, true);
  const QString cert = settingsFile.absolutePath() + "/tls/identity.pem";
  Settings::setValue(Settings::Security::Certificate, cert);
  try {
    if (!QFileInfo::exists(cert)) {
      if (!QDir().mkpath(QFileInfo(cert).absolutePath())) return s_exitConfig;
      deskflow::generatePemSelfSignedCert(cert, 2048);
    }
  } catch (const std::exception &e) { QTextStream(stderr) << e.what() << '\n'; return s_exitConfig; }
  if (server) {
    const QString layout = settingsFile.absolutePath() + "/server.conf";
    Settings::setValue(Settings::Server::ExternalConfig, true);
    Settings::setValue(Settings::Server::ExternalConfigFile, layout);
    if (!QFileInfo::exists(layout)) {
      QSaveFile file(layout);
      if (!file.open(QIODevice::WriteOnly)) return s_exitConfig;
      file.write(("section: screens\n  " + name + ":\nend\nsection: links\nend\n").toUtf8());
      if (!file.commit()) return s_exitConfig;
    }
    if (Settings::value(Settings::Server::DiscoveryId).toString().isEmpty())
      Settings::setValue(Settings::Server::DiscoveryId, QUuid::createUuid().toString(QUuid::WithoutBraces));
  }
  Settings::save();
  if (!Settings::isWritable()) return s_exitConfig;

  EventQueue events;
  std::unique_ptr<App> core;
  bool stopping = false;
  QThread coreThread;
  deskflow::core::ipc::CoreIpcServer ipc(&app); // Existing core status bridge; no public IPC listener needed.
  QObject::connect(&coreThread, &QThread::finished, &app, &QCoreApplication::quit);
  auto startCore = [&] {
    if (core || stopping) return;
    if (server) core = std::make_unique<ServerApp>(&events, "ZeroFlow.exe");
    else core = std::make_unique<ClientApp>(&events, "ZeroFlow.exe");
    core->run(coreThread);
  };
  auto stop = [&] {
    stopping = true;
    if (core) core->quit(); else app.quit();
  };
  std::unique_ptr<HeadlessControl> control;
  if (parser.isSet("control-stdin")) control = std::make_unique<HeadlessControl>(stop, [&](const QJsonObject &layout) {
    if (server) events.addEvent(Event(EventTypes::ServerLayoutConfigure, events.getSystemTarget(), new Server::ScreenLayoutInfo(layout)));
  });
  QUdpSocket discovery;
  QTimer discoveryTimer;
  if (server) {
    QObject::connect(&discoveryTimer, &QTimer::timeout, &app, [&] {
      const auto payload = QString("ZEROFLOW_DISCOVERY_V2|%1|%2|%3")
        .arg(Settings::value(Settings::Server::DiscoveryId).toString(), name)
        .arg(Settings::value(Settings::Core::Port).toInt()).toUtf8();
      for (const auto &iface : QNetworkInterface::allInterfaces())
        if (iface.flags().testFlag(QNetworkInterface::IsUp) && !iface.flags().testFlag(QNetworkInterface::IsLoopBack))
          for (const auto &entry : iface.addressEntries())
            if (!entry.broadcast().isNull()) discovery.writeDatagram(payload, entry.broadcast(), 24801);
    });
    discoveryTimer.start(1000);
    startCore();
  } else if (!Settings::value(Settings::Client::RemoteHost).toString().isEmpty()) startCore();
  else {
    if (!discovery.bind(QHostAddress::AnyIPv4, 24801, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) return s_exitFailed;
    HeadlessStatus::update("connectionState", "discovering");
    QObject::connect(&discoveryTimer, &QTimer::timeout, &app, &HeadlessStatus::heartbeat);
    discoveryTimer.start(2000); // No core exists yet; discovery is the active loop.
    QObject::connect(&discovery, &QUdpSocket::readyRead, &app, [&] {
      while (discovery.hasPendingDatagrams()) {
        QByteArray data(1024, 0); QHostAddress sender;
        const auto count = discovery.readDatagram(data.data(), data.size(), &sender);
        if (count <= 0 || sender.isLoopback()) continue;
        data.resize(count); const auto parts = data.split('|');
        const bool v2 = parts.size() == 4 && parts[0] == "ZEROFLOW_DISCOVERY_V2";
        const bool v1 = parts.size() == 2 && parts[0] == "ZEROFLOW_DISCOVERY_V1";
        bool valid = false; const int discoveredPort = v2 ? parts[3].toInt(&valid) : (v1 ? parts[1].toInt(&valid) : 0);
        if (!valid || discoveredPort < 1 || discoveredPort > 65535 || core) continue;
        Settings::setValue(Settings::Client::RemoteHost, sender.toString());
        Settings::setValue(Settings::Core::Port, discoveredPort);
        Settings::save(); discoveryTimer.stop(); startCore();
      }
    });
  }
  const int result = QCoreApplication::exec();
  coreThread.wait();
  return result ? result : (core ? core->getExitCode() : 0);
}
