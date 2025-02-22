//
// Created by QU on 25-2-15.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ClientMainWindow.h" resolved

#include "ClientMainWindow.h"

#include <QActionGroup>

#include "ui_ClientMainWindow.h"

#include <version.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/qt_sinks.h>

#include <QMessageBox>
#include <QScrollBar>
#include <QThread>

ClientMainWindow::ClientMainWindow(QWidget* parent)
    : QMainWindow(parent)
      , ui(new Ui::ClientMainWindow)
      , m_statusTimer(new QTimer(this))
{
    ui->setupUi(this);

    onUseUserSettingsChecked(ui->checkBox_useCustomSettings->isChecked());

    // menu bar logger level & and create QActionGroup
    setupMenuBarLoggerLevel();

    // logger setup
    setupLoggerSink();

    // 初始化IPv4地址列表
    auto addresses = network_client::get_address_list();
    for (const auto& addr : addresses)
    {
        ui->comboBox_ClientAddressSelector->addItem(QString::fromStdString(addr));
    }

    {
        std::random_device rd;
        std::default_random_engine gen(rd());
        std::uniform_int_distribution<uint16_t> dist(49152, 65535);
        ui->spinBox_ClientUDPPort->setValue(dist(gen));
    }

    // 设置状态栏定时更新
    m_statusTimer->start(1000);
    setupConnections();
}

ClientMainWindow::~ClientMainWindow()
{
    if (m_client && m_client->is_running())
    {
        m_client->stop_client();
    }

    delete ui;
}

// ################### SLOTS #####################
void ClientMainWindow::onConnectToggleClicked()
{
    if (ui->lineEdit_ServerAddressInput->text().isEmpty())
    {
        QMessageBox::critical(this, tr("Error"), tr("Server address not set."));
        return;
    }

    if (!m_client || !m_client->is_running())
    {
        startClient();
    }
    else
    {
        stopClient();
    }
}

void ClientMainWindow::updateAllInfoTimer()
{
    updateBottomBarStatus();
}

void ClientMainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About Aqua Client"),
                       tr("<h3>Aqua Client</h3>"
                           "<p>An audio streaming client application.</p>"
                           "<h6>Version: %1</h6>"
                           "<h6>Platform: %2</h6>"
                           "<a href='https://github.com/aquawius/aqua'>aqua GitHub Repository</a>")
                       .arg(aqua_client_VERSION, aqua_client_PLATFORM_NAME));
}

// ################### private functions #####################
void ClientMainWindow::setupLoggerSink()
{
    // 初始化日志系统
    const auto logger = spdlog::qt_color_logger_mt("aqua", ui->textBrowser, 1000);
    spdlog::set_default_logger(logger);

    // 监听滚动条的值变化，判断是否启用自动滚动
    connect(ui->textBrowser->verticalScrollBar(), &QSlider::valueChanged, this, [this](int value)
    {
        if (ui->textBrowser->verticalScrollBar()->maximum() == value)
            logAutoScollFlag = true;
        else
            logAutoScollFlag = false;
    });

    // 日志更新时，只有在自动滚动状态下才自动滚动到底部
    connect(ui->textBrowser, &QTextBrowser::textChanged, this, [this]()
    {
        if (!logAutoScollFlag)
            return;
        auto bar = ui->textBrowser->verticalScrollBar();
        bar->setValue(bar->maximum());
    });
}

void ClientMainWindow::setupConnections()
{
    connect(m_statusTimer, &QTimer::timeout, this, &ClientMainWindow::updateAllInfoTimer);
    connect(ui->pushButton_Connect, &QPushButton::clicked, this, &ClientMainWindow::onConnectToggleClicked);
    connect(ui->action_About, &QAction::triggered, this, &ClientMainWindow::showAboutDialog);
    connect(ui->checkBox_useCustomSettings, &QCheckBox::toggled, this, &ClientMainWindow::onUseUserSettingsChecked);
}

void ClientMainWindow::setupMenuBarLoggerLevel()
{
    QActionGroup* logLevelGroup = new QActionGroup(this);
    logLevelGroup->setExclusive(true);

    logLevelGroup->addAction(ui->actionSetLoggerLevelWarn);
    logLevelGroup->addAction(ui->actionSetLoggerLevelInfo);
    logLevelGroup->addAction(ui->actionSetLoggerLevelDebug);
    logLevelGroup->addAction(ui->actionSetLoggerLevelTrace);

    connect(ui->actionSetLoggerLevelWarn, &QAction::triggered, [=]() { spdlog::set_level(spdlog::level::warn); });
    connect(ui->actionSetLoggerLevelInfo, &QAction::triggered, [=]() { spdlog::set_level(spdlog::level::info); });
    connect(ui->actionSetLoggerLevelDebug, &QAction::triggered, [=]() { spdlog::set_level(spdlog::level::debug); });
    connect(ui->actionSetLoggerLevelTrace, &QAction::triggered, [=]() { spdlog::set_level(spdlog::level::trace); });

    // default selection
    ui->actionSetLoggerLevelInfo->setChecked(true);
}


void ClientMainWindow::updateBottomBarStatus()
{
    const QString status = m_client && m_client->is_running()
                               ? "<font color='green'>Running</font>"
                               : "<font color='red'>Stopped</font>";
    ui->runningStatus->setText("Status: " + status);
}

void ClientMainWindow::startClient()
{
    // 获取服务器配置
    QString serverAddress = ui->lineEdit_ServerAddressInput->text();
    quint16 serverRPCPort = static_cast<quint16>(ui->spinBox_ServerRPCPort->value());

    // 获取客户端配置
    QString clientAddress = ui->comboBox_ClientAddressSelector->currentText();
    quint16 clientUDPPort = static_cast<quint16>(ui->spinBox_ClientUDPPort->value());

    try
    {
        // 初始化网络客户端
        network_client::client_config config {
            .server_address = serverAddress.toStdString(),
            .server_rpc_port = serverRPCPort,
            .client_address = clientAddress.toStdString(),
            .client_udp_port = clientUDPPort,

        };

        m_client = std::make_unique<network_client>(config);

        m_client->set_shutdown_callback([this]()
        {
            spdlog::warn("[ClientMainWindow] Server connection lost, triggering shutdown...");
            stopClient();
        });

        // 启动客户端
        if (!m_client->start_client())
        {
            throw std::runtime_error("Failed to start client");
        }

        // 更新UI状态
        ui->pushButton_Connect->setText(tr("Disconnect"));
        disableClientSettingsControls();
        disableServerSettingsControls();

        ui->checkBox_useCustomSettings->setEnabled(false);

        spdlog::info("[ClientMainWindow] Client started successfully");
    }
    catch (const std::exception& e)
    {
        // 连接失败时恢复控件状态
        ui->pushButton_Connect->setText(tr("Connect"));
        enableClientSettingsControls();
        enableServerSettingsControls();
        QMessageBox::critical(this, tr("Connection Error"),
                              tr("Failed to start client: %1").arg(e.what()));
        spdlog::error("[ClientMainWindow] Client start failed: {}", e.what());
    }
}

void ClientMainWindow::stopClient()
{
    if (!m_client->stop_client())
    {
        spdlog::warn("[ClientMainWindow] Client stopped ERROR.");
    }

    ui->pushButton_Connect->setText(tr("Connect"));
    enableClientSettingsControls();
    enableServerSettingsControls();
    spdlog::info("[ClientMainWindow] Client stopped");
}

void ClientMainWindow::onUseUserSettingsChecked(bool checked)
{
    if (checked)
    {
        enableClientSettingsControls();
    }
    else
    {
        ui->comboBox_ClientAddressSelector->setEnabled(false);
        ui->spinBox_ClientUDPPort->setEnabled(false);
    }
}

void ClientMainWindow::disableClientSettingsControls()
{
    ui->comboBox_ClientAddressSelector->setEnabled(false);
    ui->spinBox_ClientUDPPort->setEnabled(false);
}

void ClientMainWindow::enableClientSettingsControls()
{
    if (ui->checkBox_useCustomSettings->isChecked())
    {
        ui->comboBox_ClientAddressSelector->setEnabled(true);
        ui->spinBox_ClientUDPPort->setEnabled(true);
    }
    ui->checkBox_useCustomSettings->setEnabled(true);
}

void ClientMainWindow::disableServerSettingsControls()
{
    ui->lineEdit_ServerAddressInput->setEnabled(false);
    ui->spinBox_ServerRPCPort->setEnabled(false);
}

void ClientMainWindow::enableServerSettingsControls()
{
    ui->lineEdit_ServerAddressInput->setEnabled(true);
    ui->spinBox_ServerRPCPort->setEnabled(true);
}
