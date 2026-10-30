// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

#include <QProgressDialog>
#include <QSaveFile>

#include <algorithm>

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
                             ReplyHandler handler,
                             const QList<QPair<QByteArray, QByteArray>>& headers) {
    auto request = makeRequest(url);
    if (!content_type.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QString::fromLatin1(content_type));
    } else if (method == "POST") {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          "application/json");
    }
    for (const auto& [name, value] : headers) {
        request.setRawHeader(name, value);
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

void MainWindow::sendFileRequest(
    const QByteArray& method, const QUrl& url, const QString& source_path,
    const QByteArray& content_type, const QString& operation,
    ReplyHandler handler,
    const QList<QPair<QByteArray, QByteArray>>& headers) {
    auto file = std::make_shared<QFile>(source_path);
    if (!file->open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, operation,
                             tr("无法读取本地文件：%1").arg(file->errorString()));
        return;
    }
    constexpr qint64 max_upload_bytes = 4LL * 1024 * 1024 * 1024;
    if (file->size() > max_upload_bytes) {
        QMessageBox::warning(this, operation,
                             tr("文件超过 4 GiB 的上传上限。"));
        return;
    }

    auto request = makeRequest(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QString::fromLatin1(content_type));
    for (const auto& [name, value] : headers) {
        request.setRawHeader(name, value);
    }
    QNetworkReply* reply = nullptr;
    if (method == "POST") {
        reply = network_->post(request, file.get());
    } else if (method == "PUT") {
        reply = network_->put(request, file.get());
    } else {
        QMessageBox::warning(this, operation,
                             tr("不支持此文件上传请求类型。"));
        return;
    }

    auto* progress = new QProgressDialog(
        tr("%1：0 B / %2").arg(operation, FormatFileSize(file->size())),
        tr("取消"), 0, 1000, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    connect(reply, &QNetworkReply::uploadProgress, this,
            [progress, operation](qint64 sent, qint64 total) {
                if (total <= 0) return;
                const auto percent = static_cast<int>(
                    (static_cast<long double>(sent) * 1000.0L) /
                    static_cast<long double>(total));
                progress->setLabelText(
                    tr("%1：%2 / %3")
                        .arg(operation, FormatFileSize(sent),
                             FormatFileSize(total)));
                progress->setValue(std::clamp(percent, 0, 1000));
            });
    connect(progress, &QProgressDialog::canceled,
            reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, progress,
             handler = std::move(handler)]() mutable {
                (void)file;
                progress->close();
                progress->deleteLater();
                if (reply->error() ==
                    QNetworkReply::OperationCanceledError) {
                    status_label_->setText(tr("文件传输已取消"));
                } else {
                    handler(reply);
                }
                reply->deleteLater();
            });
}

void MainWindow::downloadToFile(const QUrl& url, const QString& destination,
                                const QString& operation,
                                TransferHandler handler) {
    auto output = std::make_shared<QSaveFile>(destination);
    if (!output->open(QIODevice::WriteOnly)) {
        QMessageBox::critical(
            this, operation,
            tr("无法创建本地文件：%1").arg(output->errorString()));
        handler(false);
        return;
    }

    QNetworkReply* reply = network_->get(makeRequest(url));
    auto* progress = new QProgressDialog(
        tr("%1：等待服务器响应").arg(operation), tr("取消"), 0, 1000, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    auto write_failed = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, output, write_failed] {
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status >= 400) return;
                const auto bytes = reply->readAll();
                if (output->write(bytes) != bytes.size()) {
                    *write_failed = true;
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [progress, operation](qint64 received, qint64 total) {
                if (total <= 0) {
                    progress->setRange(0, 0);
                    progress->setLabelText(
                        tr("%1：已接收 %2").arg(operation,
                                                FormatFileSize(received)));
                    return;
                }
                progress->setRange(0, 1000);
                const auto percent = static_cast<int>(
                    (static_cast<long double>(received) * 1000.0L) /
                    static_cast<long double>(total));
                progress->setLabelText(
                    tr("%1：%2 / %3")
                        .arg(operation, FormatFileSize(received),
                             FormatFileSize(total)));
                progress->setValue(std::clamp(percent, 0, 1000));
            });
    connect(progress, &QProgressDialog::canceled,
            reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, output, progress, write_failed,
             destination, operation, handler = std::move(handler)]() mutable {
                progress->close();
                progress->deleteLater();
                const bool cancelled =
                    reply->error() == QNetworkReply::OperationCanceledError;
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                bool success = false;
                if (*write_failed) {
                    output->cancelWriting();
                    QMessageBox::critical(
                        this, operation,
                        tr("写入本地文件失败：%1")
                            .arg(output->errorString()));
                } else if (cancelled) {
                    output->cancelWriting();
                    status_label_->setText(tr("文件传输已取消"));
                } else if (reply->error() != QNetworkReply::NoError ||
                           status >= 400) {
                    output->cancelWriting();
                    showReplyError(reply, operation);
                } else {
                    const auto remaining = reply->readAll();
                    if (!remaining.isEmpty() &&
                        output->write(remaining) != remaining.size()) {
                        *write_failed = true;
                    }
                    if (!*write_failed && output->commit()) {
                        status_label_->setText(
                            tr("%1完成：%2").arg(operation, destination));
                        success = true;
                    } else {
                        output->cancelWriting();
                        QMessageBox::critical(
                            this, operation,
                            tr("写入本地文件失败：%1")
                                .arg(output->errorString()));
                    }
                }
                handler(success);
                reply->deleteLater();
            });
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
    const int current_version = json.object().value("current_version").toInt();
    if (message == "VERSION_CONFLICT") {
        message = tr("文件已被他人修改（当前 rev_%1），请重新下载后再编辑。")
                      .arg(current_version);
    } else {
#ifdef QT_DEBUG
        if (message.isEmpty()) message = reply->errorString();
#else
        if (status == 400) {
            message = tr("请求参数无效（INVALID_REQUEST）。");
        } else if (status == 401) {
            message = tr("登录已失效，请重新登录。");
        } else if (status == 403) {
            message = tr("没有执行此操作的权限。");
        } else if (status == 404) {
            message = tr("文件或目录不存在，或当前不可见。");
        } else if (status == 409) {
            message = tr("文件状态冲突，请刷新后重试。");
        } else if (status == 413) {
            message = tr("文件超过服务器允许的大小。");
        } else if (status == 415) {
            message = tr("文件类型不在允许的扩展名白名单中。");
        } else {
            message = tr("请求失败，请检查网络或联系管理员。");
        }
#endif
    }
    if (status != 0) message += tr("（HTTP %1）").arg(status);
    status_label_->setText(operation + tr("失败"));
    QMessageBox::warning(this, operation + tr("失败"), message);
}
