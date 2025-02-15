//
// Created by aquawius on 25-2-15.
//

#include <QApplication>
#include <QMainWindow>
#include <QDebug>
#include <QtWidgets/QPushButton>

#include "version.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QMainWindow window;
    window.show();

    qDebug() << "Hello World!";
    qDebug() << "aqua_server_VERSION: " << aqua_server_VERSION;
    qDebug() << "aqua_server_BINARY_NAME: " << aqua_server_BINARY_NAME;

    return QApplication::exec();
}
