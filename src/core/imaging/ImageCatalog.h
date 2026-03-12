#pragma once

#include <QObject>
#include <QList>
#include <QUrl>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;

namespace spw {

struct ImageEntry {
    QString name;           // e.g. "Raspberry Pi OS (64-bit)"
    QString description;    // Short description
    QString category;       // "Raspberry Pi", "Ubuntu", "Debian", "Fedora", "Kali", "DietPi", "Other"
    QString version;        // e.g. "2024-11-15"
    QUrl downloadUrl;       // Direct download URL
    QString sha256;         // Expected hash (empty if unknown)
    qint64 downloadSize;    // Approximate download size in bytes (0 if unknown)
    qint64 extractedSize;   // Approximate extracted size (0 if unknown)
    bool isCompressed;      // Whether the download needs decompression
    QString compressedExt;  // ".xz", ".gz", ".zip", ".7z"
};

class ImageCatalog : public QObject
{
    Q_OBJECT

public:
    explicit ImageCatalog(QObject* parent = nullptr);

    QList<ImageEntry> builtinImages() const;
    QList<ImageEntry> allImages() const;
    QStringList categories() const;
    QList<ImageEntry> imagesByCategory(const QString& category) const;

    void fetchRemoteCatalog();

signals:
    void catalogUpdated();
    void fetchError(const QString& error);

private:
    void parseRpiImagerJson(const QByteArray& data);

    QNetworkAccessManager* m_networkManager = nullptr;
    QList<ImageEntry> m_remoteImages;
};

} // namespace spw
