#include "ImageCatalog.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

namespace spw {

ImageCatalog::ImageCatalog(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

QList<ImageEntry> ImageCatalog::builtinImages() const
{
    QList<ImageEntry> images;

    // --- Raspberry Pi ---

    images.append({
        QStringLiteral("Raspberry Pi OS Lite (64-bit)"),
        QStringLiteral("A port of Debian Bookworm with no desktop environment, 64-bit kernel and userspace"),
        QStringLiteral("Raspberry Pi"),
        QStringLiteral("2024-11-19"),
        QUrl(QStringLiteral("https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2024-11-19/2024-11-19-raspios-bookworm-arm64-lite.img.xz")),
        QStringLiteral("3d844d09a803524b15a3c1b06ee7882d6f8e57a95b7c1bc32773edf44b0b0e7f"),
        static_cast<qint64>(503'316'480),   // ~480 MB compressed
        static_cast<qint64>(2'684'354'560),  // ~2.5 GB extracted
        true,
        QStringLiteral(".xz")
    });

    images.append({
        QStringLiteral("Raspberry Pi OS Lite (32-bit)"),
        QStringLiteral("A port of Debian Bookworm with no desktop environment, 32-bit kernel and userspace"),
        QStringLiteral("Raspberry Pi"),
        QStringLiteral("2024-11-19"),
        QUrl(QStringLiteral("https://downloads.raspberrypi.com/raspios_lite_armhf/images/raspios_lite_armhf-2024-11-19/2024-11-19-raspios-bookworm-armhf-lite.img.xz")),
        QStringLiteral(""),
        static_cast<qint64>(471'859'200),   // ~450 MB compressed
        static_cast<qint64>(2'147'483'648),  // ~2 GB extracted
        true,
        QStringLiteral(".xz")
    });

    // --- Ubuntu ---

    images.append({
        QStringLiteral("Ubuntu Server 24.04 LTS (RPi)"),
        QStringLiteral("Ubuntu Server 24.04 LTS Noble Numbat for Raspberry Pi (arm64)"),
        QStringLiteral("Ubuntu"),
        QStringLiteral("24.04.1"),
        QUrl(QStringLiteral("https://cdimage.ubuntu.com/releases/24.04.1/release/ubuntu-24.04.1-preinstalled-server-arm64+raspi.img.xz")),
        QStringLiteral(""),
        static_cast<qint64>(1'153'433'600),  // ~1.1 GB compressed
        static_cast<qint64>(3'758'096'384),  // ~3.5 GB extracted
        true,
        QStringLiteral(".xz")
    });

    // --- Debian ---

    images.append({
        QStringLiteral("Debian 12 Netinst (amd64)"),
        QStringLiteral("Debian 12 Bookworm network installer for 64-bit PC (amd64)"),
        QStringLiteral("Debian"),
        QStringLiteral("12.8.0"),
        QUrl(QStringLiteral("https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-12.8.0-amd64-netinst.iso")),
        QStringLiteral(""),
        static_cast<qint64>(659'554'304),    // ~629 MB
        static_cast<qint64>(659'554'304),    // ISO, no decompression
        false,
        QString()
    });

    // --- Fedora ---

    images.append({
        QStringLiteral("Fedora Server 40 (aarch64)"),
        QStringLiteral("Fedora Server 40 raw disk image for ARM 64-bit systems"),
        QStringLiteral("Fedora"),
        QStringLiteral("40-1.14"),
        QUrl(QStringLiteral("https://download.fedoraproject.org/pub/fedora/linux/releases/40/Server/aarch64/images/Fedora-Server-40-1.14.aarch64.raw.xz")),
        QStringLiteral(""),
        static_cast<qint64>(838'860'800),    // ~800 MB compressed
        static_cast<qint64>(7'516'192'768),  // ~7 GB extracted
        true,
        QStringLiteral(".xz")
    });

    // --- Kali Linux ---

    images.append({
        QStringLiteral("Kali Linux Installer (amd64)"),
        QStringLiteral("Kali Linux full installer ISO for 64-bit PC"),
        QStringLiteral("Kali"),
        QStringLiteral("2024.3"),
        QUrl(QStringLiteral("https://cdimage.kali.org/kali-2024.3/kali-linux-2024.3-installer-amd64.iso")),
        QStringLiteral(""),
        static_cast<qint64>(4'089'446'400),  // ~3.8 GB
        static_cast<qint64>(4'089'446'400),  // ISO, no decompression
        false,
        QString()
    });

    images.append({
        QStringLiteral("Kali Linux (RPi ARM64)"),
        QStringLiteral("Kali Linux image for Raspberry Pi (64-bit ARM)"),
        QStringLiteral("Kali"),
        QStringLiteral("2024.3"),
        QUrl(QStringLiteral("https://kali.download/arm-images/kali-2024.3/kali-linux-2024.3-raspberry-pi-arm64.img.xz")),
        QStringLiteral(""),
        static_cast<qint64>(1'610'612'736),  // ~1.5 GB compressed
        static_cast<qint64>(7'516'192'768),  // ~7 GB extracted
        true,
        QStringLiteral(".xz")
    });

    // --- DietPi ---

    images.append({
        QStringLiteral("DietPi (RPi ARMv8 64-bit)"),
        QStringLiteral("Highly optimized minimal Debian-based OS for Raspberry Pi 2/3/4/5 (64-bit)"),
        QStringLiteral("DietPi"),
        QStringLiteral("9.8"),
        QUrl(QStringLiteral("https://dietpi.com/downloads/images/DietPi_RPi-ARMv8-Bookworm.img.xz")),
        QStringLiteral(""),
        static_cast<qint64>(209'715'200),    // ~200 MB compressed
        static_cast<qint64>(1'073'741'824),  // ~1 GB extracted
        true,
        QStringLiteral(".xz")
    });

    // --- Alpine Linux ---

    images.append({
        QStringLiteral("Alpine Linux Extended (RPi aarch64)"),
        QStringLiteral("Alpine Linux extended image for Raspberry Pi (aarch64), includes common packages"),
        QStringLiteral("Other"),
        QStringLiteral("3.20.3"),
        QUrl(QStringLiteral("https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-rpi-3.20.3-aarch64.img.gz")),
        QStringLiteral(""),
        static_cast<qint64>(209'715'200),    // ~200 MB compressed
        static_cast<qint64>(524'288'000),    // ~500 MB extracted
        true,
        QStringLiteral(".gz")
    });

    // --- Arch Linux ARM ---

    images.append({
        QStringLiteral("Arch Linux ARM (RPi 4/5 aarch64)"),
        QStringLiteral("Arch Linux ARM root filesystem tarball for Raspberry Pi 4 and 5"),
        QStringLiteral("Other"),
        QStringLiteral("latest"),
        QUrl(QStringLiteral("http://os.archlinuxarm.org/os/ArchLinuxARM-rpi-aarch64-latest.tar.gz")),
        QStringLiteral(""),
        static_cast<qint64>(419'430'400),    // ~400 MB compressed
        static_cast<qint64>(2'147'483'648),  // ~2 GB extracted
        true,
        QStringLiteral(".gz")
    });

    return images;
}

QList<ImageEntry> ImageCatalog::allImages() const
{
    QList<ImageEntry> all = builtinImages();
    all.append(m_remoteImages);
    return all;
}

QStringList ImageCatalog::categories() const
{
    QSet<QString> cats;
    const auto all = allImages();
    for (const auto& img : all) {
        cats.insert(img.category);
    }
    QStringList sorted = cats.values();
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}

QList<ImageEntry> ImageCatalog::imagesByCategory(const QString& category) const
{
    QList<ImageEntry> result;
    const auto all = allImages();
    for (const auto& img : all) {
        if (img.category.compare(category, Qt::CaseInsensitive) == 0) {
            result.append(img);
        }
    }
    return result;
}

void ImageCatalog::fetchRemoteCatalog()
{
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://downloads.raspberrypi.com/os_list_imagingutility_v4.json")));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SetecPartitionWizard/1.0"));

    QNetworkReply* reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit fetchError(QStringLiteral("Failed to fetch remote catalog: %1")
                                .arg(reply->errorString()));
            return;
        }

        const QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            emit fetchError(QStringLiteral("Remote catalog response was empty"));
            return;
        }

        parseRpiImagerJson(data);
        emit catalogUpdated();
    });
}

void ImageCatalog::parseRpiImagerJson(const QByteArray& data)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull()) {
        emit fetchError(QStringLiteral("JSON parse error: %1 at offset %2")
                            .arg(parseError.errorString())
                            .arg(parseError.offset));
        return;
    }

    m_remoteImages.clear();

    const QJsonObject root = doc.object();
    const QJsonArray osList = root.value(QStringLiteral("os_list")).toArray();

    // Recursively process the os_list; entries can contain nested "subitems"
    std::function<void(const QJsonArray&, const QString&)> processArray;
    processArray = [&](const QJsonArray& array, const QString& parentCategory) {
        for (const QJsonValue& val : array) {
            if (!val.isObject())
                continue;

            const QJsonObject obj = val.toObject();
            const QString name = obj.value(QStringLiteral("name")).toString().trimmed();

            // If this entry has subitems, recurse into them using this entry's
            // name as the category hint
            const QJsonArray subitems = obj.value(QStringLiteral("subitems")).toArray();
            if (!subitems.isEmpty()) {
                processArray(subitems, name);
                continue;
            }

            // Skip entries without a download URL
            const QString url = obj.value(QStringLiteral("url")).toString().trimmed();
            if (url.isEmpty())
                continue;

            // Determine category from parent or name
            QString category = parentCategory;
            if (category.isEmpty()) {
                if (name.contains(QStringLiteral("Raspberry Pi"), Qt::CaseInsensitive) ||
                    name.contains(QStringLiteral("Raspbian"), Qt::CaseInsensitive)) {
                    category = QStringLiteral("Raspberry Pi");
                } else if (name.contains(QStringLiteral("Ubuntu"), Qt::CaseInsensitive)) {
                    category = QStringLiteral("Ubuntu");
                } else if (name.contains(QStringLiteral("Debian"), Qt::CaseInsensitive)) {
                    category = QStringLiteral("Debian");
                } else if (name.contains(QStringLiteral("Fedora"), Qt::CaseInsensitive)) {
                    category = QStringLiteral("Fedora");
                } else if (name.contains(QStringLiteral("Kali"), Qt::CaseInsensitive)) {
                    category = QStringLiteral("Kali");
                } else {
                    category = QStringLiteral("Other");
                }
            }

            // Determine compression from URL
            bool isCompressed = false;
            QString compressedExt;
            if (url.endsWith(QStringLiteral(".xz"))) {
                isCompressed = true;
                compressedExt = QStringLiteral(".xz");
            } else if (url.endsWith(QStringLiteral(".gz"))) {
                isCompressed = true;
                compressedExt = QStringLiteral(".gz");
            } else if (url.endsWith(QStringLiteral(".zip"))) {
                isCompressed = true;
                compressedExt = QStringLiteral(".zip");
            } else if (url.endsWith(QStringLiteral(".7z"))) {
                isCompressed = true;
                compressedExt = QStringLiteral(".7z");
            }

            ImageEntry entry;
            entry.name = name;
            entry.description = obj.value(QStringLiteral("description")).toString().trimmed();
            entry.category = category;
            entry.version = obj.value(QStringLiteral("release_date")).toString().trimmed();
            entry.downloadUrl = QUrl(url);
            entry.sha256 = obj.value(QStringLiteral("extract_sha256")).toString().trimmed();
            entry.downloadSize = static_cast<qint64>(
                obj.value(QStringLiteral("image_download_size")).toDouble(0.0));
            entry.extractedSize = static_cast<qint64>(
                obj.value(QStringLiteral("extract_size")).toDouble(0.0));
            entry.isCompressed = isCompressed;
            entry.compressedExt = compressedExt;

            // Skip duplicate entries already in the builtin list
            bool duplicate = false;
            const auto builtins = builtinImages();
            for (const auto& b : builtins) {
                if (b.downloadUrl == entry.downloadUrl) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                m_remoteImages.append(entry);
        }
    };

    processArray(osList, QString());
}

} // namespace spw
