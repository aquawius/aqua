//
// Created by QU on 25-2-15.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ClientMainWindow.h" resolved

#include "ClientMainWindow.h"
#include "ui_ClientMainWindow.h"

#include "ClientMainWindow.h"
#include "ui_ClientMainWindow.h"
#include <spdlog/spdlog.h>
#include <QMessageBox>
#include <QThread>

ClientMainWindow::ClientMainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::ClientMainWindow)
{
    ui->setupUi(this);
    initConnections();

    // 初始化UI状态
    ui->spinBox_ServerRPCPort->setValue(10120);
    ui->spinBox_ClientUDPPort->setValue(0);
    ui->comboBox_ClientAddressSelector->addItems({"0.0.0.0", "127.0.0.1"});
}

ClientMainWindow::~ClientMainWindow()
{
    delete ui;
}

void ClientMainWindow::initConnections()
{
    // 连接按钮信号
    connect(ui->pushButton_Connect, &QPushButton::clicked,
            this, &ClientMainWindow::onConnectClicked);

    // 日志级别菜单
    auto createLevelAction = [this](spdlog::level::level_enum level) {
        return [this, level]() {
            spdlog::set_level(level);
            appendLog(QString("Log level set to %1").arg(
                QString::fromStdString(spdlog::level::to_string_view(level).data())));
        };
    };

    connect(ui->actionWarn, &QAction::triggered, createLevelAction(spdlog::level::warn));
    connect(ui->actionInfo, &QAction::triggered, createLevelAction(spdlog::level::info));
    connect(ui->actionDebug, &QAction::triggered, createLevelAction(spdlog::level::debug));
    connect(ui->actionTrace, &QAction::triggered, createLevelAction(spdlog::level::trace));
}

void ClientMainWindow::onConnectClicked()
{
    if (m_connected) {
        if (m_client) {
            m_client->stop_client();
        }
        m_connected = false;
        ui->pushButton_Connect->setText("Connect");
        appendLog("Disconnected from server");
        return;
    }

    try {
        network_client::client_config config {
            .server_address = ui->lineEdit_ServerAddressInput->text().toStdString(),
            .server_rpc_port = static_cast<uint16_t>(ui->spinBox_ServerRPCPort->value()),
            .client_address = ui->comboBox_ClientAddressSelector->currentText().toStdString(),
            .client_udp_port = static_cast<uint16_t>(ui->spinBox_ClientUDPPort->value())
        };

        m_client = std::make_unique<network_client>(config);

        // 设置回调
        m_client->set_shutdown_callback([this]() {
            QMetaObject::invokeMethod(this, [this]() {
                handleClientError("Connection lost");
            });
        });

        // 在单独线程中启动客户端
        QThread* clientThread = new QThread;
        connect(clientThread, &QThread::started, [this]() {
            if (m_client->start_client()) {
                m_connected = true;
                appendLog("Connected to server successfully");
                QMetaObject::invokeMethod(ui->pushButton_Connect, "setText", Q_ARG(QString, "Disconnect"));
            } else {
                handleClientError("Failed to connect");
            }
        });

        m_client->moveToThread(clientThread);
        clientThread->start();

    } catch (const std::exception& e) {
        handleClientError(QString("Error: %1").arg(e.what()));
    }
}

void ClientMainWindow::appendLog(const QString& log)
{
    QDateTime currentTime = QDateTime::currentDateTime();
    ui->textBrowser->append(QString("[%1] %2")
                            .arg(currentTime.toString("hh:mm:ss"))
                            .arg(log));
}

void ClientMainWindow::handleClientError(const QString& error)
{
    m_connected = false;
    ui->pushButton_Connect->setText("Connect");
    appendLog(QString("<font color='red'>Error: %1</font>").arg(error));
    QMessageBox::critical(this, "Connection Error", error);
}