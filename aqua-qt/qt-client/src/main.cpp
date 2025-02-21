//
// Created by aquawius on 25-2-15.
//

#include <QApplication>
#include <QStyleFactory>

#include "ClientMainWindow.h"
#include "version.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    ClientMainWindow window;
    window.setWindowTitle(QString("%1 v%2").arg(aqua_client_BINARY_NAME).arg(aqua_client_VERSION));
    window.show();

    return app.exec();
}