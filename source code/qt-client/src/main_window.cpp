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
constexpr int UploadPermission = 5;

QStringList DefaultNotifications() {
    return {
        QObject::tr("权限标志：读，写，删，手动增版，手动删版，保存策略（自动增版和覆盖），上传。"),
        QObject::tr("双击打开文件就立即锁定，不用时请手动释放锁，点击刷新会折叠文件夹，请记住你的文件位置。"),
        QObject::tr("bug请报邮箱1129111763@qq.com。")};
}

QString NodePath(const QTreeWidgetItem* item) {
    return item == nullptr ? QString{} : item->data(0, PathRole).toString();
}

QString ParentPath(const QString& path) {
    const int separator = path.lastIndexOf('/');
    return separator <= 0 ? "/" : path.left(separator);
}

bool IsDirectory(const QTreeWidgetItem* item) {
    return item != nullptr && item->data(0, TypeRole).toString() == "dir";
}

QString DisplayName(const QString& path) {
    return path.section('/', -1);
}

bool HasPermission(const QTreeWidgetItem* item, int index) {
    if (item == nullptr) return false;
    const auto bits = item->data(0, PermissionsRole).toString();
    return index >= 0 && index < bits.size() && bits.at(index) == '1';
}

QString FormatFileSize(qint64 bytes) {
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

QByteArray MethodName(const char* method) {
    return QByteArray(method);
}

QIcon RedLockIcon() {
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

bool LoadServerConnection(QString* endpoint, QString* config_path,
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

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      network_(new QNetworkAccessManager(this)),
      endpoint_edit_(new QLineEdit(this)),
      connect_button_(new QPushButton(tr("连接并登录"), this)),
      refresh_button_(new QPushButton(tr("刷新"), this)),
      open_button_(new QPushButton(tr("打开"), this)),
      save_button_(new QPushButton(tr("上传本地文件"), this)),
      status_label_(new QLabel(tr("未连接"), this)),
      message_view_(new QPlainTextEdit(this)),
      tree_(new QTreeWidget(this)),
      heartbeat_timer_(new QTimer(this)),
      notifications_timer_(new QTimer(this)),
      message_timer_(new QTimer(this)),
      temporary_dir_(QDir::tempPath() + "/dms-client-XXXXXX") {
    setWindowTitle(tr("图纸文件管理系统"));
    resize(960, 680);

    QSettings settings;
    endpoint_edit_->setPlaceholderText(
        tr("HTTPS 服务地址，例如 https://dms.example.com:8080"));
    QString configured_endpoint;
    QString connection_config_path;
    QString connection_config_error;
    const bool has_connection_config = LoadServerConnection(
        &configured_endpoint, &connection_config_path,
        &connection_config_error);
    if (!connection_config_error.isEmpty()) {
        QMessageBox::warning(this, tr("服务器连接配置"),
                             connection_config_error);
    }
    // Prefer the portable file so address changes do not depend on per-user settings.
    endpoint_edit_->setText(
        has_connection_config
            ? configured_endpoint
            : settings.value("server/endpoint",
                             "https://127.0.0.1:8080").toString());
    endpoint_edit_->setClearButtonEnabled(true);
    refresh_button_->setEnabled(false);
    open_button_->setEnabled(false);
    save_button_->setEnabled(false);

    auto* connection_layout = new QHBoxLayout;
    connection_layout->addWidget(new QLabel(tr("服务器:"), this));
    connection_layout->addWidget(endpoint_edit_, 1);
    connection_layout->addWidget(connect_button_);
    connection_layout->addWidget(refresh_button_);

    auto* action_layout = new QHBoxLayout;
    action_layout->addWidget(open_button_);
    action_layout->addWidget(save_button_);
    action_layout->addStretch();
    action_layout->addWidget(status_label_);

    tree_->setColumnCount(9);
    tree_->setHeaderLabels({
        tr("名称"), tr("修改日期"), tr("类型"), tr("大小"),
        tr("创建者"), tr("权限"), tr("版本"), tr("锁定用户"),
        tr("锁定时间")});
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setMinimumSectionSize(64);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionsMovable(true);
    tree_->setColumnWidth(0, 240);
    tree_->setColumnWidth(1, 150);
    tree_->setColumnWidth(2, 100);
    tree_->setColumnWidth(3, 100);
    tree_->setColumnWidth(4, 100);
    tree_->setColumnWidth(5, 100);
    tree_->setColumnWidth(6, 80);
    tree_->setColumnWidth(7, 100);
    tree_->setColumnWidth(8, 150);
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(20, 20));
    message_view_->setReadOnly(true);
    message_view_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    message_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    message_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    message_view_->setFixedHeight(fontMetrics().lineSpacing() * 3 + 16);
    message_view_->setPlainText(DefaultNotifications().join('\n'));

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->addLayout(connection_layout);
    layout->addLayout(action_layout);
    layout->addWidget(message_view_);
    layout->addWidget(tree_, 1);
    setCentralWidget(central);

    heartbeat_timer_->setInterval(5 * 60 * 1000);
    notifications_timer_->setInterval(1000);
    message_timer_->setInterval(2000);
    connect(heartbeat_timer_, &QTimer::timeout,
            this, &MainWindow::sendHeartbeats);
    connect(notifications_timer_, &QTimer::timeout,
            this, &MainWindow::pollNotifications);
    connect(message_timer_, &QTimer::timeout,
            this, &MainWindow::showNextNotification);
    connect(connect_button_, &QPushButton::clicked,
            this, [this] {
                if (token_.isEmpty()) {
                    connectToServer();
                } else {
                    logout();
                }
            });
    connect(refresh_button_, &QPushButton::clicked,
            this, &MainWindow::refreshTree);
    connect(open_button_, &QPushButton::clicked,
            this, [this] { openFile(selectedItem()); });
    connect(save_button_, &QPushButton::clicked,
            this, [this] { uploadLocalFile(selectedItem()); });
    connect(tree_, &QTreeWidget::itemExpanded,
            this, &MainWindow::loadChildren);
    connect(tree_, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem* item, int) {
                if (!IsDirectory(item)) openFile(item);
            });
    connect(tree_, &QTreeWidget::currentItemChanged,
            this, [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                open_button_->setEnabled(!token_.isEmpty() &&
                    current != nullptr &&
                    !IsDirectory(current) &&
                    HasPermission(current, ReadPermission));
                save_button_->setEnabled(!token_.isEmpty() &&
                    current != nullptr &&
                    HasPermission(current, UploadPermission));
            });
    connect(tree_, &QTreeWidget::customContextMenuRequested,
            this, &MainWindow::showContextMenu);

    if (!temporary_dir_.isValid()) {
        connect_button_->setEnabled(false);
        QMessageBox::critical(this, tr("错误"),
                              tr("无法创建本地临时文件夹，客户端无法下载图纸。"));
    }
}

QUrl MainWindow::apiUrl(
    const QString& route,
    const QList<QPair<QString, QString>>& query) const {
    QUrl url = base_url_;
    QString path = url.path();
    if (!path.endsWith('/')) path += '/';
    QString relative = route;
    while (relative.startsWith('/')) relative.remove(0, 1);
    path += relative;
    url.setPath(path);
    if (!query.isEmpty()) {
        QUrlQuery parameters;
        for (const auto& [key, value] : query) {
            parameters.addQueryItem(key, value);
        }
        url.setQuery(parameters);
    }
    return url;
}

QNetworkRequest MainWindow::makeRequest(const QUrl& url) const {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "DMS-Qt-Client/1.0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    if (!token_.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    }
    return request;
}

void MainWindow::sendRequest(const QByteArray& method, const QUrl& url,
                             const QByteArray& body,
                             const QByteArray& content_type,
                             ReplyHandler handler) {
    auto request = makeRequest(url);
    if (!content_type.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QString::fromLatin1(content_type));
    } else if (method == "POST") {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          "application/json");
    }

    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = network_->get(request);
    } else if (method == "POST") {
        reply = network_->post(request, body);
    } else if (method == "PUT") {
        reply = network_->put(request, body);
    } else if (method == "DELETE") {
        reply = network_->deleteResource(request);
    } else {
        request.setAttribute(QNetworkRequest::CustomVerbAttribute, method);
        reply = network_->sendCustomRequest(request, method, body);
    }

    connect(reply, &QNetworkReply::finished, this,
            [reply, handler = std::move(handler)]() mutable {
                handler(reply);
                reply->deleteLater();
            });
}

void MainWindow::connectToServer() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("连接服务器"));
    auto* form = new QFormLayout(&dialog);
    auto* endpoint = new QLineEdit(endpoint_edit_->text(), &dialog);
    auto* username = new QLineEdit(&dialog);
    auto* password = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    username->setPlaceholderText(tr("用户名"));
    password->setPlaceholderText(tr("密码"));
    form->addRow(tr("服务器地址"), endpoint);
    form->addRow(tr("用户名"), username);
    form->addRow(tr("密码"), password);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    auto server_url = endpoint->text().trimmed();
    if (!server_url.contains("://")) server_url.prepend("https://");
    QUrl parsed_url(server_url);
    const auto scheme = parsed_url.scheme().toLower();
    const auto host = parsed_url.host();
    QHostAddress address;
    const bool loopback = host.compare("localhost", Qt::CaseInsensitive) == 0 ||
                          (address.setAddress(host) && address.isLoopback());
    if (!parsed_url.isValid() || host.isEmpty() ||
        !parsed_url.userName().isEmpty() || !parsed_url.password().isEmpty() ||
        !parsed_url.query().isEmpty() || !parsed_url.fragment().isEmpty() ||
        (scheme != "https" && !(scheme == "http" && loopback))) {
        QMessageBox::warning(
            this, tr("服务器地址无效"),
            tr("远程服务器必须使用 HTTPS。仅允许对 localhost 使用 HTTP。"));
        return;
    }
    if (username->text().trimmed().isEmpty() || password->text().isEmpty()) {
        QMessageBox::warning(this, tr("登录信息不完整"),
                             tr("请输入用户名和密码。"));
        return;
    }

    parsed_url.setPath(parsed_url.path().isEmpty() ? "/" : parsed_url.path());
    parsed_url.setQuery(QString{});
    parsed_url.setFragment(QString{});
    base_url_ = parsed_url;
    endpoint_edit_->setText(parsed_url.toString(QUrl::StripTrailingSlash));
    endpoint_edit_->setEnabled(false);
    connect_button_->setEnabled(false);
    status_label_->setText(tr("正在登录…"));
    QSettings settings;
    settings.setValue("server/endpoint", endpoint_edit_->text());

    const QByteArray body = QJsonDocument(QJsonObject{
        {"user", username->text().trimmed()},
        {"pass", password->text()}
    }).toJson(QJsonDocument::Compact);
    password->clear();
    const auto attempted_user = username->text().trimmed();
    sendRequest(MethodName("POST"), apiUrl("/api/login"), body,
                "application/json",
                [this, attempted_user](QNetworkReply* reply) {
                    const QByteArray response = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        token_.clear();
                        endpoint_edit_->setEnabled(true);
                        connect_button_->setEnabled(true);
                        status_label_->setText(tr("登录失败"));
                        showReplyError(reply, tr("登录"));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(response);
                    const auto object = document.object();
                    const auto token = object.value("token").toString();
                    if (!document.isObject() || token.isEmpty()) {
                        endpoint_edit_->setEnabled(true);
                        connect_button_->setEnabled(true);
                        status_label_->setText(tr("登录失败"));
                        QMessageBox::warning(
                            this, tr("登录失败"),
                            tr("服务器响应缺少有效的登录令牌。"));
                        return;
                    }
                    token_ = token;
                    username_ = object.value("user").toString(attempted_user);
                    connect_button_->setText(tr("注销"));
                    connect_button_->setEnabled(true);
                    refresh_button_->setEnabled(true);
                    status_label_->setText(tr("已连接：%1").arg(username_));
                    refreshTree();
                    heartbeat_timer_->start();
                    notifications_timer_->start();
                    message_timer_->start();
                });
}

void MainWindow::logout() {
    if (!open_documents_.isEmpty() &&
        QMessageBox::question(
            this, tr("注销"),
            tr("仍有图纸在本地打开。请先保存图纸并上传，然后再注销。\n"
               "继续注销会释放服务器占用锁。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
            QMessageBox::Yes) {
        return;
    }
    heartbeat_timer_->stop();
    notifications_timer_->stop();
    message_timer_->stop();
    if (!token_.isEmpty()) {
        sendRequest(MethodName("POST"), apiUrl("/api/logout"), {}, {},
                    [this](QNetworkReply* reply) {
                        if (reply->error() != QNetworkReply::NoError) {
                            showReplyError(reply, tr("注销"));
                        }
                    });
    }
    token_.clear();
    username_.clear();
    notifications_request_in_flight_ = false;
    notification_queue_.clear();
    displayed_notifications_.clear();
    message_view_->setPlainText(DefaultNotifications().join('\n'));
    open_documents_.clear();
    children_requests_.clear();
    nodes_.clear();
    tree_->clear();
    endpoint_edit_->setEnabled(true);
    connect_button_->setText(tr("连接并登录"));
    connect_button_->setEnabled(true);
    refresh_button_->setEnabled(false);
    open_button_->setEnabled(false);
    save_button_->setEnabled(false);
    status_label_->setText(tr("已注销"));
}

void MainWindow::refreshTree() {
    if (token_.isEmpty()) return;
    children_requests_.clear();
    tree_->clear();
    nodes_.clear();
    auto* root = new QTreeWidgetItem(tree_);
    root->setText(0, "/");
    root->setData(0, PathRole, "/");
    root->setData(0, TypeRole, "dir");
    root->setData(0, PermissionsRole, "rwd");
    root->setData(0, LoadedRole, false);
    root->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
    root->setExpanded(true);
    nodes_.insert("/", root);
}

void MainWindow::loadChildren(QTreeWidgetItem* parent) {
    if (parent == nullptr || token_.isEmpty() ||
        !IsDirectory(parent) || parent->data(0, LoadedRole).toBool()) {
        return;
    }
    const QString path = NodePath(parent);
    const quint64 request_id = ++next_children_request_id_;
    const QString request_token = token_;
    children_requests_.insert(path, request_id);
    parent->setData(0, LoadedRole, true);
    const QString descendant_prefix = path == "/" ? "/" : path + "/";
    for (auto it = children_requests_.begin();
         it != children_requests_.end();) {
        if (it.key() != path && it.key().startsWith(descendant_prefix)) {
            it = children_requests_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        if (it.key() != path &&
            it.key().startsWith(descendant_prefix)) {
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
    const auto old_children = parent->takeChildren();
    for (auto* child : old_children) delete child;
    status_label_->setText(tr("正在读取 %1").arg(path));
    sendRequest(MethodName("GET"),
                apiUrl("/api/list", {{"path", path}}), {}, {},
                [this, request_id, request_token, path](QNetworkReply* reply) {
                    if (token_ != request_token ||
                        children_requests_.value(path) != request_id) {
                        return;
                    }
                    auto* current_parent = nodes_.value(path, nullptr);
                    children_requests_.remove(path);
                    if (current_parent == nullptr) return;
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        current_parent->setData(0, LoadedRole, false);
                        showReplyError(reply, tr("读取目录"));
                        status_label_->setText(tr("目录读取失败"));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(bytes);
                    if (!document.isObject() ||
                        !document.object().value("nodes").isArray()) {
                        current_parent->setData(0, LoadedRole, false);
                        status_label_->setText(tr("服务器响应格式错误"));
                        QMessageBox::warning(this, tr("读取目录失败"),
                                             tr("服务器没有返回有效的 nodes 列表。"));
                        return;
                    }
                    current_parent->setData(
                        0, PermissionsRole,
                        document.object().value("directory_perms").toString());
                    for (const auto& node : document.object().value("nodes").toArray()) {
                        if (node.isObject()) {
                            addNode(current_parent, node.toObject());
                        }
                    }
                    if (selectedItem() == current_parent) {
                        const auto bits = document.object()
                                              .value("directory_perms")
                                              .toString();
                        save_button_->setEnabled(
                            bits.size() > UploadPermission &&
                            bits.at(UploadPermission) == '1');
                    }
                    status_label_->setText(tr("已加载 %1").arg(path));
                });
}

void MainWindow::addNode(QTreeWidgetItem* parent, const QJsonObject& node) {
    const QString name = node.value("name").toString();
    const QString type = node.value("type").toString();
    const QString permissions = node.value("perms").toString();
    if (name.isEmpty() || (type != "file" && type != "dir")) return;
    const QString parent_path = NodePath(parent);
    const QString path = parent_path == "/" ? "/" + name : parent_path + "/" + name;

    auto* item = new QTreeWidgetItem(parent);
    const auto modified = node.value("modified").toString();
    const auto size = node.value("size").toString();
    const auto owner = node.value("owner").toString();
    const auto version = node.value("version").toString();
    const auto locked_by = node.value("locked_by").toString();
    const auto locked_at = node.value("locked_at").toString();
    item->setText(0, name);
    item->setText(1, modified.isEmpty() ? tr("—") : modified);
    const QString extension = QFileInfo(name).suffix().toUpper();
    item->setText(2, type == "dir"
                         ? tr("目录")
                         : (extension.isEmpty() ? tr("文件")
                                                : tr("%1 文件").arg(extension)));
    bool size_ok = false;
    const auto size_bytes = size.toLongLong(&size_ok);
    item->setText(3, size_ok ? FormatFileSize(size_bytes) : tr("—"));
    item->setText(4, owner.isEmpty() ? tr("—") : owner);
    item->setText(5, permissions);
    item->setText(6, version.isEmpty() ? tr("—") : version);
    item->setText(7, locked_by);
    item->setText(8, locked_at.isEmpty() ? tr("—") : locked_at);
    item->setData(0, PathRole, path);
    item->setData(0, TypeRole, type);
    item->setData(0, PermissionsRole, permissions);
    item->setData(0, LoadedRole, false);
    item->setIcon(0, style()->standardIcon(
        type == "dir" ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
    nodes_.insert(path, item);
    if (!locked_by.isEmpty()) setItemLockState(path, locked_by, locked_at);
    if (type == "dir") {
        auto* placeholder = new QTreeWidgetItem(item);
        placeholder->setText(0, tr("加载中…"));
    }
}

void MainWindow::openFile(QTreeWidgetItem* item, const QString& application) {
    if (item == nullptr || IsDirectory(item) || token_.isEmpty()) return;
    const QString path = NodePath(item);
    auto launch = [application](const QString& local_file) {
        if (application.isEmpty()) {
            return QDesktopServices::openUrl(QUrl::fromLocalFile(local_file));
        }
        return QProcess::startDetached(application, {local_file});
    };
    if (open_documents_.contains(path)) {
        const auto document = open_documents_.value(path);
        if (!launch(document.local_file)) {
            QMessageBox::warning(this, tr("打开文件"),
                                 tr("无法启动所选程序打开该图纸。"));
        }
        return;
    }

    const bool writable = HasPermission(item, WritePermission);
    auto download = [this, path, item, launch](const bool lock_held,
                                      const QString& occupied_by,
                                      const QString& locked_at) {
        sendRequest(MethodName("GET"),
                    apiUrl("/api/file", {{"path", path}}), {}, {},
                    [this, path, item, lock_held, occupied_by, locked_at,
                     launch](QNetworkReply* reply) {
                        const QByteArray bytes = reply->readAll();
                        if (reply->error() != QNetworkReply::NoError) {
                            if (lock_held) releaseServerLock(path);
                            showReplyError(reply, tr("下载图纸"));
                            return;
                        }
                        const QString suffix = QFileInfo(path).suffix();
                        const QByteArray digest = QCryptographicHash::hash(
                            path.toUtf8(), QCryptographicHash::Sha256).toHex();
                        QString local_file = temporary_dir_.filePath(
                            QString::fromLatin1(digest) +
                            (suffix.isEmpty() ? QString{} : "." + suffix));
                        QFile file(local_file);
                        if (!file.open(QIODevice::WriteOnly) ||
                            file.write(bytes) != bytes.size() || !file.flush()) {
                            if (file.isOpen()) file.close();
                            if (lock_held) releaseServerLock(path);
                            QMessageBox::critical(
                                this, tr("下载图纸"),
                                tr("无法写入本地临时文件：%1").arg(local_file));
                            return;
                        }
                        file.close();
                        open_documents_.insert(path,
                            OpenDocument{local_file, lock_held, lock_held,
                                         occupied_by, locked_at});
                        setItemLockState(path, lock_held ? username_ : occupied_by,
                                         locked_at);
                        if (launch(local_file)) {
                            status_label_->setText(
                                lock_held ? tr("已打开并取得编辑锁")
                                          : tr("只读打开：%1").arg(occupied_by));
                        } else {
                            QMessageBox::warning(
                                this, tr("打开图纸"),
                                tr("图纸已下载到：\n%1\n但无法启动打开程序。")
                                    .arg(local_file));
                        }
                        save_button_->setEnabled(lock_held &&
                                                  selectedItem() == item);
                    });
    };

    if (!writable) {
        download(false, tr("无写权限"), {});
        return;
    }
    const QByteArray body = QJsonDocument(QJsonObject{{"path", path}})
                                .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/file/open"), body,
                "application/json",
                [this, path, download = std::move(download)](
                    QNetworkReply* reply) mutable {
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("打开图纸"));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(bytes);
                    if (!document.isObject()) {
                        QMessageBox::warning(this, tr("打开图纸"),
                                             tr("服务器返回了无效的占用锁状态。"));
                        return;
                    }
                    const auto response = document.object();
                    const auto state = response.value("status").toString();
                    const auto locked_at =
                        response.value("locked_at").toString();
                    if (state == "editable") {
                        download(true, {}, locked_at);
                    } else if (state == "read_only") {
                        download(false,
                                 response.value("occupied_by").toString(
                                     tr("其他用户")),
                                 locked_at);
                    } else {
                        QMessageBox::warning(this, tr("打开图纸"),
                                             tr("服务器返回了未知的占用锁状态。"));
                    }
                });
}

void MainWindow::downloadCurrent(QTreeWidgetItem* item) {
    if (item == nullptr || IsDirectory(item) || token_.isEmpty()) return;
    const QString path = NodePath(item);
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("下载图纸"), DisplayName(path));
    if (destination.isEmpty()) return;
    sendRequest(MethodName("GET"), apiUrl("/api/file", {{"path", path}}), {}, {},
                [this, destination](QNetworkReply* reply) {
                    const QByteArray data = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("下载图纸"));
                        return;
                    }
                    QFile file(destination);
                    if (!file.open(QIODevice::WriteOnly) ||
                        file.write(data) != data.size() || !file.flush()) {
                        QMessageBox::critical(this, tr("下载图纸"),
                                              tr("无法写入文件：%1").arg(destination));
                        return;
                    }
                    status_label_->setText(tr("已下载到 %1").arg(destination));
                });
}

void MainWindow::uploadLocalFile(QTreeWidgetItem* item) {
    if (item == nullptr || token_.isEmpty()) return;
    if (!HasPermission(item, UploadPermission)) {
        QMessageBox::warning(this, tr("上传本地文件"),
                             tr("此位置没有上传权限。"));
        return;
    }

    QMessageBox chooser(this);
    chooser.setWindowTitle(tr("上传本地内容"));
    chooser.setText(tr("选择要上传的内容类型"));
    auto* file_choice = chooser.addButton(tr("文件"), QMessageBox::AcceptRole);
    auto* folder_choice = chooser.addButton(tr("文件夹"), QMessageBox::AcceptRole);
    chooser.addButton(QMessageBox::Cancel);
    chooser.exec();
    if (chooser.clickedButton() == folder_choice) {
        const auto source_directory = QFileDialog::getExistingDirectory(
            this, tr("选择要上传的文件夹"));
        if (!source_directory.isEmpty()) uploadFolder(item, source_directory);
        return;
    }
    if (chooser.clickedButton() != file_choice) return;

    // Choose a local source file; the selected tree row determines its server folder.
    const QString source = QFileDialog::getOpenFileName(
        this, tr("选择要上传的本地文件"));
    if (source.isEmpty()) return;

    const QString target_directory = IsDirectory(item)
        ? NodePath(item)
        : ParentPath(NodePath(item));
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("上传本地文件"),
                             tr("无法读取本地文件：%1").arg(file.errorString()));
        return;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, tr("上传本地文件"),
                             tr("读取本地文件失败：%1").arg(file.errorString()));
        return;
    }

    const QString name = QFileInfo(source).fileName();
    const QString uploaded_path = target_directory == "/"
        ? "/" + name
        : target_directory + "/" + name;
    sendRequest(MethodName("POST"),
                apiUrl("/api/upload",
                       {{"path", target_directory}, {"name", name}}),
                contents, "application/octet-stream",
                [this, uploaded_path](QNetworkReply* reply) {
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("上传本地文件"));
                        return;
                    }
                    const auto response = QJsonDocument::fromJson(bytes).object();
                    status_label_->setText(
                        tr("上传成功，最新版本 v%1")
                            .arg(response.value("version").toInt()));
                    reloadParent(uploaded_path);
                });
}

void MainWindow::uploadFolder(QTreeWidgetItem* item,
                              const QString& source_directory) {
    if (item == nullptr || token_.isEmpty() ||
        !HasPermission(item, UploadPermission)) {
        return;
    }
    QDir source_root(source_directory);
    if (!source_root.exists()) {
        QMessageBox::warning(this, tr("上传文件夹"),
                             tr("所选文件夹不存在或无法访问。"));
        return;
    }
    const QString destination_parent = IsDirectory(item)
        ? NodePath(item)
        : ParentPath(NodePath(item));
    const QString folder_name = QFileInfo(source_directory).fileName();
    if (folder_name.isEmpty()) {
        QMessageBox::warning(this, tr("上传文件夹"),
                             tr("无法确定所选文件夹名称。"));
        return;
    }

    auto job = std::make_shared<FolderUploadJob>();
    job->source_directory = source_directory;
    job->target_directory = destination_parent == "/"
        ? "/" + folder_name
        : destination_parent + "/" + folder_name;
    // Create every remote directory before uploading files into it.
    job->directories.push_back(job->target_directory);

    QDirIterator directories(
        source_directory,
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (directories.hasNext()) {
        const QString source_path = directories.next();
        const QString relative = source_root.relativeFilePath(source_path);
        job->directories.push_back(job->target_directory + "/" + relative);
    }
    QDirIterator files(
        source_directory,
        QDir::Files | QDir::Hidden | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (files.hasNext()) job->files.push_back(files.next());

    status_label_->setText(tr("正在上传文件夹：%1").arg(folder_name));
    uploadNextFolderDirectory(job);
}

void MainWindow::uploadNextFolderDirectory(
    const std::shared_ptr<FolderUploadJob>& job) {
    if (job->next_directory >= job->directories.size()) {
        uploadNextFolderFile(job);
        return;
    }
    const QString path = job->directories.at(job->next_directory++);
    sendRequest(MethodName("POST"),
                apiUrl("/api/upload-directory", {{"path", path}}), {}, {},
                [this, job](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("创建上传目录"));
                        return;
                    }
                    uploadNextFolderDirectory(job);
                });
}

void MainWindow::uploadNextFolderFile(
    const std::shared_ptr<FolderUploadJob>& job) {
    if (job->next_file >= job->files.size()) {
        status_label_->setText(tr("文件夹上传完成：%1").arg(job->target_directory));
        reloadParent(job->target_directory);
        return;
    }

    const QString source_path = job->files.at(job->next_file++);
    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("上传文件夹"),
                             tr("无法读取文件 %1：%2")
                                 .arg(source_path, source.errorString()));
        status_label_->setText(tr("文件夹上传中断"));
        return;
    }
    const QByteArray contents = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, tr("上传文件夹"),
                             tr("读取文件失败：%1").arg(source_path));
        status_label_->setText(tr("文件夹上传中断"));
        return;
    }

    const QFileInfo source_info(source_path);
    const QString file_name = source_info.fileName();
    const QString relative_parent =
        QDir(job->source_directory).relativeFilePath(source_info.absolutePath());
    const QString server_parent = relative_parent == "."
        ? job->target_directory
        : job->target_directory + "/" + relative_parent;

    sendRequest(MethodName("POST"),
                apiUrl("/api/upload",
                       {{"path", server_parent}, {"name", file_name}}),
                contents, "application/octet-stream",
                [this, job](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("上传文件夹中的文件"));
                        status_label_->setText(tr("文件夹上传中断"));
                        return;
                    }
                    uploadNextFolderFile(job);
                });
}

void MainWindow::uploadCurrent(QTreeWidgetItem* item) {
    if (item == nullptr || token_.isEmpty()) return;
    const QString path = NodePath(item);
    const auto document = open_documents_.constFind(path);
    if (document == open_documents_.cend() ||
        !document->editable || !document->lock_held) {
        QMessageBox::warning(this, tr("保存当前编辑内容"),
                             tr("此文件没有可用的编辑锁。请先打开并取得编辑权限。"));
        return;
    }
    QFile file(document->local_file);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("保存当前编辑内容"),
                             tr("无法读取本地图纸：%1").arg(document->local_file));
        return;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, tr("保存当前编辑内容"),
                             tr("读取本地图纸失败：%1").arg(file.errorString()));
        return;
    }
    sendRequest(MethodName("PUT"),
                apiUrl("/api/file", {{"path", path}}), contents,
                "application/octet-stream",
                [this, path](QNetworkReply* reply) {
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("保存当前编辑内容"));
                        return;
                    }
                    const auto response = QJsonDocument::fromJson(bytes).object();
                    status_label_->setText(
                        tr("保存成功，版本 rev_%1")
                            .arg(response.value("version").toInt()));
                    reloadParent(path);
                });
}

void MainWindow::closeDocument(const QString& path) {
    const auto found = open_documents_.find(path);
    if (found == open_documents_.end()) return;
    const bool release_lock = found->lock_held;
    open_documents_.erase(found);
    setItemLockState(path, {});
    if (selectedItem() != nullptr && NodePath(selectedItem()) == path) {
        save_button_->setEnabled(false);
    }
    if (release_lock) releaseServerLock(path);
}

void MainWindow::releaseServerLock(const QString& path) {
    if (token_.isEmpty()) return;
    const QByteArray body = QJsonDocument(QJsonObject{{"path", path}})
                                .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/file/close"), body,
                "application/json",
                [this](QNetworkReply* reply) {
                    if (reply->error() == QNetworkReply::NoError) {
                        status_label_->setText(tr("已释放文件占用锁"));
                    } else {
                        showReplyError(reply, tr("释放文件占用锁"));
                    }
                });
}

void MainWindow::sendHeartbeats() {
    if (token_.isEmpty()) return;
    const auto paths = open_documents_.keys();
    for (const auto& path : paths) {
        const auto document = open_documents_.constFind(path);
        if (document == open_documents_.cend() || !document->lock_held) continue;
        const QByteArray body = QJsonDocument(QJsonObject{{"path", path}})
                                    .toJson(QJsonDocument::Compact);
        sendRequest(MethodName("POST"), apiUrl("/api/file/heartbeat"), body,
                    "application/json",
                    [this, path](QNetworkReply* reply) {
                        if (reply->error() != QNetworkReply::NoError) {
                            auto document = open_documents_.find(path);
                            if (document != open_documents_.end()) {
                                document->lock_held = false;
                                document->editable = false;
                                document->occupied_by = tr("占用锁已失效");
                            }
                            setItemLockState(path, tr("占用锁已失效"));
                            if (selectedItem() != nullptr &&
                                NodePath(selectedItem()) == path) {
                                save_button_->setEnabled(false);
                            }
                            showReplyError(reply, tr("续期文件占用锁"));
                        }
                    });
    }
}

void MainWindow::pollNotifications() {
    if (token_.isEmpty() || notifications_request_in_flight_) return;
    notifications_request_in_flight_ = true;
    sendRequest(MethodName("GET"), apiUrl("/api/notifications"), {}, {},
                [this](QNetworkReply* reply) {
                    notifications_request_in_flight_ = false;
                    const QByteArray response = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        status_label_->setText(
                            tr("通知同步失败：%1").arg(reply->errorString()));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(response);
                    if (!document.isObject() ||
                        !document.object().value("messages").isArray()) {
                        status_label_->setText(tr("通知同步失败：服务器响应格式无效"));
                        return;
                    }
                    for (const auto& value :
                         document.object().value("messages").toArray()) {
                        if (!value.isObject()) continue;
                        const auto message = value.toObject();
                        const auto type = message.value("type").toString();
                        if (type == "message") {
                            notification_queue_.enqueue(
                                message.value("text").toString());
                        } else if (type == "lock") {
                            setItemLockState(
                                message.value("path").toString(),
                                message.value("user").toString(),
                                message.value("locked_at").toString());
                        }
                    }
                });
}

void MainWindow::showNextNotification() {
    if (notification_queue_.isEmpty()) {
        displayed_notifications_.clear();
        message_view_->setPlainText(DefaultNotifications().join('\n'));
        return;
    }
    // The timer presents one message at a time and keeps only the newest three.
    displayed_notifications_.prepend(notification_queue_.dequeue());
    while (displayed_notifications_.size() > 3) {
        displayed_notifications_.removeLast();
    }
    message_view_->setPlainText(displayed_notifications_.join('\n'));
}

void MainWindow::requestEdit(QTreeWidgetItem* item) {
    if (item == nullptr || token_.isEmpty() || IsDirectory(item)) return;
    if (!HasPermission(item, ReadPermission)) {
        QMessageBox::warning(this, tr("请求编辑"),
                             tr("你没有读取该文件的权限。"));
        return;
    }
    const QString path = NodePath(item);
    const QByteArray body =
        QJsonDocument(QJsonObject{{"path", path}})
            .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/file/request-edit"), body,
                "application/json",
                [this](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("请求编辑"));
                        return;
                    }
                    status_label_->setText(tr("编辑请求已发送"));
                });
}

void MainWindow::showContextMenu(const QPoint& position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr || token_.isEmpty()) return;
    tree_->setCurrentItem(item);
    const QString path = NodePath(item);
    const QString permissions = item->data(0, PermissionsRole).toString();
    const bool directory = IsDirectory(item);
    QMenu menu(this);
    if (directory) {
        auto* create_action = menu.addAction(tr("新建文件夹…"));
        auto* upload_action = menu.addAction(tr("上传本地文件到此目录…"));
        upload_action->setEnabled(HasPermission(item, UploadPermission));
        QAction* rename_action = nullptr;
        QAction* delete_action = nullptr;
        if (path != "/") {
            rename_action = menu.addAction(tr("重命名/移动…"));
            delete_action = menu.addAction(tr("申请删除文件夹…"));
        }
        auto* refresh_action = menu.addAction(tr("刷新目录"));
        const auto* chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
        if (chosen == create_action) createDirectory(item);
        else if (chosen == upload_action) uploadLocalFile(item);
        else if (chosen == rename_action) renameNode(item);
        else if (chosen == delete_action) deleteNode(item);
        else if (chosen == refresh_action) {
            item->setData(0, LoadedRole, false);
            loadChildren(item);
        }
        return;
    }

    auto* open_action = menu.addAction(tr("打开"));
    auto* open_with_action = menu.addAction(tr("打开方式…"));
    auto* download_action = menu.addAction(tr("下载到…"));
    auto* upload_action = menu.addAction(tr("上传本地文件…"));
    QAction* request_edit_action = nullptr;
    const auto locked_by = item->data(0, OccupiedByRole).toString();
    if (!locked_by.isEmpty() && locked_by != username_) {
        request_edit_action = menu.addAction(tr("请求编辑"));
    }
    QAction* save_edit_action = nullptr;
    if (open_documents_.contains(path)) {
        save_edit_action = menu.addAction(tr("保存当前编辑内容"));
    }
    auto* add_version_action = menu.addAction(tr("手动增加版本"));
    auto* versions_action = menu.addAction(tr("历史版本…"));
    auto* delete_action = menu.addAction(tr("删除文件"));
    QAction* close_action = nullptr;
    if (open_documents_.contains(path)) {
        close_action = menu.addAction(tr("关闭文件并释放占用锁"));
    }
    open_action->setEnabled(HasPermission(item, ReadPermission));
    open_with_action->setEnabled(HasPermission(item, ReadPermission));
    upload_action->setEnabled(HasPermission(item, UploadPermission));
    if (save_edit_action != nullptr) {
        const auto document = open_documents_.value(path);
        save_edit_action->setEnabled(document.editable && document.lock_held);
    }
    const auto* chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
    if (chosen == open_action) openFile(item);
    else if (chosen == open_with_action) {
#ifdef _WIN32
        const QString filters = tr("应用程序 (*.exe);;所有文件 (*)");
#else
        const QString filters = tr("所有文件 (*)");
#endif
        const auto application = QFileDialog::getOpenFileName(
            this, tr("选择打开程序"), {}, filters);
        if (!application.isEmpty()) openFile(item, application);
    }
    else if (chosen == download_action) downloadCurrent(item);
    else if (chosen == upload_action) uploadLocalFile(item);
    else if (chosen == request_edit_action) requestEdit(item);
    else if (chosen == save_edit_action) uploadCurrent(item);
    else if (chosen == add_version_action) addManualVersion(item);
    else if (chosen == versions_action) showVersions(item);
    else if (chosen == delete_action) deleteNode(item);
    else if (chosen == close_action) {
        if (QMessageBox::question(
                this, tr("释放文件占用锁"),
                tr("请先确认 CAD 程序已关闭或不再修改此文件。"
                   "释放占用锁后，其他用户就可以编辑。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
            QMessageBox::Yes) {
            closeDocument(path);
        }
    }
}

void MainWindow::createDirectory(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("新建文件夹"), tr("文件夹名称："),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    if (name.contains('/') || name == "." || name == "..") {
        QMessageBox::warning(this, tr("文件夹名称无效"),
                             tr("名称不能包含路径分隔符或点目录。"));
        return;
    }
    const QString parent = NodePath(item);
    const QString path = parent == "/" ? "/" + name : parent + "/" + name;
    const QByteArray body = QJsonDocument(QJsonObject{{"path", path}})
                                .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/mkdir"), body,
                "application/json",
                [this, parent](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("新建文件夹"));
                        return;
                    }
                    reloadParent(parent);
                    status_label_->setText(tr("文件夹已创建"));
                });
}

void MainWindow::renameNode(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    const QString from = NodePath(item);
    const int separator = from.lastIndexOf('/');
    const QString parent = separator <= 0 ? "/" : from.left(separator);
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("重命名/移动"),
        tr("输入目标路径（以 / 开头），或输入新名称："),
        QLineEdit::Normal, DisplayName(from), &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    QString to = name;
    if (!to.startsWith('/')) to = (parent.isEmpty() ? "" : parent) + "/" + to;
    if (to == "/" || to.contains("/../") || to.endsWith("/..") ||
        to.contains("/./") || to.endsWith("/.")) {
        QMessageBox::warning(this, tr("目标路径无效"),
                             tr("请指定有效的逻辑路径。"));
        return;
    }
    const bool directory = IsDirectory(item);
    const QByteArray body = QJsonDocument(
        QJsonObject{{"from", from}, {"to", to}})
                                .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/rename"), body,
                "application/json",
                [this, from, to, parent, directory](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("重命名/移动"));
                        return;
                    }
                    const QString descendants_prefix = from + "/";
                    const auto open_paths = open_documents_.keys();
                    for (const auto& open_path : open_paths) {
                        if (open_path == from ||
                            (directory &&
                             open_path.startsWith(descendants_prefix))) {
                            const auto suffix = open_path == from
                                                    ? QString{}
                                                    : open_path.mid(from.size());
                            const QString moved_path = to + suffix;
                            const auto document = open_documents_.take(open_path);
                            open_documents_.insert(moved_path, document);
                            setItemLockState(moved_path, document.occupied_by,
                                             document.locked_at);
                        }
                    }
                    reloadParent(parent);
                    const int separator = to.lastIndexOf('/');
                    const QString destination_parent =
                        separator <= 0 ? "/" : to.left(separator);
                    if (destination_parent != parent) {
                        auto* destination = nodes_.value(destination_parent, nullptr);
                        if (destination != nullptr) {
                            destination->setData(0, LoadedRole, false);
                            loadChildren(destination);
                        }
                    }
                    status_label_->setText(tr("文件或文件夹已移动"));
                });
}

void MainWindow::deleteNode(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    const QString path = NodePath(item);
    const bool directory = IsDirectory(item);
    const auto confirmation = QMessageBox::question(
        this, directory ? tr("申请删除文件夹") : tr("删除文件"),
        directory ? tr("确定提交文件夹删除审批吗？目录会立即对用户隐藏。")
                  : tr("确定直接删除此文件吗？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirmation != QMessageBox::Yes) return;
    sendRequest(MethodName("DELETE"),
                apiUrl("/api/file", {{"path", path}}), {}, {},
                [this, path, directory](QNetworkReply* reply) {
                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError &&
                        !(directory && status == 202)) {
                        showReplyError(reply, tr("删除"));
                        return;
                    }
                    if (!directory) {
                        closeDocument(path);
                    } else {
                        const QString prefix = path + "/";
                        const auto open_paths = open_documents_.keys();
                        for (const auto& open_path : open_paths) {
                            if (open_path == path || open_path.startsWith(prefix)) {
                                open_documents_.remove(open_path);
                            }
                        }
                    }
                    reloadParent(path);
                    status_label_->setText(
                        directory ? tr("删除申请已提交，等待管理员审核")
                                  : tr("文件已删除"));
                    if (directory) {
                        QMessageBox::information(
                            this, tr("删除申请"),
                            QJsonDocument::fromJson(bytes).object()
                                .value("message").toString(
                                    tr("删除申请已提交，等待管理员审核")));
                    }
                });
}

void MainWindow::addManualVersion(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    const QString path = NodePath(item);
    const QByteArray body = QJsonDocument(QJsonObject{{"path", path}})
                                .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/file/version/manual"), body,
                "application/json",
                [this, path](QNetworkReply* reply) {
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("手动增加版本"));
                        return;
                    }
                    const auto response = QJsonDocument::fromJson(bytes).object();
                    QMessageBox::information(
                        this, tr("增加版本"),
                        tr("已创建版本 rev_%1")
                            .arg(response.value("version").toInt()));
                    reloadParent(path);
                });
}

void MainWindow::showVersions(QTreeWidgetItem* item) {
    if (item == nullptr) return;
    const QString path = NodePath(item);
    sendRequest(MethodName("GET"),
                apiUrl("/api/file/versions", {{"path", path}}), {}, {},
                [this, path](QNetworkReply* reply) {
                    const QByteArray bytes = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("读取历史版本"));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(bytes);
                    if (!document.isObject() ||
                        !document.object().value("versions").isArray()) {
                        QMessageBox::warning(this, tr("历史版本"),
                                             tr("服务器返回了无效的版本列表。"));
                        return;
                    }
                    const auto revisions =
                        document.object().value("versions").toArray();
                    if (revisions.isEmpty()) {
                        QMessageBox::information(this, tr("历史版本"),
                                                 tr("此文件没有历史版本。"));
                        return;
                    }
                    QStringList choices;
                    int latest = 0;
                    for (const auto& entry : revisions) {
                        const auto version = entry.toObject();
                        const int revision = version.value("rev").toInt();
                        latest = qMax(latest, revision);
                        choices.push_back(
                            QString("rev_%1  (%2)")
                                .arg(revision)
                                .arg(version.value("ts").toString()));
                    }
                    bool accepted = false;
                    const QString choice = QInputDialog::getItem(
                        this, tr("历史版本"), tr("选择版本："), choices,
                        choices.size() - 1, false, &accepted);
                    if (!accepted) return;
                    const int selected_revision = choice.section(' ', 0, 0)
                                                      .mid(4).toInt();
                    QMenu menu(this);
                    auto* download_action = menu.addAction(tr("下载此版本…"));
                    QAction* delete_action = nullptr;
                    if (selected_revision != latest) {
                        delete_action = menu.addAction(tr("删除此历史版本"));
                    }
                    const auto* chosen = menu.exec(QCursor::pos());
                    if (chosen == download_action) {
                        downloadVersion(path, selected_revision);
                    } else if (chosen == delete_action) {
                        deleteVersion(path, selected_revision);
                    }
                });
}

void MainWindow::downloadVersion(const QString& path, int revision) {
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("下载历史版本"),
        DisplayName(path) + QString(".rev_%1").arg(revision));
    if (destination.isEmpty()) return;
    sendRequest(MethodName("GET"),
                apiUrl("/api/file/version",
                       {{"path", path}, {"rev", QString::number(revision)}}),
                {}, {},
                [this, destination, revision](QNetworkReply* reply) {
                    const QByteArray data = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("下载历史版本"));
                        return;
                    }
                    QFile file(destination);
                    if (!file.open(QIODevice::WriteOnly) ||
                        file.write(data) != data.size() || !file.flush()) {
                        QMessageBox::critical(this, tr("下载历史版本"),
                                              tr("无法写入文件：%1").arg(destination));
                        return;
                    }
                    status_label_->setText(
                        tr("已下载历史版本 rev_%1").arg(revision));
                });
}

void MainWindow::deleteVersion(const QString& path, int revision) {
    if (QMessageBox::question(
            this, tr("删除历史版本"),
            tr("确定删除历史版本 rev_%1 吗？").arg(revision),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes) {
        return;
    }
    sendRequest(MethodName("DELETE"),
                apiUrl("/api/file/version",
                       {{"path", path}, {"rev", QString::number(revision)}}),
                {}, {},
                [this, path, revision](QNetworkReply* reply) {
                    if (reply->error() != QNetworkReply::NoError) {
                        showReplyError(reply, tr("删除历史版本"));
                        return;
                    }
                    status_label_->setText(
                        tr("已删除历史版本 rev_%1").arg(revision));
                    reloadParent(path);
                });
}

void MainWindow::setItemLockState(const QString& path,
                                  const QString& occupied_by,
                                  const QString& locked_at) {
    auto* item = nodes_.value(path, nullptr);
    if (item == nullptr) return;
    item->setData(0, OccupiedByRole, occupied_by);
    item->setData(0, LockTimeRole, locked_at);
    item->setText(7, occupied_by);
    item->setText(8, locked_at.isEmpty() ? tr("—") : locked_at);
    if (!occupied_by.isEmpty()) {
        item->setText(0, DisplayName(path));
        item->setIcon(0, RedLockIcon());
        item->setForeground(0, QBrush(QColor(190, 45, 45)));
        item->setToolTip(0, tr("文件占用锁：%1").arg(occupied_by));
    } else {
        item->setText(0, DisplayName(path));
        item->setIcon(0, style()->standardIcon(
            IsDirectory(item) ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        item->setForeground(0, palette().brush(QPalette::Text));
        item->setToolTip(0, {});
    }
}

void MainWindow::reloadParent(const QString& path) {
    const int separator = path.lastIndexOf('/');
    const QString parent_path = separator <= 0 ? "/" : path.left(separator);
    auto* parent = nodes_.value(parent_path, nullptr);
    if (parent != nullptr) {
        parent->setData(0, LoadedRole, false);
        loadChildren(parent);
    } else {
        refreshTree();
    }
}

QTreeWidgetItem* MainWindow::selectedItem() const {
    return tree_->currentItem();
}

void MainWindow::showReplyError(QNetworkReply* reply,
                                const QString& operation) {
    const QByteArray body = reply->readAll();
    QString message;
    const auto json = QJsonDocument::fromJson(body);
    if (json.isObject()) {
        message = json.object().value("error").toString();
    }
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (message.isEmpty()) {
        message = reply->errorString();
    }
    if (status != 0) message += tr("（HTTP %1）").arg(status);
    status_label_->setText(operation + tr("失败"));
    QMessageBox::warning(this, operation + tr("失败"), message);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!token_.isEmpty()) {
        if (!open_documents_.isEmpty() &&
            QMessageBox::question(
                this, tr("退出"),
                tr("仍有图纸在本地打开。请先保存图纸并上传。"
                   "退出将释放占用锁并清理本地临时文件。\n确定退出吗？"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No) != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        heartbeat_timer_->stop();
        notifications_timer_->stop();
        message_timer_->stop();
        QList<QNetworkReply*> replies;
        for (auto it = open_documents_.cbegin();
             it != open_documents_.cend(); ++it) {
            if (!it->lock_held) continue;
            const QByteArray body = QJsonDocument(
                QJsonObject{{"path", it.key()}})
                                        .toJson(QJsonDocument::Compact);
            auto request = makeRequest(apiUrl("/api/file/close"));
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              "application/json");
            replies.push_back(network_->post(request, body));
        }
        if (!replies.isEmpty()) {
            QEventLoop wait_for_release;
            QTimer timeout;
            int pending = replies.size();
            int failed = 0;
            for (auto* reply : replies) {
                connect(reply, &QNetworkReply::finished,
                        &wait_for_release,
                        [reply, &pending, &failed, &wait_for_release] {
                            if (reply->error() != QNetworkReply::NoError) {
                                ++failed;
                            }
                            --pending;
                            if (pending == 0) wait_for_release.quit();
                        });
            }
            connect(&timeout, &QTimer::timeout,
                    &wait_for_release, &QEventLoop::quit);
            timeout.start(1500);
            wait_for_release.exec();
            for (auto* reply : replies) reply->deleteLater();
            if (pending > 0 || failed > 0) {
                QMessageBox::warning(
                    this, tr("释放文件占用锁"),
                    tr("退出前未能确认所有占用锁均已释放。"
                       "未释放的锁将在服务器超时后自动清除。"));
            }
        }
        auto logout_request = makeRequest(apiUrl("/api/logout"));
        logout_request.setHeader(QNetworkRequest::ContentTypeHeader,
                                 "application/json");
        auto* logout_reply = network_->post(logout_request, QByteArray{});
        QEventLoop wait_for_logout;
        QTimer logout_timeout;
        bool logout_failed = false;
        bool logout_completed = false;
        connect(logout_reply, &QNetworkReply::finished, &wait_for_logout,
                [&] {
                    logout_completed = true;
                    logout_failed =
                        logout_reply->error() != QNetworkReply::NoError;
                    wait_for_logout.quit();
                });
        connect(&logout_timeout, &QTimer::timeout,
                &wait_for_logout, &QEventLoop::quit);
        logout_timeout.start(1500);
        wait_for_logout.exec();
        logout_reply->deleteLater();
        if (logout_failed || !logout_completed) {
            QMessageBox::warning(this, tr("注销"),
                                 tr("服务器未确认注销；未释放的占用锁将在超时后清除。"));
        }
    }
    event->accept();
    QMainWindow::closeEvent(event);
}
