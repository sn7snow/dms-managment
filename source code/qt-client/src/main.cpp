// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("DMS");
    QCoreApplication::setApplicationName("DMS Qt Client");

    MainWindow window;
    window.show();
    return application.exec();
}
