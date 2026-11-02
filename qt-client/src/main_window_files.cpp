// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

bool MainWindow::isAllowedUploadExtension(const QString& path) const {
    return upload_policy_loaded_ &&
           allowed_upload_extensions_.contains(
               QFileInfo(path).suffix().toLower());
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
                                      const QString& locked_at,
                                      const int base_version) {
        const QString suffix = QFileInfo(path).suffix();
        const QByteArray digest = QCryptographicHash::hash(
            path.toUtf8(), QCryptographicHash::Sha256).toHex();
        const QString local_file = temporary_dir_.filePath(
            QString::fromLatin1(digest) +
            (suffix.isEmpty() ? QString{} : "." + suffix));
        downloadToFile(
            apiUrl("/api/file", {{"path", path}}), local_file,
            tr("下载图纸"),
            [this, path, item, lock_held, occupied_by, locked_at,
             base_version, launch, local_file](bool success) {
                if (!success) {
                    if (lock_held) releaseServerLock(path);
                    return;
                }
                QFile file(local_file);
                if (!file.setPermissions(QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner)) {
                    QFile::remove(local_file);
                    if (lock_held) releaseServerLock(path);
                    QMessageBox::critical(
                        this, tr("下载图纸"),
                        tr("无法限制本地临时文件的访问权限：%1")
                            .arg(local_file));
                    return;
                }
                open_documents_.insert(path,
                    OpenDocument{local_file, lock_held, lock_held,
                                 occupied_by, locked_at, base_version});
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
                save_button_->setEnabled(lock_held && selectedItem() == item);
            });
    };

    if (!writable) {
        download(false, tr("无写权限"), {}, 0);
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
                    const int base_version = response.value("version").toInt();
                    if (state == "editable") {
                        download(true, {}, locked_at, base_version);
                    } else if (state == "read_only") {
                        download(false,
                                 response.value("occupied_by").toString(
                                     tr("其他用户")),
                                 locked_at, base_version);
                    } else {
                        QMessageBox::warning(this, tr("打开图纸"),
                                             tr("服务器返回了未知的占用锁状态。"));
                    }
                });
}

void MainWindow::downloadCurrent(QTreeWidgetItem* item) {
    if (item == nullptr || IsDirectory(item) || token_.isEmpty() ||
        !HasPermission(item, ReadPermission)) {
        return;
    }
    const QString path = NodePath(item);
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("下载图纸"), DisplayName(path));
    if (destination.isEmpty()) return;
    downloadToFile(apiUrl("/api/file", {{"path", path}}), destination,
                   tr("下载图纸"), [](bool) {});
}

void MainWindow::downloadArchive(QTreeWidgetItem* item) {
    if (item == nullptr || !IsDirectory(item) || token_.isEmpty() ||
        !HasPermission(item, ReadPermission)) {
        return;
    }
    const QString path = NodePath(item);
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("下载文件夹为 tar"),
        DisplayName(path).isEmpty()
            ? QStringLiteral("files.tar")
            : DisplayName(path) + QStringLiteral(".tar"),
        tr("Tar 归档 (*.tar)"));
    if (destination.isEmpty()) return;
    downloadToFile(apiUrl("/api/archive", {{"path", path}}), destination,
                   tr("下载文件夹归档"), [](bool) {});
}

void MainWindow::uploadLocalFile(QTreeWidgetItem* item) {
    if (item == nullptr || token_.isEmpty()) return;
    if (!upload_policy_loaded_) {
        QMessageBox::information(
            this, tr("上传本地文件"),
            tr("正在读取服务器上传类型策略，请稍后重试。"));
        return;
    }
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
    if (!isAllowedUploadExtension(source)) {
        QMessageBox::warning(
            this, tr("上传本地文件"),
            tr("该文件扩展名不在服务器允许的白名单中。"));
        return;
    }

    const QString target_directory = IsDirectory(item)
        ? NodePath(item)
        : ParentPath(NodePath(item));
    const QString name = QFileInfo(source).fileName();
    const QString uploaded_path = target_directory == "/"
        ? "/" + name
        : target_directory + "/" + name;
    sendFileRequest(
                MethodName("POST"),
                apiUrl("/api/upload",
                       {{"path", target_directory}, {"name", name}}),
                source, "application/octet-stream", tr("上传本地文件"),
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
    for (const auto& file_path : job->files) {
        if (!isAllowedUploadExtension(file_path)) {
            QMessageBox::warning(
                this, tr("上传文件夹"),
                tr("文件夹中包含不在扩展名白名单内的文件：%1")
                    .arg(file_path));
            return;
        }
        if (QFileInfo(file_path).size() > 4LL * 1024 * 1024 * 1024) {
            QMessageBox::warning(
                this, tr("上传文件夹"),
                tr("文件夹中包含超过 4 GiB 上限的文件：%1")
                    .arg(file_path));
            return;
        }
    }

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
    const QFileInfo source_info(source_path);
    const QString file_name = source_info.fileName();
    const QString relative_parent =
        QDir(job->source_directory).relativeFilePath(source_info.absolutePath());
    const QString server_parent = relative_parent == "."
        ? job->target_directory
        : job->target_directory + "/" + relative_parent;

    sendFileRequest(MethodName("POST"),
                apiUrl("/api/upload",
                       {{"path", server_parent}, {"name", file_name}}),
                source_path, "application/octet-stream",
                tr("上传文件夹中的文件"),
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
    if (!upload_policy_loaded_) {
        QMessageBox::information(
            this, tr("保存当前编辑内容"),
            tr("正在读取服务器上传类型策略，请稍后重试。"));
        return;
    }
    const QString path = NodePath(item);
    if (!isAllowedUploadExtension(path)) {
        QMessageBox::warning(
            this, tr("保存当前编辑内容"),
            tr("该文件扩展名不在服务器允许的白名单中。"));
        return;
    }
    const auto document = open_documents_.constFind(path);
    if (document == open_documents_.cend() ||
        !document->editable || !document->lock_held) {
        QMessageBox::warning(this, tr("保存当前编辑内容"),
                             tr("此文件没有可用的编辑锁。请先打开并取得编辑权限。"));
        return;
    }
    const int base_version = document->base_version;
    sendFileRequest(MethodName("PUT"),
                apiUrl("/api/file", {{"path", path}}),
                document->local_file, "application/octet-stream",
                tr("保存当前编辑内容"),
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
                    auto open_document = open_documents_.find(path);
                    if (open_document != open_documents_.end()) {
                        open_document->base_version =
                            response.value("version").toInt();
                    }
                    reloadParent(path);
                },
                {{QByteArrayLiteral("If-Match"),
                  QByteArray("\"rev_") + QByteArray::number(base_version) +
                      "\""}});
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
    if (token_.isEmpty() || heartbeat_request_in_flight_) return;
    const auto paths = open_documents_.keys();
    QJsonArray requested_paths;
    for (const auto& path : paths) {
        const auto document = open_documents_.constFind(path);
        if (document == open_documents_.cend() || !document->lock_held) continue;
        requested_paths.append(path);
    }
    if (requested_paths.isEmpty()) return;

    heartbeat_request_in_flight_ = true;
    const QByteArray body =
        QJsonDocument(QJsonObject{{"paths", requested_paths}})
            .toJson(QJsonDocument::Compact);
    sendRequest(MethodName("POST"), apiUrl("/api/file/heartbeat"), body,
                "application/json",
                [this, paths](QNetworkReply* reply) {
                    heartbeat_request_in_flight_ = false;
                    const auto mark_expired = [this](const QString& path) {
                        auto document = open_documents_.find(path);
                        if (document == open_documents_.end() ||
                            !document->lock_held) {
                            return;
                        }
                        document->lock_held = false;
                        document->editable = false;
                        document->occupied_by = tr("占用锁已失效");
                        setItemLockState(path, tr("占用锁已失效"));
                        if (selectedItem() != nullptr &&
                            NodePath(selectedItem()) == path) {
                            save_button_->setEnabled(false);
                        }
                    };
                    if (reply->error() != QNetworkReply::NoError) {
                        for (const auto& path : paths) mark_expired(path);
                        showReplyError(reply, tr("续期文件占用锁"));
                        return;
                    }
                    const auto response = QJsonDocument::fromJson(
                        reply->readAll());
                    if (!response.isObject() ||
                        !response.object().value("ok").isArray() ||
                        !response.object().value("expired").isArray()) {
                        for (const auto& path : paths) mark_expired(path);
                        status_label_->setText(
                            tr("续期文件占用锁失败：服务器响应格式无效"));
                        return;
                    }
                    QStringList acknowledged;
                    for (const auto& value :
                         response.object().value("ok").toArray()) {
                        if (value.isString()) {
                            acknowledged.push_back(value.toString());
                        }
                    }
                    QStringList expired;
                    for (const auto& value :
                         response.object().value("expired").toArray()) {
                        if (value.isString()) {
                            expired.push_back(value.toString());
                        }
                    }
                    for (const auto& path : paths) {
                        if (expired.contains(path) ||
                            !acknowledged.contains(path)) {
                            mark_expired(path);
                        }
                    }
                    if (!expired.isEmpty()) {
                        status_label_->setText(
                            tr("%1 个文件的占用锁已失效")
                                .arg(expired.size()));
                    }
                });
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
