//
// Created by QU on 25-2-15.
//

#include "ServerMainWindow.h"
#include "AudioMeterWidget.h"
#include "ui_ServerMainWindow.h"

#include <version.h>

#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>
#include <QActionGroup>

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

    // menu bar logger level & and create QActionGroup
    setupMenuBarLoggerLevel();

    // logger setup
    setupLoggerSink();

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

// ################### SLOTS #####################
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
    updateTabIPv4ConnectionsList();
    updateBottomBarServerStatus();
    updateBottomBarConnectionCount();
}

void ServerMainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About Aqua Server"),
                       tr("<h3>Aqua Server</h3>"
                           "<p>An audio streaming server application.</p>"
                           "<h6>Version: %1</h6>"
                           "<h6>Platform: %2</h6>"
                           "<a href='https://github.com/aquawius/aqua'>aqua GitHub Repository</a>")
                       .arg(aqua_server_VERSION, aqua_server_PLATFORM_NAME));
}

void ServerMainWindow::onKickClient()
{
    // 获取所有选中的行
    QModelIndexList selectedRows = ui->tableIPv4Connections->selectionModel()->selectedRows();

    QSet<QString> uuidsToKick;

    // 遍历选中的行并提取UUID
    for (const QModelIndex& index : selectedRows)
    {
        int row = index.row();
        QTableWidgetItem* uuidItem = ui->tableIPv4Connections->item(row, 2); // 第2列是UUID

        if (uuidItem)
        {
            uuidsToKick.insert(uuidItem->text());
        }
    }

    // 执行踢出操作
    for (const QString& uuid : uuidsToKick)
    {
        session_manager::get_instance().remove_session(uuid.toStdString());
        spdlog::info("[main_window] Kicked client: {}", uuid.toStdString());
    }

    if (!uuidsToKick.isEmpty())
    {
        updateTabIPv4ConnectionsList();
        QMessageBox::information(this, "Info",
                                 QString("Kicked %1 client(s)").arg(uuidsToKick.size()));
    }
    else
    {
        QMessageBox::warning(this, "Warning", "No clients selected");
    }
}


void ServerMainWindow::onMuteClient()
{
    // TODO: 实现静音逻辑
    QMessageBox::information(this, "Info", "Mute feature not implemented yet");
}

// ################### private functions #####################
void ServerMainWindow::setupLoggerSink()
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

void ServerMainWindow::setupMenuBarLoggerLevel()
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
                spdlog::error("[main_window] Network server shutdown ungracefully, triggering exit...");
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

        m_audio_manager->set_peak_callback([this](const float peak_val)
        {
            QMetaObject::invokeMethod(this, [this, peak_val]()
            {
                ui->audioMeterWidget->setPeakValue(peak_val);
            }, Qt::QueuedConnection);
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
    updateTabIPv4ConnectionsList();
    // updateTabIPv6ConnectionsList();
}

void ServerMainWindow::updateTabIPv4ConnectionsList()
{
    // 保存当前选中项的UUID
    QSet<QString> selectedUUIDs;
    QModelIndexList selectedRows = ui->tableIPv4Connections->selectionModel()->selectedRows();
    for (const QModelIndex& index : selectedRows)
    {
        int row = index.row();
        QTableWidgetItem* uuidItem = ui->tableIPv4Connections->item(row, 2);
        if (uuidItem)
        {
            selectedUUIDs.insert(uuidItem->text());
        }
    }

    // 保存当前的排序状态
    int sortColumn = ui->tableIPv4Connections->horizontalHeader()->sortIndicatorSection();
    Qt::SortOrder sortOrder = ui->tableIPv4Connections->horizontalHeader()->sortIndicatorOrder();

    // 禁用排序以避免插入数据时的干扰
    ui->tableIPv4Connections->setSortingEnabled(false);

    // 清空表格并重新填充数据
    ui->tableIPv4Connections->setRowCount(0);
    auto sessions = session_manager::get_instance().get_sessions();

    for (const auto& s : sessions)
    {
        const int row = ui->tableIPv4Connections->rowCount();
        ui->tableIPv4Connections->insertRow(row);

        auto* ipItem = new QTableWidgetItem(QString::fromStdString(s->get_endpoint().address().to_string()));
        auto* portItem = new QTableWidgetItem(QString::number(s->get_endpoint().port()));
        auto* uuidItem = new QTableWidgetItem(QString::fromStdString(s->get_client_uuid()));

        // 设置数据关联
        const QVariant sessionData = QVariant::fromValue(s);
        ipItem->setData(Qt::UserRole, sessionData);
        portItem->setData(Qt::UserRole, sessionData);
        uuidItem->setData(Qt::UserRole, sessionData);

        ui->tableIPv4Connections->setItem(row, 0, ipItem);
        ui->tableIPv4Connections->setItem(row, 1, portItem);
        ui->tableIPv4Connections->setItem(row, 2, uuidItem);
    }

    // 恢复之前的排序状态
    ui->tableIPv4Connections->setSortingEnabled(true);
    ui->tableIPv4Connections->sortByColumn(sortColumn, sortOrder);

    // 根据UUID重新选中项（使用QItemSelection批量选中）
    QItemSelection selection;
    for (int row = 0; row < ui->tableIPv4Connections->rowCount(); ++row)
    {
        QTableWidgetItem* uuidItem = ui->tableIPv4Connections->item(row, 2);
        if (uuidItem && selectedUUIDs.contains(uuidItem->text()))
        {
            QModelIndex topLeft = ui->tableIPv4Connections->model()->index(row, 0);
            QModelIndex bottomRight = ui->tableIPv4Connections->model()->index(
                row, ui->tableIPv4Connections->columnCount() - 1);
            selection.select(topLeft, bottomRight);
        }
    }

    // 应用选中并确保使用正确的选择命令
    ui->tableIPv4Connections->selectionModel()->select(selection,
                                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
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
    ui->tabIPv6Connections->setEnabled(true);
}

void ServerMainWindow::disableIPv6Controls()
{
    // TODO: ipv6 server not implemented. so turn off all controls.
    ui->comboBox_IPv6AddrSelector->setEnabled(false);
    ui->spinBox_IPv6Port->setEnabled(false);
    ui->spinBox_IPv6RPCPort->setEnabled(false);
    ui->pushButton_IPv6ToggleStart->setEnabled(false);
    ui->tabIPv6Connections->setEnabled(false);
}
