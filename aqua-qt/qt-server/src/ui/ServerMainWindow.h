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
#include "audio_format_common.hpp"

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
    
    // 添加新的槽函数
    void showAudioFormatDialog();
    void onAudioFormatAccepted(const audio_common::AudioFormat& format);
    void onAudioFormatRejected();

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

    // 处理音频格式更改
    void applyAudioFormat(const audio_common::AudioFormat& format);

    Ui::ServerMainWindow* ui;

    std::unique_ptr<network_server> m_v4_server;
    std::unique_ptr<network_server> m_v6_server;
    std::shared_ptr<audio_manager> m_audio_manager;

    bool logAutoScollFlag = true;

    QTimer* m_statusTimer;

    // 存储当前音频格式
    audio_common::AudioFormat m_current_audio_format;
};

#endif // SERVER_MAIN_WINDOW_H
