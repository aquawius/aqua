//
// Created by QU on 25-2-15.
//

#include <QMainWindow>
#include <memory>
#include "network_client.h"

QT_BEGIN_NAMESPACE
namespace Ui { class ClientMainWindow; }
QT_END_NAMESPACE

class ClientMainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit ClientMainWindow(QWidget *parent = nullptr);
    ~ClientMainWindow() override;

    private slots:
        void onConnectClicked();
    void updateStatus(const QString& message);
    void appendLog(const QString& log);
    void handleClientError(const QString& error);

private:
    void initConnections();
    void setupNetworkClient();

    Ui::ClientMainWindow *ui;
    std::unique_ptr<network_client> m_client;
    std::atomic<bool> m_connected {false};
};