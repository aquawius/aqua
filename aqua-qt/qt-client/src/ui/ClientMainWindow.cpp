//
// Created by QU on 25-2-15.
//

// You may need to build the project (run Qt uic code generator) to get "ui_ClientMainWindow.h" resolved

#include "ClientMainWindow.h"
#include "ui_ClientMainWindow.h"


ClientMainWindow::ClientMainWindow(QWidget *parent) :
    QMainWindow(parent), ui(new Ui::ClientMainWindow) {
    ui->setupUi(this);
}

ClientMainWindow::~ClientMainWindow() {
    delete ui;
}
