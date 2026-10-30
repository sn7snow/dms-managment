// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

void MainWindow::addManualVersion(QTreeWidgetItem* item) {
    if (item == nullptr || !HasPermission(item, AddVersionPermission)) return;
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
    if (item == nullptr || !HasPermission(item, ReadPermission)) return;
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
                        auto* current_item = nodes_.value(path, nullptr);
                        delete_action->setEnabled(
                            HasPermission(current_item, DeleteVersionPermission));
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
    if (!HasPermission(nodes_.value(path, nullptr), ReadPermission)) return;
    const QString destination = QFileDialog::getSaveFileName(
        this, tr("下载历史版本"),
        DisplayName(path) + QString(".rev_%1").arg(revision));
    if (destination.isEmpty()) return;
    downloadToFile(
        apiUrl("/api/file/version",
               {{"path", path}, {"rev", QString::number(revision)}}),
        destination, tr("下载历史版本"),
        [this, revision](bool success) {
            if (success) {
                status_label_->setText(
                    tr("已下载历史版本 rev_%1").arg(revision));
            }
        });
}

void MainWindow::deleteVersion(const QString& path, int revision) {
    if (!HasPermission(nodes_.value(path, nullptr),
                      DeleteVersionPermission)) {
        return;
    }
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
