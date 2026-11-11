// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("DMS");
    QCoreApplication::setApplicationName("DMS Qt Client");
    const QIcon app_icon(QCoreApplication::applicationDirPath() + "/theme/cor.png");
    if (!app_icon.isNull()) {
        application.setWindowIcon(app_icon);
    }

    MainWindow window;
    window.show();
    return application.exec();
}
