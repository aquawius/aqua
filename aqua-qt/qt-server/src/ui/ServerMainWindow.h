//
// Created by QU on 25-2-15.
//

#ifndef SERVER_MAIN_WINDOW_H
#define SERVER_MAIN_WINDOW_H

#include <QMainWindow>
#include <memory>
#include <QTimer>

#include "network_server.h"
#include "audio_manager.h"

QT_BEGIN_NAMESPACE

namespace Ui
{
    class ServerMainWindow;
}

QT_END_NAMESPACE

class ServerMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ServerMainWindow(QWidget* parent = nullptr);
    ~ServerMainWindow() override;

private Q_SLOTS:
    void onIPv4StartToggleClicked();
    // void onIPv6StartToggleClicked();

    void onRefreshConnectionsListButtonClicked();
    void updateAllInfoTimer();
    void showAboutDialog();
    void onKickClient();
    void onMuteClient();

private:
    void setupLoggerSink();
    void setupConnections();
    void setupMenuBarLoggerLevel();

    void startIPv4Server();
    void stopIPv4Server();
    // void startIPv6Server();
    // void stopIPv6Server();

    void updateBottomBarServerStatus();
    void updateBottomBarConnectionCount();

    void updateTabIPv4ConnectionsList();
    // void updateTabIPv6ConnectionsList();

    void disableIPv4Controls();
    void enableIPv4Controls();

    void disableIPv6Controls();
    void enableIPv6Controls();

    Ui::ServerMainWindow* ui;

    std::unique_ptr<network_server> m_v4_server;
    std::unique_ptr<network_server> m_v6_server;
    std::shared_ptr<audio_manager> m_audio_manager;

    bool logAutoScollFlag = true;

    QTimer* m_statusTimer;
};

#endif // SERVER_MAIN_WINDOW_H
