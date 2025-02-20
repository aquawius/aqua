//
// Created by aquawius on 25-2-15.
//

#include "version.h"

#include <QApplication>
#include <QDebug>
#include <QStyleFactory>

#include "ui/ServerMainWindow.h"
#include "ui/AudioMeterWidget.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    ServerMainWindow server_main_window { };

    server_main_window.show();

    qDebug() << "Hello World!";
    qDebug() << "aqua_server_VERSION: " << aqua_server_VERSION;
    qDebug() << "aqua_server_BINARY_NAME: " << aqua_server_BINARY_NAME;

    return QApplication::exec();
}
