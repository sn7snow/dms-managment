// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

void MainWindow::showContextMenu(const QPoint& position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr || token_.isEmpty()) return;
    tree_->setCurrentItem(item);
    const QString path = NodePath(item);
    const bool directory = IsDirectory(item);
    QMenu menu(this);
    if (directory) {
        auto* create_action = menu.addAction(tr("新建文件夹…"));
        auto* upload_action = menu.addAction(tr("上传本地文件到此目录…"));
        auto* archive_action = menu.addAction(tr("下载文件夹为 tar…"));
        create_action->setEnabled(HasPermission(item, WritePermission));
        upload_action->setEnabled(HasPermission(item, UploadPermission));
        archive_action->setEnabled(HasPermission(item, ReadPermission));
        QAction* rename_action = nullptr;
        QAction* delete_action = nullptr;
        if (path != "/") {
            rename_action = menu.addAction(tr("重命名/移动…"));
            delete_action = menu.addAction(tr("申请删除文件夹…"));
            rename_action->setEnabled(HasPermission(item, RenamePermission));
            delete_action->setEnabled(HasPermission(item, DeletePermission));
        }
        auto* refresh_action = menu.addAction(tr("刷新目录"));
        refresh_action->setEnabled(HasPermission(item, ReadPermission));
        const auto* chosen = menu.exec(tree_->viewport()->mapToGlobal(position));
        if (chosen == create_action) createDirectory(item);
        else if (chosen == upload_action) uploadLocalFile(item);
        else if (chosen == archive_action) downloadArchive(item);
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
    download_action->setEnabled(HasPermission(item, ReadPermission));
    upload_action->setEnabled(HasPermission(item, UploadPermission));
    add_version_action->setEnabled(HasPermission(item, AddVersionPermission));
    versions_action->setEnabled(HasPermission(item, ReadPermission));
    delete_action->setEnabled(HasPermission(item, DeletePermission));
    if (request_edit_action != nullptr) {
        request_edit_action->setEnabled(HasPermission(item, ReadPermission));
    }
    if (save_edit_action != nullptr) {
        const auto document = open_documents_.value(path);
        save_edit_action->setEnabled(
            HasPermission(item, WritePermission) &&
            document.editable && document.lock_held);
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
    if (item == nullptr || !HasPermission(item, WritePermission)) return;
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
    if (item == nullptr || !HasPermission(item, RenamePermission)) return;
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
    if (to == "/" || to.contains('\\') || to.contains("/../") || to.endsWith("/..") ||
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
    if (item == nullptr || !HasPermission(item, DeletePermission)) return;
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
