// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

#include <QRegularExpression>

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
    notifications_timer_->setInterval(3000);
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
                    allowed_upload_extensions_.clear();
                    upload_policy_loaded_ = false;
                    username_ = object.value("user").toString(attempted_user);
                    connect_button_->setText(tr("注销"));
                    connect_button_->setEnabled(true);
                    refresh_button_->setEnabled(true);
                    status_label_->setText(tr("已连接：%1").arg(username_));
                    refreshTree();
                    heartbeat_timer_->start();
                    notifications_timer_->start();
                    message_timer_->start();
                    const QString policy_token = token_;
                    sendRequest(
                        MethodName("GET"), apiUrl("/api/upload-policy"), {}, {},
                        [this, policy_token](QNetworkReply* policy_reply) {
                            const QByteArray policy_bytes =
                                policy_reply->readAll();
                            if (token_ != policy_token) return;
                            if (policy_reply->error() !=
                                QNetworkReply::NoError) {
                                showReplyError(policy_reply,
                                               tr("读取上传类型策略"));
                                status_label_->setText(
                                    tr("已连接，但未能读取上传类型策略"));
                                return;
                            }
                            const auto policy =
                                QJsonDocument::fromJson(policy_bytes);
                            const auto values = policy.object().value(
                                "allowed_extensions").toArray();
                            if (!policy.isObject() ||
                                !policy.object()
                                     .value("allowed_extensions").isArray()) {
                                QMessageBox::warning(
                                    this, tr("读取上传类型策略"),
                                    tr("服务器返回的上传类型策略格式无效。"));
                                return;
                            }
                            QStringList extensions;
                            static const QRegularExpression valid_extension(
                                QStringLiteral("^[a-z0-9]+$"));
                            for (const auto& value : values) {
                                if (!value.isString() ||
                                    !valid_extension.match(
                                        value.toString()).hasMatch()) {
                                    QMessageBox::warning(
                                        this, tr("读取上传类型策略"),
                                        tr("服务器返回的上传类型策略包含无效扩展名。"));
                                    return;
                                }
                                extensions.push_back(value.toString());
                            }
                            allowed_upload_extensions_ =
                                std::move(extensions);
                            upload_policy_loaded_ = true;
                        });
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
    allowed_upload_extensions_.clear();
    upload_policy_loaded_ = false;
    notifications_request_in_flight_ = false;
    heartbeat_request_in_flight_ = false;
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
    if (temporary_dir_.isValid() && !temporary_dir_.remove()) {
        QMessageBox::warning(
            this, tr("清理本地临时文件"),
            tr("无法完全清理客户端临时目录：%1")
                .arg(temporary_dir_.path()));
    }
    event->accept();
    QMainWindow::closeEvent(event);
}
