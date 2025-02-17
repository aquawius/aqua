//
// Created by QU on 25-2-15.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ServerMainWindow.h" resolved

#include "ServerMainWindow.h"
#include "ui_ServerMainWindow.h"

#include <QMessageBox>
#include <QTimer>
#include <version.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/qt_sinks.h>

#include "../../../aqua-server/src/network_server.h"

ServerMainWindow::ServerMainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::ServerMainWindow),
    m_statusTimer(new QTimer(this))
{
    ui->setupUi(this);

    // 初始化日志系统
    auto logger = spdlog::qt_color_logger_mt("aqua", ui->textBrowser, 1000);
    spdlog::set_default_logger(logger);

    // 初始化网络地址列表
    auto addresses = network_server::get_address_list();
    for (const auto& addr : addresses) {
        ui->comboBox_IPv4AddrSelector->addItem(QString::fromStdString(addr));
        ui->comboBox_IPv6AddrSelector->addItem(QString::fromStdString(addr));
    }

    // 设置状态栏定时更新
    m_statusTimer->start(1000);

    setupConnections();
}

ServerMainWindow::~ServerMainWindow()
{
    delete ui;
}

void ServerMainWindow::setupConnections()
{
    // 按钮点击事件
    connect(ui->pushButton_IPv4ToggleStart, &QPushButton::clicked,
            this, &ServerMainWindow::onIPv4StartClicked);
    // connect(ui->pushButton_IPv6ToggleStart, &QPushButton::clicked,
            // this, &ServerMainWindow::onIPv6StartClicked);
    connect(ui->pushButton_Refresh, &QPushButton::clicked,
            this, &ServerMainWindow::refreshConnections);

    // 定时更新状态
    connect(m_statusTimer, &QTimer::timeout,
            this, &ServerMainWindow::updateStatusBar);

    // 菜单动作
    connect(ui->actionAbout, &QAction::triggered,
            this, &ServerMainWindow::showAboutDialog);
}

void ServerMainWindow::onIPv4StartClicked()
{
    if (!m_v4_server || !m_v4_server->is_running()) {
        try {
            const auto address = ui->comboBox_IPv4AddrSelector->currentText().toStdString();
            const auto port = ui->spinBox_IPv4Port->value();

            m_v4_server = network_server::create(address, port, port);
            if (m_v4_server->start_server()) {
                ui->pushButton_IPv4ToggleStart->setText("Stop");
                spdlog::info("IPv4 server started on {}:{}", address, port);
            }
        } catch (const std::exception& e) {
            spdlog::error("Failed to start IPv4 server: {}", e.what());
        }
    } else {
        m_v4_server->stop_server();
        ui->pushButton_IPv4ToggleStart->setText("Start");
        spdlog::info("IPv4 server stopped");
    }
    // updateServerStatus(m_v4_server->is_running());
}

void ServerMainWindow::refreshConnections()
{
    // 清空表格
    ui->tableIPv4->setRowCount(0);
    ui->tableIPv6->setRowCount(0);

    // 获取活动连接并更新表格
    if (m_v4_server) {
        auto endpoints = session_manager::get_instance().get_active_endpoints();
        for (const auto& endpoint : endpoints) {
            const int row = ui->tableIPv4->rowCount();
            ui->tableIPv4->insertRow(row);
            ui->tableIPv4->setItem(row, 0,
                new QTableWidgetItem(QString::fromStdString(endpoint.address().to_string())));
            ui->tableIPv4->setItem(row, 1,
                new QTableWidgetItem(QString::number(endpoint.port())));
        }
    }
}

void ServerMainWindow::updateStatusBar()
{
    // 更新服务器状态
    QString status = "Stopped";
    if ((m_v4_server && m_v4_server->is_running()) ||
        (m_v6_server && m_v6_server->is_running())) {
        status = "Running";
    }
    ui->runningStatus->setText("Server status: " + status);

    // 更新连接数
    updateConnectionCount();
}

void ServerMainWindow::updateConnectionCount()
{
    const auto count = session_manager::get_instance().get_session_count();
    ui->connectionNum->setText(QString("Connections: %1").arg(count));
}

void ServerMainWindow::showAboutDialog()
{
    QMessageBox::about(this, tr("About Aqua Server"),
        tr("<h2>Aqua Server</h2>"
           "<p>Version: %1</p>"
           "<p>An audio streaming server application.</p>")
           .arg(aqua_server_VERSION));
}
