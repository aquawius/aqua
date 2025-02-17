//
// Created by QU on 25-2-15.
//

#ifndef SERVERMAINWINDOW_H
#define SERVERMAINWINDOW_H

#include <QMainWindow>
#include <memory>
#include "../../../aqua-server/src/network_server.h"

namespace Ui
{
    class ServerMainWindow;
}

class ServerMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ServerMainWindow(QWidget* parent = nullptr);
    ~ServerMainWindow();

private slots:
    void onIPv4StartClicked();
    // void onIPv6StartClicked();
    void refreshConnections();
    void updateStatusBar();
    void showAboutDialog();

private:
    void setupConnections();
    // void updateServerStatus(bool running);
    void updateConnectionCount();

    Ui::ServerMainWindow* ui;
    std::unique_ptr<network_server> m_v4_server;
    std::unique_ptr<network_server> m_v6_server;
    QTimer* m_statusTimer;
};

#endif // SERVERMAINWINDOW_H
