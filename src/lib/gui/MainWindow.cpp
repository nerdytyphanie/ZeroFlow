/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2024 - 2026 Chris Rizzitello <sithord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "Diagnostic.h"
#include "StyleUtils.h"

#include "dialogs/AboutDialog.h"
#include "dialogs/ClientConfigDialog.h"
#include "dialogs/FingerprintDialog.h"
#include "dialogs/HelpDialog.h"
#include "dialogs/ServerConfigDialog.h"
#include "dialogs/SettingsDialog.h"

#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "common/UrlConstants.h"
#include "common/VersionInfo.h"
#include "gui/Messages.h"
#include "gui/TlsUtility.h"
#include "gui/core/CoreProcess.h"
#include "gui/ipc/DaemonIpcClient.h"
#include "gui/widgets/LogDock.h"
#include "net/FingerprintDatabase.h"
#include "widgets/StatusBar.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDateTime>
#include <QFileDialog>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#include <algorithm>
#include <memory>

#if defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace deskflow::gui;

MainWindow::MainWindow(FixedRole role)
    : ui{std::make_unique<Ui::MainWindow>()},
      m_fixedRole{role},
      m_guiSocketName{QStringLiteral("%1-gui").arg(QCoreApplication::applicationName())},
      m_coreProcess(m_serverConfig),
      m_trayIcon{new QSystemTrayIcon(this)},
      m_guiDupeChecker{new QLocalServer(this)},
      m_daemonIpcClient{new ipc::DaemonIpcClient(this)},
      m_logDock{new LogDock(this)},
      m_statusBar{new StatusBar(this)},
      m_menuFile{new QMenu(this)},
      m_menuEdit{new QMenu(this)},
      m_menuView{new QMenu(this)},
      m_menuHelp{new QMenu(this)},
      m_actionAbout{new QAction(this)},
      m_actionMinimize{new QAction(this)},
      m_actionQuit{new QAction(this)},
      m_actionTrayQuit{new QAction(this)},
      m_actionRestore{new QAction(this)},
      m_actionSettings{new QAction(this)},
      m_actionStartCore{new QAction(this)},
      m_actionRestartCore{new QAction(this)},
      m_actionStopCore{new QAction(this)},
      m_actionShowHelp{new QAction(this)},
      m_networkMonitor{new NetworkMonitor(this)},
      m_discoverySocket{new QUdpSocket(this)},
      m_discoveryTimer{new QTimer(this)},
      m_discoveryFallbackTimer{new QTimer(this)}
{
  ui->setupUi(this);

  setWindowIcon(QIcon::fromTheme(kRevFqdnName));

  addDockWidget(Qt::BottomDockWidgetArea, m_logDock);

  // Setup Actions
  m_actionAbout->setMenuRole(QAction::AboutRole);
  m_actionAbout->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::HelpAbout));

  m_actionMinimize->setIcon(QIcon::fromTheme(QStringLiteral("window-minimize-pip")));
  m_actionRestore->setIcon(QIcon::fromTheme(QStringLiteral("window-restore-pip")));

  if (!deskflow::platform::isWindows()) {
    m_actionQuit->setShortcut(QKeySequence::Quit);
    m_actionTrayQuit->setShortcut(QKeySequence::Quit);
  }

  m_actionQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionQuit->setMenuRole(QAction::QuitRole);

  m_actionTrayQuit->setIcon(QIcon::fromTheme("application-exit"));
  m_actionTrayQuit->setMenuRole(QAction::NoRole);

  m_actionSettings->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  m_actionSettings->setMenuRole(QAction::PreferencesRole);
  m_actionSettings->setShortcut(QKeySequence::Preferences);

  m_actionStartCore->setIcon(QIcon::fromTheme(QStringLiteral("system-run")));
  m_actionStartCore->setMenuRole(QAction::NoRole);

  m_actionRestartCore->setVisible(false);
  m_actionRestartCore->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
  m_actionRestartCore->setMenuRole(QAction::NoRole);

  m_actionStopCore->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop));
  m_actionStopCore->setMenuRole(QAction::NoRole);

  m_actionShowHelp->setIcon(QIcon::fromTheme(QStringLiteral("question")));
  m_actionShowHelp->setMenuRole(QAction::NoRole);
  m_actionShowHelp->setShortcut(QKeySequence::HelpContents);

  // Setup the Instance Checking
  // In case of a previous crash remove first
  QLocalServer::removeServer(m_guiSocketName);
  m_guiDupeChecker->listen(m_guiSocketName);

  createMenuBar();
  setupControls();
  updateText();
  connectSlots();
  setupTrayIcon();
  updateScreenName();
  setHelpFilePath();

  qDebug().noquote() << "active settings path:" << Settings::settingsPath();

  // Force generation of a certificate on the host
  if (TlsUtility::isEnabled()) {
    if (Settings::value(Settings::Security::KeySize).toInt() < 2048) {
      qInfo("upgrading TLS key to the minimum 2048-bit size");
      Settings::setValue(Settings::Security::KeySize, 2048);
    }
    if (!TlsUtility::isCertValid()) {
      generateCertificate();
    } else {
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
    }
  }

  applyConfig();
  setupLanDiscovery();
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
  restoreWindow();

#ifdef Q_OS_MACOS
  // Native Quit is explicit termination on macOS; close-to-tray applies only
  // to ordinary window closing and must never veto Cmd+Q or Apple-menu Quit.
  installQuitHandler([] { return true; });
#endif
}
MainWindow::~MainWindow()
{
  // Stop network monitoring
  if (m_networkMonitor) {
    m_networkMonitor->stopMonitoring();
  }

  m_guiDupeChecker->close();
  m_coreProcess.cleanup();
}

void MainWindow::restoreWindow()
{
  auto windowGeometry = Settings::value(Settings::Gui::WindowGeometry).toRect();
  const auto totalGeometry = QGuiApplication::primaryScreen()->availableGeometry();
  if (!windowGeometry.isValid()) {
    adjustSize();
    windowGeometry = geometry();
  } else {
    setGeometry(windowGeometry);
  }
  m_expandedSize = geometry().size();

  if (!totalGeometry.contains(windowGeometry)) {
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
    move(screenGeometry.center() - rect().center());
  }

  if (!Settings::value(Settings::Gui::LogExpanded).toBool())
    setFixedSize(size());
}

void MainWindow::setupControls()
{
  secureSocket(false);

  ui->btnConfigureServer->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
  ui->btnConfigureClient->setIcon(QIcon::fromTheme(QStringLiteral("configure")));

  if (Settings::value(Settings::Core::LastVersion).toString() != kVersion) {
    Settings::setValue(Settings::Core::LastVersion, kVersion);
  }

  if (!Settings::value(Settings::Gui::LogExpanded).toBool()) {
    m_logDock->hide();
  }

  ui->serverOptions->setVisible(false);
  ui->clientOptions->setVisible(false);

  const auto coreMode = Settings::value(Settings::Core::CoreMode).value<Settings::CoreMode>();
  ui->rbModeClient->setChecked(coreMode == Settings::CoreMode::Client);
  ui->rbModeServer->setChecked(coreMode == Settings::CoreMode::Server);

  ui->lineEditName->setValidator(new QRegularExpressionValidator(m_nameRegEx, this));
  ui->lineEditName->setVisible(false);
  ui->lineEditName->installEventFilter(this);

  if (deskflow::platform::isMac()) {
    ui->rbModeServer->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->rbModeClient->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->btnSaveServerConfig->setFixedWidth(ui->btnSaveServerConfig->height());
  } else {
    ui->btnSaveServerConfig->setIconSize(QSize(22, 22));
  }
  setStatusBar(m_statusBar);
}

//////////////////////////////////////////////////////////////////////////////
// Begin slots
//////////////////////////////////////////////////////////////////////////////

// remember: using queued connection allows the render loop to run before
// executing the slot. the default is to instantly call the slot when the
// signal is emitted from the thread that owns the receiver's object.
void MainWindow::connectSlots()
{
  connect(Settings::instance(), &Settings::serverSettingsChanged, this, &MainWindow::serverConfigSaving);
  connect(Settings::instance(), &Settings::settingsChanged, this, &MainWindow::settingsChanged);

  connect(&m_coreProcess, &CoreProcess::error, this, &MainWindow::coreProcessError);
  connect(&m_coreProcess, &CoreProcess::logLine, this, &MainWindow::handleLogLine);
  connect(
      &m_coreProcess, &CoreProcess::processStateChanged, this, &MainWindow::coreProcessStateChanged,
      Qt::QueuedConnection
  );
  connect(&m_coreProcess, &CoreProcess::connectionStateChanged, this, &MainWindow::coreConnectionStateChanged);
  connect(&m_coreProcess, &CoreProcess::secureSocket, this, &MainWindow::secureSocket);
  connect(
      &m_coreProcess, &CoreProcess::daemonIpcClientConnectionFailed, this, &MainWindow::daemonIpcClientConnectionFailed
  );
  connect(&m_coreProcess, &CoreProcess::securityLevelChanged, m_statusBar, &StatusBar::setSecurityLevel);

  connect(m_actionAbout, &QAction::triggered, this, &MainWindow::openAboutDialog);
  connect(m_actionMinimize, &QAction::triggered, this, &MainWindow::hide);

  connect(m_actionQuit, &QAction::triggered, this, &MainWindow::close);
  connect(m_actionTrayQuit, &QAction::triggered, this, [this] {
    if (m_fixedRole == FixedRole::Client && m_coreProcess.processState() != ProcessState::Stopped) {
      m_coreProcess.stop();
      QTimer::singleShot(250, this, &MainWindow::close);
    } else {
      close();
    }
  });
  if (m_fixedRole == FixedRole::Server) {
    connect(m_actionRestore, &QAction::triggered, this, [this] { showConfigureServer({}); });
  }
  connect(m_actionSettings, &QAction::triggered, this, &MainWindow::openSettings);
  connect(m_actionStartCore, &QAction::triggered, this, &MainWindow::startCore);
  connect(m_actionRestartCore, &QAction::triggered, this, &MainWindow::resetCore);
  connect(m_actionStopCore, &QAction::triggered, this, &MainWindow::stopCore);
  connect(m_actionShowHelp, &QAction::triggered, this, &MainWindow::showHelpViewer);

  // Mac os tray will only show a menu
  if (!deskflow::platform::isMac())
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::trayIconActivated);

  connect(&m_coreProcess, &CoreProcess::connectedClientsChanged, this, &MainWindow::serverClientsChanged);
  connect(&m_coreProcess, &CoreProcess::unrecognisedClient, this, &MainWindow::handleUnrecognisedClient);
  connect(&m_coreProcess, &CoreProcess::connectionRefused, this, &MainWindow::handleConnectionRefused);
  connect(&m_coreProcess, &CoreProcess::retryIn, this, &MainWindow::updateTimeoutDelay);
  connect(&m_coreProcess, &CoreProcess::peerFingerprint, this, &MainWindow::handlePeerFingerprint);
  connect(&m_coreProcess, &CoreProcess::missingKeyboardLayouts, this, &MainWindow::handleMissingKeyboardLayouts);

  if (Settings::value(Settings::Gui::AutoStartCore).toBool()) {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);
  } else {
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);
  }

  connect(ui->btnRestartCore, &QPushButton::clicked, this, &MainWindow::resetCore);

  connect(ui->lineHostname, &QLineEdit::returnPressed, ui->btnRestartCore, &QPushButton::click);
  connect(ui->lineHostname, &QLineEdit::textChanged, this, &MainWindow::remoteHostChanged);

  connect(ui->btnSaveServerConfig, &QPushButton::clicked, this, &MainWindow::saveServerConfig);
  connect(ui->btnConfigureServer, &QPushButton::clicked, this, [this] { showConfigureServer(""); });
  connect(ui->btnConfigureClient, &QPushButton::clicked, this, [this] { showConfigureClient(); });
  connect(ui->lblComputerName, &QLabel::linkActivated, this, &MainWindow::openSettings);

  connect(ui->rbModeServer, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);
  connect(ui->rbModeClient, &QRadioButton::toggled, this, &MainWindow::coreModeToggled);

  connect(m_logDock->toggleViewAction(), &QAction::toggled, this, &MainWindow::toggleLogVisible);

  connect(m_statusBar, &StatusBar::requestShowMyFingerprints, this, &MainWindow::showMyFingerprint);
  connect(m_statusBar, &StatusBar::requestUpdateVersion, this, &MainWindow::openGetNewVersionUrl);
  connect(&m_versionChecker, &VersionChecker::updateFound, m_statusBar, &StatusBar::updateFound);

  connect(m_guiDupeChecker, &QLocalServer::newConnection, this, &MainWindow::showAndActivate);

  connect(ui->btnEditName, &QPushButton::clicked, this, &MainWindow::showHostNameEditor);

  connect(ui->lineEditName, &QLineEdit::editingFinished, this, &MainWindow::setHostName);

  connect(m_networkMonitor, &NetworkMonitor::ipAddressesChanged, this, &MainWindow::updateIpLabel);
}

void MainWindow::toggleLogVisible(bool visible)
{
  // When the main window is hidden e.g. close to tray / minimized), this also triggers the log visibility toggle,
  // but we don't want to hide the log in this case since we would need to un-hide it when the window is shown again.
  if (!isVisible() || isMinimized()) {
    qDebug() << "not toggling log, window not visible";
    return;
  }

  setFixedSize(16777215, 16777215);
  Settings::setValue(Settings::Gui::LogExpanded, visible);
  if (visible) {
    if (m_logDock->isFloating()) {
      adjustSize();
      setFixedSize(size());
    } else {
      QTimer::singleShot(15, this, [&] { resize(m_expandedSize); });
    }
  } else {
    if (!m_logDock->isFloating()) {
      m_expandedSize = geometry().size();
    }
    m_logDock->hide();
    if (!m_logDock->isFloating()) {
      adjustSize();
    }
    setFixedSize(size());
  }
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
}

void MainWindow::settingsChanged(const QString &key)
{
  if (key == Settings::Log::Level) {
    m_coreProcess.applyLogLevel();
    return;
  }

  if (key == Settings::Core::ComputerName)
    updateScreenName();

  if ((key == Settings::Security::Certificate) || (key == Settings::Security::KeySize) ||
      (key == Settings::Security::TlsEnabled) || (key == Settings::Security::CheckPeers)) {
    if (TlsUtility::isEnabled()) {
      if (!TlsUtility::isCertValid()) {
        qWarning() << tr("invalid certificate, generating a new one");
        TlsUtility::generateCertificate();
      }
      m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
      updateFingerprintButton();
    }
    updateSecurityIcon(m_statusBar->securityIconVisible());
    return;
  }
}

void MainWindow::serverConfigSaving()
{
  m_serverConfig.commit();
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
  if (reason != QSystemTrayIcon::Trigger)
    return;
  if (m_fixedRole == FixedRole::Server) {
    showConfigureServer({});
  }
}

void MainWindow::coreProcessError(CoreProcess::Error error)
{
  if (error == CoreProcess::Error::AddressMissing) {
    QMessageBox::warning(
        this, tr("Address missing"), tr("Please enter the hostname or IP address of the other computer.")
    );
  } else if (error == CoreProcess::Error::StartFailed) {
    show();
    auto message = tr("The Core executable could not be started.\n"
                      "Please check if you have sufficient permissions to run %1.")
                       .arg(kCoreBinName);

    if (Settings::value(Settings::Core::CoreMode) == Settings::CoreMode::Server) {
      const auto mode =
          Settings::value(Settings::Server::ExternalConfigFile).toBool() ? tr("read") : tr("read and write");
      message.append(tr("\nAdditionally, check you are able to %1 the server config file: %2")
                         .arg(mode, Settings::serverConfigFile()));
    }
    QMessageBox::warning(this, kAppName, message);
  }
}

void MainWindow::startCore()
{
  // Save current IP state when server starts
  if (m_coreProcess.mode() == CoreMode::Server && Settings::value(Settings::Core::Interface).toString().isEmpty()) {
    m_serverStartIPs = NetworkMonitor::validAddresses();
    m_serverStartSuggestedIP = m_serverStartIPs.isEmpty() ? "" : m_serverStartIPs.first();
  }

  m_actionStartCore->setVisible(false);
  m_actionRestartCore->setVisible(true);
  m_coreProcess.start();
}

void MainWindow::stopCore()
{
  qDebug() << "stopping core process";
  m_coreProcess.stop();
  m_actionStartCore->setVisible(true);
  m_actionRestartCore->setVisible(false);
}

void MainWindow::clearSettings()
{
  qDebug() << "clearing settings";

  m_networkMonitor->stopMonitoring();

  disconnect(&m_coreProcess, nullptr, this, nullptr);
  disconnect(&m_versionChecker, nullptr, this, nullptr);
  disconnect(m_guiDupeChecker, nullptr, this, nullptr);
  disconnect(m_trayIcon, nullptr, this, nullptr);
  disconnect(m_logDock->toggleViewAction(), nullptr, this, nullptr);

  m_coreProcess.stop();
  m_coreProcess.clearSettings();

  m_saveOnExit = false;
  diagnostic::clearSettings(true);
}

bool MainWindow::saveServerConfig()
{
  QString fileName = QFileDialog::getSaveFileName(this, tr("Save server configuration as..."));

  if (!fileName.isEmpty() && !m_serverConfig.save(fileName)) {
    QMessageBox::warning(this, tr("Save failed"), tr("Could not save server configuration to file."));
    return true;
  }

  return false;
}

void MainWindow::openAboutDialog()
{
  AboutDialog about(this);
  about.exec();
}

void MainWindow::openGetNewVersionUrl() const
{
  QDesktopServices::openUrl(QUrl(kUrlDownload));
}

void MainWindow::openSettings()
{
  auto dialog = SettingsDialog(this, m_serverConfig);

  connect(&dialog, &SettingsDialog::requestRemoveAllSettings, this, &MainWindow::clearSettings, Qt::UniqueConnection);
  if (dialog.exec() == QDialog::Accepted) {
    Settings::save();
    disconnect(&dialog, &SettingsDialog::requestRemoveAllSettings, nullptr, nullptr);

    applyConfig();

    if (m_coreProcess.isStarted()) {
      m_coreProcess.restart();
    }
  }
}

void MainWindow::resetCore()
{
  m_coreProcess.restart();
}

void MainWindow::showMyFingerprint()
{
  FingerprintDialog fingerprintDialog(this, m_fingerprint);
  fingerprintDialog.exec();
}

void MainWindow::coreModeToggled(bool checked)
{
  // this method is called when rbClient or rbServer toggles
  // with both being in the same group one must be turned on if the other is turned off
  // only react to toggle on to avoid calling everything twice when the user switches modes
  if (!checked)
    return;

  Settings::CoreMode mode = Settings::CoreMode::None;

  if (ui->rbModeServer->isChecked())
    mode = Settings::CoreMode::Server;
  if (ui->rbModeClient->isChecked())
    mode = Settings::CoreMode::Client;

  qDebug() << QStringLiteral("change mode to: %1").arg(QVariant::fromValue(mode).toString());

  if (m_coreProcess.isStarted() && m_coreProcess.mode() != mode)
    m_coreProcess.stop();
  m_coreProcess.setMode(mode);

  Settings::setValue(Settings::Core::CoreMode, mode);
  Settings::save();

  updateModeControls();
}

void MainWindow::updateModeControls()
{
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;

  ui->serverOptions->setVisible(isServer);
  ui->clientOptions->setVisible(isClient);
  ui->lblNoMode->setVisible(!isServer && !isClient);
  toggleCanRunCore(canRunCore());

  ui->lblIpAddresses->setVisible(
      (isClient && !Settings::value(Settings::Core::Interface).toString().isEmpty()) || isServer
  );

  if (ui->lblIpAddresses->isVisible())
    updateNetworkInfo();

  if (isServer) {
    m_networkMonitor->startMonitoring();
  } else {
    m_networkMonitor->stopMonitoring();
  }

  if (isServer || isClient)
    updateModeControlLabels();
}

void MainWindow::updateModeControlLabels()
{
  const bool isServer = m_coreProcess.mode() == CoreMode::Server;
  const bool isStarted = m_coreProcess.isStarted();

  QString startText;
  QString stopText;
  QIcon startIcon;
  QIcon stopIcon;

  if (isServer) {
    startText = tr("Start");
    stopText = tr("Stop");
    startIcon = QIcon::fromTheme(QStringLiteral("system-run"));
    stopIcon = QIcon::fromTheme(QIcon::ThemeIcon::ProcessStop);
  } else {
    startText = tr("Connect");
    stopText = tr("Disconnect");
    startIcon = QIcon::fromTheme(QStringLiteral("network-connect"));
    stopIcon = QIcon::fromTheme(QStringLiteral("network-disconnect"));
  }

  m_actionStartCore->setText(startText);
  m_actionStartCore->setIcon(startIcon);
  m_actionStopCore->setText(stopText);
  m_actionStopCore->setIcon(stopIcon);

  if (isStarted) {
    ui->btnToggleCore->setText(stopText);
    ui->btnToggleCore->setIcon(stopIcon);
  } else {
    ui->btnToggleCore->setText(startText);
    ui->btnToggleCore->setIcon(startIcon);
  }
}

void MainWindow::updateSecurityIcon(bool visible)
{
  m_statusBar->setSecurityIconVisible(visible);
  if (!visible)
    return;
  m_statusBar->setSecurityIcon(TlsUtility::isEnabled());
}

void MainWindow::updateNetworkInfo()
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  if (mode == CoreMode::Server)
    updateIpLabel(NetworkMonitor::validAddresses());
  else
    updateIpLabel({Settings::value(Settings::Core::Interface).toString()});
}

void MainWindow::serverConnectionConfigureClient(const QString &clientName)
{
  m_serverConfigDialogVisible = true;
  ServerConfigDialog dialog(this, m_serverConfig);
  if (dialog.addClient(clientName) && dialog.exec() == QDialog::Accepted) {
    m_coreProcess.restart();
  }
  m_serverConfigDialogVisible = false;
}

void MainWindow::setupLanDiscovery()
{
  constexpr quint16 discoveryPort = 24801;
  if (!m_discoverySocket->bind(
          QHostAddress::AnyIPv4, discoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint
      )) {
    qWarning("unable to bind LAN discovery socket: %s", qPrintable(m_discoverySocket->errorString()));
    return;
  }

  connect(m_discoverySocket, &QUdpSocket::readyRead, this, &MainWindow::readLanDiscovery);
  if (m_fixedRole == FixedRole::Server) {
    if (Settings::value(Settings::Server::DiscoveryId).toString().isEmpty()) {
      Settings::setValue(Settings::Server::DiscoveryId, QUuid::createUuid().toString(QUuid::WithoutBraces));
      Settings::save(false);
    }
    connect(m_discoveryTimer, &QTimer::timeout, this, &MainWindow::announceLanServer);
  } else {
    m_selectedLanServerId.clear();
    connect(m_discoveryTimer, &QTimer::timeout, this, &MainWindow::expireLanServers);
    m_discoveryFallbackTimer->setSingleShot(true);
    connect(m_discoveryFallbackTimer, &QTimer::timeout, this, [this] {
      const auto lastServerId = Settings::value(Settings::Client::LastServerId).toString();
      if (!lastServerId.isEmpty() && m_lanServers.contains(lastServerId)) {
        selectLanServer(lastServerId);
      } else if (m_lanServers.contains(m_fallbackLanServerId)) {
        selectLanServer(m_fallbackLanServerId);
      }
    });
  }
  m_discoveryTimer->start(1000);
  announceLanServer();
}

void MainWindow::announceLanServer()
{
  if (m_coreProcess.mode() != CoreMode::Server || !m_coreProcess.isStarted()) {
    return;
  }

  constexpr quint16 discoveryPort = 24801;
  const auto port = Settings::value(Settings::Core::Port).toInt();
  const auto serverId = Settings::value(Settings::Server::DiscoveryId).toString();
  const auto serverName = Settings::value(Settings::Core::ComputerName).toString();
  const auto payload = QStringLiteral("ZEROFLOW_DISCOVERY_V2|%1|%2|%3").arg(serverId, serverName).arg(port).toUtf8();
  QSet<QHostAddress> broadcasts;
  broadcasts.insert(QHostAddress::Broadcast);
  for (const auto &interface : QNetworkInterface::allInterfaces()) {
    if (!interface.flags().testFlag(QNetworkInterface::IsUp) ||
        interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
      continue;
    }
    for (const auto &entry : interface.addressEntries()) {
      if (!entry.broadcast().isNull()) {
        broadcasts.insert(entry.broadcast());
      }
    }
  }
  for (const auto &broadcast : broadcasts) {
    m_discoverySocket->writeDatagram(payload, broadcast, discoveryPort);
  }
}

void MainWindow::readLanDiscovery()
{
  while (m_discoverySocket->hasPendingDatagrams()) {
    QByteArray payload;
    payload.resize(static_cast<int>(m_discoverySocket->pendingDatagramSize()));
    QHostAddress sender;
    if (m_discoverySocket->readDatagram(payload.data(), payload.size(), &sender) < 0) {
      continue;
    }
    if (m_coreProcess.mode() != CoreMode::Client || sender.isLoopback()) {
      continue;
    }

    const auto parts = payload.split('|');
    bool validPort = false;
    const bool isV2 = parts.value(0) == QByteArrayLiteral("ZEROFLOW_DISCOVERY_V2") && parts.size() == 4;
    const bool isV1 = parts.value(0) == QByteArrayLiteral("ZEROFLOW_DISCOVERY_V1") && parts.size() == 2;
    const int port = isV2 ? parts.at(3).toInt(&validPort) : (isV1 ? parts.at(1).toInt(&validPort) : 0);
    if ((!isV1 && (!isV2 || parts.value(1).isEmpty())) || !validPort || port < 1 || port > 65535) {
      continue;
    }

    const auto host = sender.toString();
    const auto serverId = isV2 ? QString::fromUtf8(parts.at(1)) : QStringLiteral("legacy:%1:%2").arg(host).arg(port);
    const auto serverName = isV2 ? QString::fromUtf8(parts.at(2)) : host;
    m_lanServers.insert(serverId, {serverId, serverName.isEmpty() ? host : serverName, host, port,
                                   QDateTime::currentMSecsSinceEpoch()});
    refreshLanServerMenu();

    if (serverId == m_selectedLanServerId) {
      if (ui->lineHostname->text() != host || Settings::value(Settings::Core::Port).toInt() != port) {
        selectLanServer(serverId);
      } else if (m_coreProcess.processState() == ProcessState::Stopped) {
        m_coreProcess.start();
      }
      continue;
    }
    if (m_coreProcess.connectionState() == ConnectionState::Connected || !m_selectedLanServerId.isEmpty()) {
      continue;
    }

    const auto lastServerId = Settings::value(Settings::Client::LastServerId).toString();
    if (serverId == lastServerId || lastServerId.isEmpty()) {
      selectLanServer(serverId);
    } else if (!m_discoveryFallbackTimer->isActive()) {
      m_fallbackLanServerId = serverId;
      m_discoveryFallbackTimer->start(1500);
    }
  }
}

void MainWindow::selectLanServer(const QString &serverId)
{
  const auto found = m_lanServers.constFind(serverId);
  if (found == m_lanServers.cend()) {
    return;
  }

  const auto server = found.value();
  const bool changed = m_selectedLanServerId != server.id || ui->lineHostname->text() != server.host ||
                       Settings::value(Settings::Core::Port).toInt() != server.port;
  m_selectedLanServerId = server.id;
  Settings::setValue(Settings::Client::SelectedServerId, server.id);
  Settings::setValue(Settings::Client::RemoteHost, server.host);
  Settings::setValue(Settings::Core::Port, server.port);
  ui->lineHostname->setText(server.host);
  Settings::save();
  refreshLanServerMenu();

  if (m_coreProcess.processState() == ProcessState::Stopped) {
    m_coreProcess.start();
  } else if (changed) {
    m_coreProcess.restart();
  }
}

void MainWindow::refreshLanServerMenu()
{
  if (!m_lanServerMenu) {
    return;
  }
  m_lanServerMenu->clear();
  if (m_lanServers.isEmpty()) {
    auto unavailable = m_lanServerMenu->addAction(tr("No servers found"));
    unavailable->setEnabled(false);
    return;
  }
  auto servers = m_lanServers.values();
  std::sort(servers.begin(), servers.end(), [](const LanServer &left, const LanServer &right) {
    return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
  });
  for (const auto &server : servers) {
    auto action = m_lanServerMenu->addAction(QStringLiteral("%1 (%2)").arg(server.name, server.host));
    action->setCheckable(true);
    action->setChecked(server.id == m_selectedLanServerId);
    connect(action, &QAction::triggered, this, [this, serverId = server.id] { selectLanServer(serverId); });
  }
}

void MainWindow::expireLanServers()
{
  const auto cutoff = QDateTime::currentMSecsSinceEpoch() - 3500;
  for (auto it = m_lanServers.begin(); it != m_lanServers.end();) {
    if (it->lastSeen < cutoff) {
      it = m_lanServers.erase(it);
    } else {
      ++it;
    }
  }
  refreshLanServerMenu();
  if (!m_selectedLanServerId.isEmpty() && !m_lanServers.contains(m_selectedLanServerId) &&
      m_coreProcess.connectionState() != ConnectionState::Connected && !m_lanServers.isEmpty()) {
    m_selectedLanServerId.clear();
    selectLanServer(m_lanServers.cbegin().key());
  }
}

//////////////////////////////////////////////////////////////////////////////
// End slots
//////////////////////////////////////////////////////////////////////////////

void MainWindow::open()
{
  hide();

  // if a critical error was shown just before the main window (i.e. on app
  // load), it will be hidden behind the main window. therefore we need to raise
  // it up in front of the main window.
  // HACK: because the `onShown` event happens just as the window is shown, the
  // message box has a chance of being raised under the main window. to solve
  // this we delay the error dialog raise by a split second. this seems a bit
  // hacky and fragile, so maybe there's a better approach.
  const auto kCriticalDialogDelay = 100;
  QTimer::singleShot(kCriticalDialogDelay, this, &messages::raiseCriticalDialog);

  if (Settings::value(Settings::Gui::AutoUpdateCheck).toBool()) {
    m_versionChecker.checkLatest();
  } else {
    qDebug() << "skipping check for new version, disabled";
  }

  if (Settings::value(Settings::Gui::AutoStartCore).toBool()) {
    if (ui->rbModeClient->isChecked() && ui->lineHostname->text().isEmpty())
      return;
    startCore();
  }
}

void MainWindow::createMenuBar()
{
  m_menuFile->addAction(m_actionStartCore);
  m_menuFile->addAction(m_actionRestartCore);
  m_menuFile->addAction(m_actionStopCore);
  m_menuFile->addSeparator();
  m_menuFile->addAction(m_actionQuit);

  m_menuEdit->addAction(m_actionSettings);

  m_menuView->addAction(m_logDock->toggleViewAction());

  m_menuHelp->addAction(m_actionAbout);
  m_menuHelp->addAction(m_actionShowHelp);

  auto menuBar = new QMenuBar(this);
  menuBar->addMenu(m_menuFile);
  menuBar->addMenu(m_menuEdit);
  menuBar->addMenu(m_menuView);
  menuBar->addMenu(m_menuHelp);

  setMenuBar(menuBar);
}

void MainWindow::setupTrayIcon()
{
  auto trayMenu = new QMenu(this);
  if (m_fixedRole == FixedRole::Server) {
    trayMenu->addAction(m_actionRestore);
    trayMenu->addSeparator();
    trayMenu->addActions({m_actionStartCore, m_actionRestartCore, m_actionStopCore, m_actionTrayQuit});
  } else {
    m_actionRestartCore->setVisible(true);
    m_lanServerMenu = trayMenu->addMenu(tr("Servers"));
    refreshLanServerMenu();
    trayMenu->addSeparator();
    trayMenu->addActions({m_actionRestartCore, m_actionTrayQuit});
  }
  trayMenu->insertSeparator(m_actionTrayQuit);
  m_trayIcon->setContextMenu(trayMenu);

  setTrayIcon();
  m_trayIcon->show();
}

void MainWindow::applyConfig()
{
  if (Settings::value(Settings::Gui::ShowVersionInTitle).toBool()) {
    setWindowTitle(QStringLiteral("%1 - %2").arg(kAppName, kDisplayVersion));
  } else {
    setWindowTitle(kAppName);
  }

  if (const auto host = Settings::value(Settings::Client::RemoteHost).toString(); !host.isEmpty())
    ui->lineHostname->setText(host);

  updateFingerprintButton();
  setTrayIcon();

  if (const auto ip = Settings::value(Settings::Core::Interface).toString(); !ip.isEmpty()) {
    m_serverStartIPs = {ip};
    m_serverStartSuggestedIP = ip;
  }

  coreModeToggled(true);
}

void MainWindow::saveSettings() const
{
  if (ui->rbModeClient->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Client);
  } else if (ui->rbModeServer->isChecked()) {
    Settings::setValue(Settings::Core::CoreMode, Settings::CoreMode::Server);
  }
  if (!ui->lineHostname->text().isEmpty())
    Settings::setValue(Settings::Client::RemoteHost, ui->lineHostname->text());
  Settings::save();
}

void MainWindow::setTrayIcon()
{
  static const auto fallbackPath = QStringLiteral(":/icons/%1-%2/apps/64/%3");

  if (deskflow::platform::isWindows()) {
    m_trayIcon->setIcon(QIcon(QStringLiteral(":/deskflow.ico")));
    return;
  }

  if (deskflow::platform::isMac()) {
    auto icon = QIcon(QStringLiteral(":/icons/deskflow-dark/apps/64/org.deskflow.deskflow-symbolic.svg"));
    icon.setIsMask(true);
    m_trayIcon->setIcon(icon);
    return;
  }

  QString themeIcon = kRevFqdnName;
  if (!Settings::value(Settings::Gui::SymbolicTrayIcon).toBool()) {
    m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, QStringLiteral("dark"), themeIcon)));
    return;
  }

  themeIcon.append(QStringLiteral("-symbolic"));

  if (deskflow::platform::isWindows()) {
    QSettings settings(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat
    );
    const QString theme = settings.value(QStringLiteral("SystemUsesLightTheme"), 1).toBool() ? QStringLiteral("light")
                                                                                             : QStringLiteral("dark");
    m_trayIcon->setIcon(QIcon(fallbackPath.arg(kAppId, theme, themeIcon)));
    return;
  }

  auto icon = QIcon::fromTheme(themeIcon, QIcon(fallbackPath.arg(kAppId, iconMode(), themeIcon)));
  icon.setIsMask(true);
  m_trayIcon->setIcon(icon);
}

void MainWindow::handleLogLine(const QString &line)
{
  m_logDock->appendLine(line);
}

void MainWindow::handleUnrecognisedClient(const QString &clientName)
{
  if (m_ignoredClients.contains(clientName)) {
    qDebug("ignoring %s:", qPrintable(clientName));
    return;
  }

  if (m_newClientPromptShowing || m_serverConfigDialogVisible)
    return;

  if (Settings::value(Settings::Server::ExternalConfig).toBool())
    return;

  if (m_serverConfig.isFull() || m_serverConfig.screenExists(clientName))
    return;

  qInfo("automatically accepting LAN client: %s", qPrintable(clientName));
  ServerConfigDialog dialog(this, m_serverConfig);
  if (dialog.addClientAndSave(clientName)) {
    m_coreProcess.restart();
  }
}

void MainWindow::handleConnectionRefused(deskflow::core::ConnectionRefusal reason)
{
  if (reason != deskflow::core::ConnectionRefusal::AlreadyConnected)
    return;

  if (!isVisible() || m_clientErrorVisible)
    return;

  m_clientErrorVisible = true;
  showAndActivate();

  const auto address = Settings::value(Settings::Client::RemoteHost).toString();
  QMessageBox::warning(
      this, tr("%1 Connection Error").arg(kAppName),
      tr("<p>Failed to connect to the server '%1'.</p>"
         "<p>A Client with your name is already connected to the server.</p>"
         "Please ensure that you're using a unique name and that only a "
         "single instance of the client process is running.</p>")
          .arg(address)
  );

  m_clientErrorVisible = false;
}

void MainWindow::handleMissingKeyboardLayouts(const QString &layouts)
{
  if (Settings::value(Settings::Gui::IgnoreMissingKeyboardLayouts).toBool())
    return;

  QMessageBox msgBox(this);
  msgBox.setIcon(QMessageBox::Warning);
  msgBox.setWindowTitle(tr("Missing Keyboard Layouts"));
  msgBox.setText(tr("<p>Keyboard layout support requires matching layouts on all computers. "
                    "The following layouts from the other computer are not installed on this computer:</p>"
                    "<p><b>%1</b></p>"
                    "<p>Please install them to enable support for these layouts.</p>")
                     .arg(layouts));

  auto *checkBox = new QCheckBox(tr("Don't show this again"), &msgBox);
  msgBox.setCheckBox(checkBox);
  msgBox.exec();

  if (checkBox->isChecked()) {
    Settings::setValue(Settings::Gui::IgnoreMissingKeyboardLayouts, true);
  }
}

void MainWindow::handlePeerFingerprint(const QString &fingerprint)
{
  const auto sha256Text = QString(fingerprint).remove(':');
  const Fingerprint sha256 = {QCryptographicHash::Sha256, QByteArray::fromHex(sha256Text.toLatin1())};

  FingerprintDatabase db;
  db.read(trustedFingerprintDatabase());

  if (db.isTrusted(sha256)) {
    qDebug("fingerprint is trusted");
    return;
  }

  qInfo("automatically trusting LAN peer fingerprint");
  db.addTrusted(sha256);
  if (!db.write(trustedFingerprintDatabase())) {
    qCritical().noquote() << "unable to write fingerprint to db:" << trustedFingerprintDatabase();
  }
}

bool MainWindow::maybeHideToTray()
{
  if (!Settings::value(Settings::Gui::CloseToTray).toBool()) {
    return false;
  }

  if (Settings::value(Settings::Gui::CloseReminder).toBool()) {
    messages::showCloseReminder(this);
    Settings::setValue(Settings::Gui::CloseReminder, false);
  }
  Settings::setValue(Settings::Gui::WindowGeometry, geometry());
  qDebug() << "hiding to tray";
  hide();
  return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
#if !defined(Q_OS_MACOS)
  if (event->spontaneous() && maybeHideToTray()) {
    event->ignore();
    return;
  }
#endif

  if (m_saveOnExit) {
    Settings::setValue(Settings::Gui::WindowGeometry, geometry());
    Settings::setValue(Settings::Gui::AutoStartCore, m_coreProcess.isStarted());
  }
  qDebug() << "quitting application";

  // any connected dock view acitons will be triggered
  // disconnect them before accepting the event
  disconnect(m_logDock->toggleViewAction(), &QAction::toggled, nullptr, nullptr);

  event->accept();
  QApplication::quit();
}

void MainWindow::updateStatus()
{
  using enum ProcessState;
  const auto connection = m_coreProcess.connectionState();
  const auto process = m_coreProcess.processState();
  const bool isServer = (m_coreProcess.mode() == CoreMode::Server);
  if (process == Stopped || process == Started) {
    updateNetworkInfo();
    ui->btnEditName->setVisible(process == Stopped);
  }
  m_statusBar->setStatus(connection, process, isServer);
}

void MainWindow::coreProcessStateChanged(ProcessState state)
{
  using enum ProcessState;
  updateStatus();
  if (state == Started) {
    qDebug() << "recording that core has started";
    Settings::setValue(Settings::Gui::AutoStartCore, true);
  }

  if (state == Started || state == Starting || state == RetryPending) {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(true);
    m_actionStartCore->setVisible(false);
    m_actionRestartCore->setVisible(true);
    m_actionStopCore->setEnabled(true);

    if (state == Starting) {
      saveSettings();
    }

  } else {
    disconnect(ui->btnToggleCore, &QPushButton::clicked, m_actionStopCore, &QAction::trigger);
    connect(ui->btnToggleCore, &QPushButton::clicked, m_actionStartCore, &QAction::trigger, Qt::UniqueConnection);

    ui->btnRestartCore->setEnabled(false);
    m_actionStartCore->setVisible(true);
    m_actionRestartCore->setVisible(false);
    m_actionStopCore->setEnabled(false);
  }
  updateModeControlLabels();
}

void MainWindow::coreConnectionStateChanged(ConnectionState state)
{
  qDebug() << "core connection state changed:" << static_cast<int>(state);

  updateStatus();

  // always assume connection is not secure when connection changes
  // to anything except connected. the only way the padlock shows is
  // when the correct TLS version string is detected.
  if (state != ConnectionState::Connected) {
    secureSocket(false);
  }
}

void MainWindow::updateFingerprintButton()
{
  m_statusBar->setBtnFingerprintVisible(TlsUtility::isEnabled() && !m_fingerprint.data.isEmpty());
}

void MainWindow::hide()
{
#ifdef Q_OS_MACOS
  macOSNativeHide();
#else
  QMainWindow::hide();
#endif
  m_actionRestore->setVisible(true);
  m_actionMinimize->setVisible(false);
}

void MainWindow::changeEvent(QEvent *e)
{
  QMainWindow::changeEvent(e);
  if (e->type() == QEvent::PaletteChange) {
    updateIconTheme();
    setWindowIcon(QIcon::fromTheme(kRevFqdnName));
    setTrayIcon();
  } else if (e->type() == QEvent::LanguageChange) {
    ui->retranslateUi(this);
    updateModeControlLabels();
    updateNetworkInfo();
    updateStatus();
    serverClientsChanged({});
    updateText();
  }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
  if (obj != ui->lineEditName || event->type() != QEvent::KeyPress)
    return false;
  if (const auto keyEvent = static_cast<QKeyEvent *>(event); keyEvent->key() != Qt::Key_Escape)
    return false;
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  ui->lineEditName->setText(Settings::value(Settings::Core::ComputerName).toString());
  toggleCanRunCore(canRunCore());
  return true;
}

void MainWindow::updateText()
{
  m_menuFile->setTitle(tr("&File"));
  m_menuEdit->setTitle(tr("&Edit"));
  m_menuView->setTitle(tr("&View"));
  m_menuHelp->setTitle(tr("&Help"));

  m_actionMinimize->setText(tr("&Minimize to tray"));
  m_actionQuit->setText(tr("&Quit"));
  m_actionTrayQuit->setText(tr("&Quit"));
  m_actionRestore->setText(tr("&Configure Server"));
  m_actionSettings->setText(tr("&Preferences"));
  m_actionStartCore->setText(tr("&Start"));
  m_actionRestartCore->setText(tr("Rest&art"));
  m_actionStopCore->setText(tr("S&top"));
  //: %1 will be the replaced with the appname
  m_actionAbout->setText(tr("About %1...").arg(kAppName));

  m_actionShowHelp->setText(tr("View &Help"));

  //: start / restart core shortcut
  m_actionStartCore->setShortcut(QKeySequence(tr("Ctrl+S")));
  m_actionRestartCore->setShortcut(QKeySequence(tr("Ctrl+S")));

  //: stop core shortcut
  m_actionStopCore->setShortcut(QKeySequence(tr("Ctrl+T")));

  if (deskflow::platform::isWindows()) {
    //: Quit shortcut
    m_actionQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
    m_actionTrayQuit->setShortcut(QKeySequence(tr("Ctrl+Q")));
  }
}

void MainWindow::showConfigureServer(const QString &message)
{
  ServerConfigDialog dialog(this, serverConfig());
  dialog.message(message);
  if ((dialog.exec() == QDialog::Accepted) && m_coreProcess.isStarted()) {
    m_coreProcess.restart();
  }
}

void MainWindow::showConfigureClient()
{
  ClientConfigDialog dialog(this);
  if ((dialog.exec() == QDialog::Accepted) && m_coreProcess.isStarted()) {
    m_coreProcess.restart();
  }
}

void MainWindow::secureSocket(bool secureSocket)
{
  m_secureSocket = secureSocket;
  updateSecurityIcon(m_statusBar->securityIconVisible());
}

void MainWindow::updateScreenName()
{
  const auto screenName = Settings::value(Settings::Core::ComputerName).toString();
  ui->lblComputerName->setText(screenName);
  ui->lineEditName->setText(screenName);
  m_serverConfig.updateServerName();
}

void MainWindow::showAndActivate()
{
  const auto wasVisible = isVisible();
#ifdef Q_OS_MACOS
  forceAppActive();
#endif
  showNormal();
  raise();
  activateWindow();
  m_actionRestore->setVisible(false);
  m_actionMinimize->setVisible(true);
  if (!wasVisible)
    restoreWindow();
}

void MainWindow::showHostNameEditor()
{
  ui->lineEditName->show();
  ui->lblComputerName->hide();
  ui->btnEditName->hide();
  toggleCanRunCore(false);
  ui->lineEditName->setFocus();
}

void MainWindow::setHostName()
{
  ui->lineEditName->hide();
  ui->lblComputerName->show();
  ui->btnEditName->show();
  toggleCanRunCore(canRunCore());

  QString text = ui->lineEditName->text();
  const auto screenName = Settings::value(Settings::Core::ComputerName).toString();

  if (text == screenName)
    return;

  const bool isServer = ui->rbModeServer->isChecked();
  bool existingScreen = false;
  if (isServer)
    existingScreen = serverConfig().screenExists(text);

  if (!ui->lineEditName->hasAcceptableInput() || text.isEmpty() || existingScreen) {
    blockSignals(true);
    ui->lineEditName->setText(screenName);
    blockSignals(false);

    const auto title = tr("Invalid Screen Name");
    QString body;
    if (existingScreen) {
      body = tr("Screen name already exists");
    } else {
      body =
          tr("The name you have chosen is invalid.\n\n"
             "Valid names:\n"
             "• Use letters and numbers\n"
             "• May also use _ or -\n"
             "• Are between 1 and 255 characters");
    }
    QMessageBox::information(this, title, body);
    return;
  }

  ui->lblComputerName->setText(ui->lineEditName->text());
  Settings::setValue(Settings::Core::ComputerName, ui->lineEditName->text());
  if (isServer)
    serverConfig().updateServerName();
  applyConfig();
}

QString MainWindow::trustedFingerprintDatabase() const
{
  const bool isClient = m_coreProcess.mode() == CoreMode::Client;
  return isClient ? Settings::tlsTrustedServersDb() : Settings::tlsTrustedClientsDb();
}

bool MainWindow::generateCertificate()
{
  const auto certificate = Settings::value(Settings::Security::Certificate).toString();
  if (!QFile::exists(certificate) && !TlsUtility::generateCertificate()) {
    return false;
  }

  m_fingerprint = {QCryptographicHash::Sha256, TlsUtility::certFingerprint()};
  updateFingerprintButton();
  return true;
}

void MainWindow::serverClientsChanged(const QStringList &clients)
{
  if (m_coreProcess.mode() != CoreMode::Server || !m_coreProcess.isStarted())
    return;
  m_statusBar->setServerClients(clients);
}

void MainWindow::daemonIpcClientConnectionFailed()
{
  if (deskflow::gui::messages::showDaemonOffline(this)) {
    m_coreProcess.retryDaemon();
  }
}

void MainWindow::toggleCanRunCore(bool enableButtons)
{
  const bool isStarted = m_coreProcess.isStarted();
  ui->btnToggleCore->setEnabled(enableButtons);
  ui->btnRestartCore->setEnabled(enableButtons && isStarted);
  m_actionStartCore->setEnabled(enableButtons);
  m_actionStopCore->setEnabled(enableButtons && isStarted);
}

void MainWindow::remoteHostChanged(const QString &newRemoteHost)
{
  m_coreProcess.setAddress(newRemoteHost);
  toggleCanRunCore(!newRemoteHost.isEmpty() && ui->rbModeClient->isChecked());
  if (newRemoteHost.isEmpty()) {
    Settings::setValue(Settings::Client::RemoteHost);
  } else {
    Settings::setValue(Settings::Client::RemoteHost, newRemoteHost);
  }
}

void MainWindow::updateIpLabel(const QStringList &addresses)
{
  const auto mode = m_coreProcess.mode();
  if (mode == CoreMode::None) {
    return;
  }

  static const auto colorText = QStringLiteral(R"(<span style="color:%1;">%2</span>)");
  const bool serverStarted = m_coreProcess.isStarted();
  const bool fixedIP = !Settings::value(Settings::Core::Interface).toString().isEmpty();

  if (!fixedIP && addresses.isEmpty() && !serverStarted || (serverStarted && m_serverStartSuggestedIP.isEmpty())) {
    ui->lblIpAddresses->setText(colorText.arg(palette().linkVisited().color().name(), tr("No IP Detected")));
    ui->lblIpAddresses->setToolTip(tr("Unable to detect an IP address. Check your network connection is active."));
    return;
  }

  QString labelText = fixedIP ? tr("Using IP: ") : tr("Suggested IP: ");
  QString toolTipText = tr("<p>If connecting via the hostname fails, try %1</p>");

  const bool filterIpList = (serverStarted || fixedIP);
  const QRegularExpression ipListFilter(filterIpList ? QStringLiteral("(%1)").arg(m_serverStartIPs.join("|")) : "");
  const QStringList ipList = addresses.filter(ipListFilter);

  bool IPValid = true;
  if (filterIpList && (m_serverStartSuggestedIP != m_currentIpAddress) || !ipList.contains(m_serverStartSuggestedIP)) {
    IPValid = !ipList.isEmpty();
  }

  if (IPValid) {
    m_currentIpAddress = ipList.first();
    labelText.append(m_currentIpAddress);
  } else {
    labelText.append(colorText.arg(palette().linkVisited().color().name(), m_serverStartSuggestedIP));
    toolTipText.append(tr("\nA bound IP is now invalid, you may need to restart the server."));
  }

  if (ipList.count() < 2 || fixedIP) {
    toolTipText = toolTipText.arg(tr("the suggested IP."));
  } else {
    toolTipText = toolTipText.arg(tr("one of the following IPs:<br/>%1").arg(ipList.join("<br/>")));
  }

  ui->lblIpAddresses->setText(labelText);
  ui->lblIpAddresses->setToolTip(mode == CoreMode::Server ? toolTipText : QString());
}

void MainWindow::updateTimeoutDelay(int newDelay)
{
  m_statusBar->setConnectionInterval(newDelay);
}

void MainWindow::setHelpFilePath()
{
  const QString appPath = QCoreApplication::applicationDirPath();
  auto buildPath = QString("%1/../docs/HelpMain.md").arg(appPath);
  auto installPath = QString("%1/../share/doc/%2/HelpMain.md").arg(appPath, kAppId);
  if (deskflow::platform::isMac()) {
    installPath = QString("%1/../Resources/docs/HelpMain.md").arg(appPath);
    buildPath = QString("%1/../../../../docs/HelpMain.md").arg(appPath);
  } else if (deskflow::platform::isWindows()) {
    installPath = QString("%1/docs/HelpMain.md").arg(appPath);
  }

  buildPath = QDir::cleanPath(buildPath);
  installPath = QDir::cleanPath(installPath);

  if (QFile::exists(installPath))
    m_helpPath = QUrl::fromLocalFile(installPath);
  else if (QFile::exists(buildPath))
    m_helpPath = QUrl::fromLocalFile(buildPath);
  else
    m_helpPath = kUrlWiki;
}

void MainWindow::showHelpViewer() const
{
  if (m_helpPath.isLocalFile()) {
    HelpDialog dialogHelp(this->centralWidget(), m_helpPath);
    dialogHelp.exec();
  } else {
    QDesktopServices::openUrl(m_helpPath);
  }
}

bool MainWindow::canRunCore() const
{
  const auto mode = m_coreProcess.mode();
  const bool isServer = mode == Settings::CoreMode::Server;
  const bool isClient = mode == Settings::CoreMode::Client;
  return ((isServer || isClient) && (isClient && !ui->lineHostname->text().isEmpty()) || isServer);
}
