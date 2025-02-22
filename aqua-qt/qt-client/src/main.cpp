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
    window.show();

    return app.exec();
}
