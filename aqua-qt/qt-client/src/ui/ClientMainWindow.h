//
// Created by QU on 25-2-15.
//

#include <QMainWindow>
#include <memory>
#include "network_client.h"

QT_BEGIN_NAMESPACE

namespace Ui
{
    class ClientMainWindow;
}

QT_END_NAMESPACE

class ClientMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ClientMainWindow(QWidget* parent = nullptr);
    ~ClientMainWindow() override;

private Q_SLOTS:
    void onConnectToggleClicked();
    void onUseUserSettingsChecked(bool checked);
    void updateAllInfoTimer();
    void showAboutDialog();

private:
    void setupLoggerSink();
    void setupConnections();
    void setupMenuBarLoggerLevel();

    void startClient();
    void stopClient();

    void updateBottomBarStatus();

    void disableClientSettingsControls();
    void enableClientSettingsControls();

    void disableServerSettingsControls();
    void enableServerSettingsControls();

    Ui::ClientMainWindow* ui;
    std::unique_ptr<network_client> m_network_client;
    std::shared_ptr<audio_playback> m_audio_playback;

    bool logAutoScollFlag = true;

    QTimer* m_statusTimer;
};
