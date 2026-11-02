// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "main_window.hpp"

#include <QCloseEvent>
#include <QAbstractItemView>
#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostAddress>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QEventLoop>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStringList>
#include <QPalette>

#include <utility>

namespace {

constexpr int PathRole = Qt::UserRole + 1;
constexpr int TypeRole = Qt::UserRole + 2;
constexpr int PermissionsRole = Qt::UserRole + 3;
constexpr int LoadedRole = Qt::UserRole + 4;
constexpr int OccupiedByRole = Qt::UserRole + 5;
constexpr int LockTimeRole = Qt::UserRole + 6;
constexpr int ReadPermission = 0;
constexpr int WritePermission = 1;
constexpr int DeletePermission = 2;
constexpr int AddVersionPermission = 3;
constexpr int DeleteVersionPermission = 4;
constexpr int UploadPermission = 5;
constexpr int RenamePermission = 6;
constexpr int PermissionCount = 7;

[[maybe_unused]] QStringList DefaultNotifications() {
    return {
        QObject::tr("权限标志：读，写，删，手动增版，手动删版，上传，重命名/移动。"),
        QObject::tr("双击打开文件就立即锁定，不用时请手动释放锁，点击刷新会折叠文件夹，请记住你的文件位置。"),
        QObject::tr("bug请报邮箱1129111763@qq.com。")};
}

[[maybe_unused]] QString NormalizePermissionBits(QString bits) {
    if (bits.size() == PermissionCount - 1) bits.append('0');
    if (bits.size() != PermissionCount) return QString(PermissionCount, '0');
    for (const auto bit : bits) {
        if (bit != '0' && bit != '1') return QString(PermissionCount, '0');
    }
    return bits;
}

[[maybe_unused]] QString NodePath(const QTreeWidgetItem* item) {
    return item == nullptr ? QString{} : item->data(0, PathRole).toString();
}

[[maybe_unused]] QString ParentPath(const QString& path) {
    const int separator = path.lastIndexOf('/');
    return separator <= 0 ? "/" : path.left(separator);
}

[[maybe_unused]] bool IsDirectory(const QTreeWidgetItem* item) {
    return item != nullptr && item->data(0, TypeRole).toString() == "dir";
}

[[maybe_unused]] QString DisplayName(const QString& path) {
    return path.section('/', -1);
}

[[maybe_unused]] bool HasPermission(const QTreeWidgetItem* item, int index) {
    if (item == nullptr) return false;
    const auto bits = item->data(0, PermissionsRole).toString();
    return index >= 0 && index < bits.size() && bits.at(index) == '1';
}

[[maybe_unused]] QString FormatFileSize(qint64 bytes) {
    static const QStringList units{
        QObject::tr("B"), QObject::tr("KB"), QObject::tr("MB"),
        QObject::tr("GB"), QObject::tr("TB")};
    double size = static_cast<double>(bytes);
    qsizetype unit = 0;
    while (size >= 1024.0 && unit + 1 < units.size()) {
        size /= 1024.0;
        ++unit;
    }
    return unit == 0
        ? QObject::tr("%1 %2").arg(bytes).arg(units.at(unit))
        : QObject::tr("%1 %2").arg(size, 0, 'f', size >= 10.0 ? 1 : 2)
              .arg(units.at(unit));
}

[[maybe_unused]] QByteArray MethodName(const char* method) {
    return QByteArray(method);
}

[[maybe_unused]] QIcon RedLockIcon() {
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(190, 45, 45), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(4, 1, 8, 10, 0, 180 * 16);
    painter.setBrush(QColor(190, 45, 45));
    painter.drawRoundedRect(2, 7, 12, 8, 2, 2);
    painter.setPen(Qt::white);
    painter.drawEllipse(7, 9, 2, 2);
    return QIcon(pixmap);
}

[[maybe_unused]] bool LoadServerConnection(QString* endpoint,
                                           QString* config_path,
                                           QString* error) {
    const auto arguments = QCoreApplication::arguments();
    bool explicit_config = false;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        if (arguments.at(index) == "--connection-config") {
            if (index + 1 >= arguments.size()) {
                *error = QObject::tr(
                    "参数 --connection-config 后需要配置文件路径。");
                return false;
            }
            *config_path = arguments.at(index + 1);
            explicit_config = true;
            break;
        }
    }
    if (config_path->isEmpty()) {
        *config_path = QCoreApplication::applicationDirPath() +
                       "/server-connection.json";
    }

    QFile file(*config_path);
    if (!file.exists()) {
        if (explicit_config) {
            *error = QObject::tr("找不到服务器连接配置文件：%1")
                         .arg(*config_path);
        }
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QObject::tr("无法读取服务器连接配置：%1")
                     .arg(file.errorString());
        return false;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        *error = QObject::tr("服务器连接配置 JSON 无效：%1")
                     .arg(parse_error.errorString());
        return false;
    }
    if (!document.isObject()) {
        *error = QObject::tr("服务器连接配置的顶层必须是 JSON 对象。");
        return false;
    }
    const auto server = document.object().value("server").toObject();
    const auto scheme = server.value("scheme").toString();
    const auto host = server.value("host").toString().trimmed();
    const auto port_value = server.value("port");
    const int port = port_value.toInt(-1);
    if ((scheme != "https" && scheme != "http") || host.isEmpty() ||
        !port_value.isDouble() || port < 1 || port > 65535) {
        *error = QObject::tr(
            "配置必须包含有效的 server.scheme、server.host 和 server.port。");
        return false;
    }
    if (scheme != "https" && host != "localhost" &&
        host != "127.0.0.1" && host != "::1") {
        *error = QObject::tr("非本机服务器必须使用 HTTPS。");
        return false;
    }

    QUrl url;
    url.setScheme(scheme);
    url.setHost(host);
    url.setPort(port);
    if (!url.isValid() || url.host().isEmpty()) {
        *error = QObject::tr("服务器地址无效。");
        return false;
    }
    *endpoint = url.toString(QUrl::FullyEncoded);
    return true;
}

}  // namespace
