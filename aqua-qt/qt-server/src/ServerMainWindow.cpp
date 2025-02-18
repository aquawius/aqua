//
// Created by QU on 25-2-15.
//

#include "ServerMainWindow.h"
#include "ui_ServerMainWindow.h"

#include <version.h>

#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/qt_sinks.h>

#include "network_server.h"
#include "audio_manager.h"

ServerMainWindow::ServerMainWindow(QWidget* parent)
    : QMainWindow(parent)
      , ui(new Ui::ServerMainWindow)
      , m_statusTimer(new QTimer(this))
{
    ui->setupUi(this);

    // logger setup
    setupLogger();

    // 初始化IPv4地址列表
    auto addresses = network_server::get_address_list();
    for (const auto& addr : addresses)
    {
        ui->comboBox_IPv4AddrSelector->addItem(QString::fromStdString(addr));
    }

    // TODO: IPv6 current not implemented.
    // 禁用IPv6控件
    disableIPv6Controls();

    // 设置状态栏定时更新
    m_statusTimer->start(1000);
    setupConnections();
}

ServerMainWindow::~ServerMainWindow()
{
    if (m_v4_server && m_v4_server->is_running())
    {
        m_v4_server->stop_server();
    }
    delete ui;
}

// SLOTS:
void ServerMainWindow::onIPv4StartToggleClicked()
{
    if (!m_v4_server || !m_v4_server->is_running())
    {
        startIPv4Server();
    }
    else
    {
        stopIPv4Server();
    }
}


void ServerMainWindow::updateAllInfoTimer()
{
    onRefreshConnectionsListButtonClicked();
    updateBottomBarServerStatus();
    updateBottomBarConnectionCount();
}

void ServerMainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About Aqua Server"),
                       tr("<h2>Aqua Server</h2>"
                           "<p>Version: %1</p>"
                           "<p>Platform: %2</p>"
                           "<p>An audio streaming server application.</p>"
                           "<a href='https://github.com/aquawius/aqua'>aqua GitHub Repository</a>")
                       .arg(aqua_server_VERSION, aqua_server_PLATFORM_NAME));
}

void ServerMainWindow::onKickClient()
{
    auto selected = ui->tableIPv4Connections->selectionModel()->selectedRows();
    for (const auto& index : selected)
    {
        QString address = ui->tableIPv4Connections->item(index.row(), 0)->text();
        QString port = ui->tableIPv4Connections->item(index.row(), 1)->text();

        session_manager::get_instance().remove_session(
            fmt::format("{}:{}", address.toStdString(), port.toStdString()));
    }
    onRefreshConnectionsListButtonClicked();
}

void ServerMainWindow::onMuteClient()
{
    // TODO: 实现静音逻辑
    QMessageBox::information(this, "Info", "Mute feature not implemented yet");
}

// ################### private functions #####################


void ServerMainWindow::setupLogger()
{
    // 初始化日志系统
    const auto logger = spdlog::qt_color_logger_mt("aqua", ui->textBrowser, 1000);
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::debug);

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

void ServerMainWindow::setupConnections()
{
    connect(m_statusTimer, &QTimer::timeout, this, &ServerMainWindow::updateAllInfoTimer);
    connect(ui->pushButton_IPv4ToggleStart, &QPushButton::clicked, this, &ServerMainWindow::onIPv4StartToggleClicked);
    connect(ui->pushButton_Refresh, &QPushButton::clicked, this,
            &ServerMainWindow::onRefreshConnectionsListButtonClicked);
    connect(ui->pushButton_Kick, &QPushButton::clicked, this, &ServerMainWindow::onKickClient);
    connect(ui->pushButton_Mute, &QPushButton::clicked, this, &ServerMainWindow::onMuteClient);
    connect(ui->actionAbout, &QAction::triggered, this, &ServerMainWindow::showAboutDialog);
}

void ServerMainWindow::startIPv4Server()
{
    disableIPv4Controls();
    try
    {
        const auto address = ui->comboBox_IPv4AddrSelector->currentText().toStdString();
        const auto grpc_port = ui->spinBox_IPv4RPCPort->value();
        const auto udp_port = ui->spinBox_IPv4Port->value();

        m_v4_server = network_server::create(address, grpc_port, udp_port);
        if (!m_v4_server)
        {
            throw std::runtime_error("[main_window] Failed to create server instance");
        }

        m_v4_server->set_shutdown_callback([this]()
        {
            QMetaObject::invokeMethod(this, [this]()
            {
                spdlog::error("[main] Network server shutdown ungracefully, triggering exit...");
                ui->pushButton_IPv4ToggleStart->setText("Start");
                stopIPv4Server();
                spdlog::info("[main_window] Network server shutdown");
            });
        });

        if (m_v4_server->start_server())
        {
            ui->pushButton_IPv4ToggleStart->setText("Stop");
            spdlog::info("[main_window] Server started on {}:{} (gRPC) / {} (UDP)",
                         address, grpc_port, udp_port);
        }

        m_audio_manager = audio_manager::create();
        if (!m_audio_manager || !m_audio_manager->init())
        {
            throw std::runtime_error("[main_window] Failed to create audio capture instance");
        }

        if (!m_audio_manager->setup_stream())
        {
            throw std::runtime_error("[main_window] Failed to setup audio capture instance");
        }
        spdlog::info("[main_window] Audio manager initialized");

        m_audio_manager->start_capture([this](const std::span<const float> data)
        {
            if (data.empty())
            {
                return;
            }
            if (m_v4_server && m_v4_server->is_running())
            {
                m_v4_server->push_audio_data(data);
            }
        });
    }
    catch (const std::exception& e)
    {
        QMessageBox::critical(this, "Error", QString("Failed to start server: ") + e.what());
        stopIPv4Server();
        spdlog::error("[main_window] Server start failed: {}", e.what());
    }
}

void ServerMainWindow::stopIPv4Server()
{
    enableIPv4Controls();
    ui->pushButton_IPv4ToggleStart->setText("Start");
    try
    {
        if (m_v4_server)
            m_v4_server->stop_server();
        if (m_audio_manager)
            m_audio_manager->stop_capture();
    }
    catch (const std::exception& e)
    {
        QMessageBox::critical(this, "Error", QString("Stop failed: ") + e.what());
    }
}

void ServerMainWindow::onRefreshConnectionsListButtonClicked()
{
    updateTabIPv4ConnectionList();
    // updateTabIPv6ConnectionList();
}

void ServerMainWindow::updateTabIPv4ConnectionList()
{
    ui->tableIPv4Connections->setRowCount(0);
    auto endpoints = session_manager::get_instance().get_active_endpoints();
    spdlog::debug("[main_window] refreshing connections, endpoints: {}", endpoints.size());

    for (const auto& endpoint : endpoints)
    {
        const int row = ui->tableIPv4Connections->rowCount();
        ui->tableIPv4Connections->insertRow(row);

        ui->tableIPv4Connections->setItem(row, 0,
                                          new QTableWidgetItem(QString::fromStdString(endpoint.address().to_string())));
        ui->tableIPv4Connections->setItem(row, 1,
                                          new QTableWidgetItem(QString::number(endpoint.port())));
        ui->tableIPv4Connections->setItem(row, 2,
                                          new QTableWidgetItem());
    }
}

void ServerMainWindow::updateBottomBarServerStatus()
{
    const QString status = m_v4_server && m_v4_server->is_running()
                               ? "<font color='green'>Running</font>"
                               : "<font color='red'>Stopped</font>";
    ui->runningStatus->setText("Status: " + status);
}

void ServerMainWindow::updateBottomBarConnectionCount()
{
    const auto count = session_manager::get_instance().get_active_endpoints().size();
    ui->connectionNum->setText(QString("Connections: %1").arg(count));
}

void ServerMainWindow::disableIPv4Controls()
{
    ui->comboBox_IPv4AddrSelector->setEnabled(false);
    ui->spinBox_IPv4Port->setEnabled(false);
    ui->spinBox_IPv4RPCPort->setEnabled(false);
}

void ServerMainWindow::enableIPv4Controls()
{
    ui->comboBox_IPv4AddrSelector->setEnabled(true);
    ui->spinBox_IPv4Port->setEnabled(true);
    ui->spinBox_IPv4RPCPort->setEnabled(true);
}

// TODO: ipv6 server not implemented. should NOT invoke this.
void ServerMainWindow::enableIPv6Controls()
{
    ui->comboBox_IPv6AddrSelector->setEnabled(true);
    ui->spinBox_IPv6Port->setEnabled(true);
    ui->spinBox_IPv6RPCPort->setEnabled(true);
    ui->pushButton_IPv6ToggleStart->setEnabled(true);
    ui->tabIPv6Connection->setEnabled(true);
}

void ServerMainWindow::disableIPv6Controls()
{
    // TODO: ipv6 server not implemented. so turn off all controls.
    ui->comboBox_IPv6AddrSelector->setEnabled(false);
    ui->spinBox_IPv6Port->setEnabled(false);
    ui->spinBox_IPv6RPCPort->setEnabled(false);
    ui->pushButton_IPv6ToggleStart->setEnabled(false);
    ui->tabIPv6Connection->setEnabled(false);
}
