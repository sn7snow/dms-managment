// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window_internal.h"

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
