#include "DownloadManager.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QSslError>
#include <QEventLoop>
#include <QDebug>

namespace spw {

DownloadManager::DownloadManager(QObject* parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
    , m_speedTimer(new QElapsedTimer)
{
}

DownloadManager::~DownloadManager()
{
    cancelDownload();
    delete m_speedTimer;
}

void DownloadManager::startDownload(const QUrl& url, const QString& outputPath)
{
    if (m_downloading) {
        emit downloadError(QStringLiteral("A download is already in progress."));
        return;
    }

    m_outputPath = outputPath;
    m_resumeOffset = 0;
    m_bytesReceivedSinceLastSpeed = 0;
    m_downloading = true;

    // Check if we can resume a partial download
    QFileInfo fileInfo(outputPath);
    QIODevice::OpenMode openMode = QIODevice::WriteOnly;

    if (fileInfo.exists() && fileInfo.size() > 0) {
        m_resumeOffset = fileInfo.size();
        openMode = QIODevice::Append;
    }

    m_file = new QFile(outputPath, this);
    if (!m_file->open(openMode)) {
        emit downloadError(QStringLiteral("Failed to open file for writing: %1").arg(m_file->errorString()));
        cleanup();
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    // Set range header for resume
    if (m_resumeOffset > 0) {
        QByteArray rangeHeader = "bytes=" + QByteArray::number(m_resumeOffset) + "-";
        request.setRawHeader("Range", rangeHeader);
        qDebug() << "[DownloadManager] Resuming download from byte" << m_resumeOffset;
    }

    m_reply = m_manager->get(request);

    connect(m_reply, &QNetworkReply::readyRead,
            this, &DownloadManager::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &DownloadManager::onDownloadProgress);
    connect(m_reply, &QNetworkReply::finished,
            this, &DownloadManager::onFinished);
    connect(m_reply, &QNetworkReply::sslErrors,
            this, [this](const QList<QSslError>& errors) {
                for (const QSslError& err : errors)
                    qWarning() << "[DownloadManager] SSL error (ignored):" << err.errorString();
                if (m_reply)
                    m_reply->ignoreSslErrors();
            });

    m_speedTimer->start();
    qDebug() << "[DownloadManager] Starting download:" << url.toString();
}

void DownloadManager::cancelDownload()
{
    if (!m_downloading)
        return;

    if (m_reply) {
        m_reply->abort();
    }

    qDebug() << "[DownloadManager] Download cancelled.";
    cleanup();
}

bool DownloadManager::isDownloading() const
{
    return m_downloading;
}

bool DownloadManager::supportsResume(const QUrl& url)
{
    QNetworkAccessManager tempManager;
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = tempManager.head(request);

    // Block until the HEAD request finishes
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    bool resumable = false;
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray acceptRanges = reply->rawHeader("Accept-Ranges");
        resumable = acceptRanges.toLower().contains("bytes");
    }

    reply->deleteLater();
    return resumable;
}

void DownloadManager::onReadyRead()
{
    if (!m_file || !m_reply)
        return;

    // Write received data directly to disk without buffering
    QByteArray data = m_reply->readAll();
    qint64 written = m_file->write(data);
    if (written == -1) {
        emit downloadError(QStringLiteral("Failed to write to disk: %1").arg(m_file->errorString()));
        cancelDownload();
        return;
    }

    m_bytesReceivedSinceLastSpeed += data.size();

    // Calculate speed every 500ms
    qint64 elapsed = m_speedTimer->elapsed();
    if (elapsed >= 500) {
        double seconds = elapsed / 1000.0;
        double bytesPerSec = m_bytesReceivedSinceLastSpeed / seconds;
        emit speedUpdate(bytesPerSec);

        m_bytesReceivedSinceLastSpeed = 0;
        m_speedTimer->restart();
    }
}

void DownloadManager::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    // Adjust for resume offset so the caller sees total file progress
    qint64 totalReceived = bytesReceived + m_resumeOffset;
    qint64 totalSize = (bytesTotal > 0) ? (bytesTotal + m_resumeOffset) : -1;
    emit progressChanged(totalReceived, totalSize);
}

void DownloadManager::onFinished()
{
    if (!m_reply)
        return;

    // Flush any remaining buffered data
    QByteArray remaining = m_reply->readAll();
    if (!remaining.isEmpty() && m_file) {
        m_file->write(remaining);
    }

    if (m_reply->error() == QNetworkReply::NoError) {
        QString path = m_outputPath;
        cleanup();
        qDebug() << "[DownloadManager] Download complete:" << path;
        emit downloadComplete(path);
    } else if (m_reply->error() == QNetworkReply::OperationCanceledError) {
        // Already handled by cancelDownload, just clean up
        cleanup();
    } else {
        QString errorMsg = m_reply->errorString();
        cleanup();
        qDebug() << "[DownloadManager] Download failed:" << errorMsg;
        emit downloadError(errorMsg);
    }
}

void DownloadManager::cleanup()
{
    m_downloading = false;

    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    if (m_file) {
        if (m_file->isOpen())
            m_file->close();
        m_file->deleteLater();
        m_file = nullptr;
    }

    m_resumeOffset = 0;
    m_bytesReceivedSinceLastSpeed = 0;
}

} // namespace spw
