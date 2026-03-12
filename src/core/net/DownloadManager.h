#pragma once

#include <QObject>
#include <QUrl>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;
class QElapsedTimer;
class QSslError;

namespace spw {

class DownloadManager : public QObject
{
    Q_OBJECT

public:
    explicit DownloadManager(QObject* parent = nullptr);
    ~DownloadManager() override;

    void startDownload(const QUrl& url, const QString& outputPath);
    void cancelDownload();
    bool isDownloading() const;

    static bool supportsResume(const QUrl& url);

signals:
    void progressChanged(qint64 bytesReceived, qint64 bytesTotal);
    void downloadComplete(const QString& filePath);
    void downloadError(const QString& error);
    void speedUpdate(double bytesPerSec);

private slots:
    void onReadyRead();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onFinished();

private:
    void cleanup();

    QNetworkAccessManager* m_manager = nullptr;
    QNetworkReply* m_reply = nullptr;
    QFile* m_file = nullptr;
    QElapsedTimer* m_speedTimer = nullptr;

    QString m_outputPath;
    qint64 m_resumeOffset = 0;
    qint64 m_bytesReceivedSinceLastSpeed = 0;
    bool m_downloading = false;
};

} // namespace spw
