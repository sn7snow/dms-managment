// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

void MainWindow::refreshTree() {
    if (token_.isEmpty()) return;
    children_requests_.clear();
    tree_->clear();
    nodes_.clear();
    auto* root = new QTreeWidgetItem(tree_);
    root->setText(0, "/");
    root->setData(0, PathRole, "/");
    root->setData(0, TypeRole, "dir");
    root->setData(0, PermissionsRole, QString(PermissionCount, '0'));
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
                        current_parent->setData(
                            0, PermissionsRole, QString(PermissionCount, '0'));
                        showReplyError(reply, tr("读取目录"));
                        status_label_->setText(tr("目录读取失败"));
                        return;
                    }
                    const auto document = QJsonDocument::fromJson(bytes);
                    if (!document.isObject() ||
                        !document.object().value("nodes").isArray()) {
                        current_parent->setData(0, LoadedRole, false);
                        current_parent->setData(
                            0, PermissionsRole, QString(PermissionCount, '0'));
                        status_label_->setText(tr("服务器响应格式错误"));
                        QMessageBox::warning(this, tr("读取目录失败"),
                                             tr("服务器没有返回有效的 nodes 列表。"));
                        return;
                    }
                    current_parent->setData(
                        0, PermissionsRole,
                        NormalizePermissionBits(
                            document.object().value("directory_perms").toString()));
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
    const QString permissions =
        NormalizePermissionBits(node.value("perms").toString());
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
