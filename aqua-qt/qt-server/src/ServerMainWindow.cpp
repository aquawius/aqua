//
// Created by QU on 25-2-15.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ServerMainWindow.h" resolved

#include "ServerMainWindow.h"
#include "ui_ServerMainWindow.h"

ServerMainWindow::ServerMainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::ServerMainWindow) {
    ui->setupUi(this);
}

ServerMainWindow::~ServerMainWindow() {
    delete ui;
}
