#pragma once

#include <QHash>
#include <QByteArray>
#include <QList>
#include <QMainWindow>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include <functional>
#include <memory>

class QCloseEvent;
class QLineEdit;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QJsonObject;
class QPlainTextEdit;
class QPushButton;
class QPoint;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct OpenDocument {
        QString local_file;
        bool editable = false;
        bool lock_held = false;
        QString occupied_by;
        QString locked_at;
    };

    struct FolderUploadJob {
        QString source_directory;
        QString target_directory;
        QStringList directories;
        QStringList files;
        qsizetype next_directory = 0;
        qsizetype next_file = 0;
    };

    using ReplyHandler = std::function<void(QNetworkReply*)>;

    void connectToServer();
    void logout();
    void refreshTree();
    void loadChildren(QTreeWidgetItem* parent);
    void addNode(QTreeWidgetItem* parent, const QJsonObject& node);
    void openFile(QTreeWidgetItem* item, const QString& application = {});
    void downloadCurrent(QTreeWidgetItem* item);
    void uploadLocalFile(QTreeWidgetItem* item);
    void uploadFolder(QTreeWidgetItem* item, const QString& source_directory);
    void uploadNextFolderDirectory(
        const std::shared_ptr<FolderUploadJob>& job);
    void uploadNextFolderFile(const std::shared_ptr<FolderUploadJob>& job);
    void uploadCurrent(QTreeWidgetItem* item);
    void requestEdit(QTreeWidgetItem* item);
    void closeDocument(const QString& path);
    void releaseServerLock(const QString& path);
    void sendHeartbeats();
    void pollNotifications();
    void showNextNotification();
    void showContextMenu(const QPoint& position);
    void createDirectory(QTreeWidgetItem* item);
    void renameNode(QTreeWidgetItem* item);
    void deleteNode(QTreeWidgetItem* item);
    void addManualVersion(QTreeWidgetItem* item);
    void showVersions(QTreeWidgetItem* item);
    void downloadVersion(const QString& path, int revision);
    void deleteVersion(const QString& path, int revision);

    QUrl apiUrl(const QString& route,
                const QList<QPair<QString, QString>>& query = {}) const;
    QNetworkRequest makeRequest(const QUrl& url) const;
    void sendRequest(const QByteArray& method, const QUrl& url,
                     const QByteArray& body, const QByteArray& content_type,
                     ReplyHandler handler);
    void showReplyError(QNetworkReply* reply, const QString& operation);
    QTreeWidgetItem* selectedItem() const;
    void setItemLockState(const QString& path, const QString& occupied_by,
                          const QString& locked_at = {});
    void reloadParent(const QString& path);

    QNetworkAccessManager* network_;
    QLineEdit* endpoint_edit_;
    QPushButton* connect_button_;
    QPushButton* refresh_button_;
    QPushButton* open_button_;
    QPushButton* save_button_;
    QLabel* status_label_;
    QPlainTextEdit* message_view_;
    QTreeWidget* tree_;
    QTimer* heartbeat_timer_;
    QTimer* notifications_timer_;
    QTimer* message_timer_;
    QTemporaryDir temporary_dir_;
    QUrl base_url_;
    QString token_;
    QString username_;
    QQueue<QString> notification_queue_;
    QStringList displayed_notifications_;
    bool notifications_request_in_flight_ = false;
    quint64 next_children_request_id_ = 0;
    QHash<QString, quint64> children_requests_;
    QHash<QString, OpenDocument> open_documents_;
    QHash<QString, QTreeWidgetItem*> nodes_;
};
