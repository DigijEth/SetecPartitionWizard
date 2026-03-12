#include "LinuxFlasherTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/common/Types.h"
#include "core/imaging/ImageCatalog.h"
#include "core/imaging/Decompressor.h"
#include "core/imaging/SevenZipExtractor.h"
#include "core/imaging/VirtualDisk.h"
#include "core/net/DownloadManager.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

LinuxFlasherTab::LinuxFlasherTab(QWidget* parent)
    : QWidget(parent)
{
    m_catalog = new ImageCatalog(this);
    m_downloader = new DownloadManager(this);

    setupUi();

    // Connect catalog signals
    connect(m_catalog, &ImageCatalog::catalogUpdated, this, &LinuxFlasherTab::onCatalogUpdated);
    connect(m_catalog, &ImageCatalog::fetchError, this, [this](const QString& err) {
        m_statusLabel->setText(tr("Catalog fetch failed: %1").arg(err));
        emit statusMessage(tr("Failed to refresh image catalog"));
    });

    // Connect downloader signals
    connect(m_downloader, &DownloadManager::progressChanged, this,
            [this](qint64 received, qint64 total) {
                if (total > 0)
                {
                    int pct = static_cast<int>((received * 100) / total);
                    m_progressBar->setValue(pct);
                    m_statusLabel->setText(tr("Downloading... %1 / %2")
                                               .arg(formatSize(static_cast<uint64_t>(received)))
                                               .arg(formatSize(static_cast<uint64_t>(total))));
                }
                else
                {
                    m_statusLabel->setText(tr("Downloading... %1")
                                               .arg(formatSize(static_cast<uint64_t>(received))));
                }
            });

    connect(m_downloader, &DownloadManager::speedUpdate, this,
            [this](double bytesPerSec) {
                double mbps = bytesPerSec / (1024.0 * 1024.0);
                m_speedLabel->setText(tr("%1 MB/s").arg(mbps, 0, 'f', 1));
            });

    connect(m_downloader, &DownloadManager::downloadError, this,
            [this](const QString& error) {
                m_statusLabel->setText(tr("Download failed: %1").arg(error));
                m_speedLabel->clear();
                setOperationRunning(false);
                emit statusMessage(tr("Download failed"));
            });

    // Populate built-in catalog
    onCatalogUpdated();
}

LinuxFlasherTab::~LinuxFlasherTab() = default;

void LinuxFlasherTab::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    // ===== 1. OS Selection Group =====
    auto* osGroup = new QGroupBox(tr("Select Linux Image"));
    auto* osLayout = new QGridLayout(osGroup);

    osLayout->addWidget(new QLabel(tr("Category:")), 0, 0);
    m_categoryCombo = new QComboBox();
    m_categoryCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LinuxFlasherTab::onCategoryChanged);
    osLayout->addWidget(m_categoryCombo, 0, 1, 1, 2);

    osLayout->addWidget(new QLabel(tr("Image:")), 1, 0);
    m_osCombo = new QComboBox();
    m_osCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_osCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LinuxFlasherTab::onOsChanged);
    osLayout->addWidget(m_osCombo, 1, 1, 1, 2);

    m_descriptionLabel = new QLabel(tr("Select a category and image above."));
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setStyleSheet("color: #6c7086; padding: 4px;");
    osLayout->addWidget(m_descriptionLabel, 2, 0, 1, 3);

    auto* refreshCatalogBtn = new QPushButton(tr("Refresh Catalog"));
    connect(refreshCatalogBtn, &QPushButton::clicked, this, [this]() {
        m_statusLabel->setText(tr("Fetching remote image catalog..."));
        m_catalog->fetchRemoteCatalog();
    });
    osLayout->addWidget(refreshCatalogBtn, 3, 2, Qt::AlignRight);

    mainLayout->addWidget(osGroup);

    // ===== 2. Custom Image Row =====
    auto* customGroup = new QGroupBox(tr("Or Use Custom Image"));
    auto* customLayout = new QHBoxLayout(customGroup);

    m_customImageEdit = new QLineEdit();
    m_customImageEdit->setPlaceholderText(tr("Path or URL to .img, .iso, .img.xz, .img.gz, .zip, .7z ..."));
    m_customImageEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    customLayout->addWidget(m_customImageEdit, 1);

    auto* browseBtn = new QPushButton(tr("Browse..."));
    connect(browseBtn, &QPushButton::clicked, this, &LinuxFlasherTab::onBrowseCustomImage);
    customLayout->addWidget(browseBtn);

    mainLayout->addWidget(customGroup);

    // ===== 3. Target Drive Group =====
    auto* targetGroup = new QGroupBox(tr("Target Drive"));
    auto* targetLayout = new QVBoxLayout(targetGroup);

    m_targetDriveCombo = new QComboBox();
    m_targetDriveCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    targetLayout->addWidget(m_targetDriveCombo);

    auto* warningLabel = new QLabel(tr("All data on the selected drive will be destroyed!"));
    warningLabel->setStyleSheet("color: #cc3333; font-weight: bold; padding: 2px 4px;");
    targetLayout->addWidget(warningLabel);

    mainLayout->addWidget(targetGroup);

    // ===== 4. Progress Area =====
    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    auto* statusRow = new QHBoxLayout();
    m_statusLabel = new QLabel();
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusRow->addWidget(m_statusLabel, 1);

    m_speedLabel = new QLabel();
    statusRow->addWidget(m_speedLabel);
    mainLayout->addLayout(statusRow);

    // ===== 5. Action Buttons =====
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();

    m_cancelBtn = new QPushButton(tr("Cancel"));
    m_cancelBtn->setVisible(false);
    connect(m_cancelBtn, &QPushButton::clicked, this, &LinuxFlasherTab::onCancel);
    btnRow->addWidget(m_cancelBtn);

    m_downloadOnlyBtn = new QPushButton(tr("Download Only"));
    connect(m_downloadOnlyBtn, &QPushButton::clicked, this, &LinuxFlasherTab::onDownloadOnly);
    btnRow->addWidget(m_downloadOnlyBtn);

    m_downloadFlashBtn = new QPushButton(tr("Download && Flash"));
    m_downloadFlashBtn->setObjectName("applyButton");
    connect(m_downloadFlashBtn, &QPushButton::clicked, this, &LinuxFlasherTab::onDownloadAndFlash);
    btnRow->addWidget(m_downloadFlashBtn);

    mainLayout->addLayout(btnRow);

    // Fill remaining space
    mainLayout->addStretch();
}

void LinuxFlasherTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateTargetDriveCombo();
}

void LinuxFlasherTab::populateTargetDriveCombo()
{
    m_targetDriveCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        if (!disk.isRemovable)
            continue;

        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));
        m_targetDriveCombo->addItem(label, disk.id);
    }

    if (m_targetDriveCombo->count() == 0)
        m_targetDriveCombo->addItem(tr("No removable drives detected"));
}

void LinuxFlasherTab::onCategoryChanged(int index)
{
    Q_UNUSED(index);
    m_osCombo->clear();

    QString category = m_categoryCombo->currentText();
    if (category.isEmpty())
        return;

    QList<ImageEntry> images = m_catalog->imagesByCategory(category);
    for (const auto& img : images)
    {
        QString label = img.name;
        if (!img.version.isEmpty())
            label += QString(" (%1)").arg(img.version);
        m_osCombo->addItem(label);
    }

    // Trigger description update
    if (m_osCombo->count() > 0)
        onOsChanged(0);
    else
        m_descriptionLabel->setText(tr("No images in this category."));
}

void LinuxFlasherTab::onOsChanged(int index)
{
    if (index < 0)
    {
        m_descriptionLabel->setText(tr("Select a category and image above."));
        return;
    }

    QString category = m_categoryCombo->currentText();
    QList<ImageEntry> images = m_catalog->imagesByCategory(category);
    if (index >= images.size())
        return;

    const ImageEntry& entry = images.at(index);

    QString desc = entry.description;
    if (entry.downloadSize > 0)
        desc += tr("\nDownload size: %1").arg(formatSize(static_cast<uint64_t>(entry.downloadSize)));
    if (entry.extractedSize > 0)
        desc += tr("  |  Extracted size: %1").arg(formatSize(static_cast<uint64_t>(entry.extractedSize)));
    if (entry.isCompressed)
        desc += tr("\nCompressed (%1)").arg(entry.compressedExt);
    if (!entry.sha256.isEmpty())
        desc += tr("\nSHA-256: %1").arg(entry.sha256.left(16) + "...");

    m_descriptionLabel->setText(desc);
}

void LinuxFlasherTab::onBrowseCustomImage()
{
    QString file = QFileDialog::getOpenFileName(
        this, tr("Select Linux Image"), QString(),
        tr("Disk Images (*.img *.iso *.img.xz *.img.gz *.zip *.7z);;All Files (*)"));
    if (!file.isEmpty())
        m_customImageEdit->setText(file);
}

void LinuxFlasherTab::onDownloadAndFlash()
{
    if (m_targetDriveCombo->currentData().isNull())
    {
        QMessageBox::warning(this, tr("No Target"),
                             tr("No removable drive selected. Insert a USB or SD card and refresh."));
        return;
    }

    int targetDiskId = m_targetDriveCombo->currentData().toInt();
    auto reply = QMessageBox::warning(
        this, tr("Flash Linux Image"),
        tr("ALL data on Disk %1 will be DESTROYED.\n\n"
           "The selected image will be downloaded (if needed), decompressed, and flashed.\n\n"
           "Continue?")
            .arg(targetDiskId),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    startPipeline(true);
}

void LinuxFlasherTab::onDownloadOnly()
{
    startPipeline(false);
}

void LinuxFlasherTab::onCancel()
{
    m_cancelled = true;
    if (m_downloader->isDownloading())
        m_downloader->cancelDownload();
    m_statusLabel->setText(tr("Cancelled."));
    m_speedLabel->clear();
    setOperationRunning(false);
    emit statusMessage(tr("Operation cancelled"));
}

void LinuxFlasherTab::onCatalogUpdated()
{
    m_categoryCombo->clear();
    QStringList categories = m_catalog->categories();
    m_categoryCombo->addItems(categories);

    if (!categories.isEmpty())
        onCategoryChanged(0);

    m_statusLabel->setText(tr("Image catalog updated (%1 categories).").arg(categories.size()));
    emit statusMessage(tr("Image catalog refreshed"));
}

void LinuxFlasherTab::startPipeline(bool flashAfter)
{
    m_cancelled = false;
    QString customPath = m_customImageEdit->text().trimmed();

    // Determine source: custom path/URL or catalog selection
    QUrl sourceUrl;
    QString localPath;
    ImageEntry selectedEntry;

    if (!customPath.isEmpty())
    {
        // Custom image — could be a URL or a local file
        if (customPath.startsWith("http://") || customPath.startsWith("https://"))
        {
            sourceUrl = QUrl(customPath);
        }
        else
        {
            localPath = customPath;
        }
    }
    else
    {
        // From catalog
        QString category = m_categoryCombo->currentText();
        QList<ImageEntry> images = m_catalog->imagesByCategory(category);
        int idx = m_osCombo->currentIndex();
        if (idx < 0 || idx >= images.size())
        {
            QMessageBox::warning(this, tr("No Image Selected"),
                                 tr("Please select an image from the catalog or specify a custom image path."));
            return;
        }
        selectedEntry = images.at(idx);
        sourceUrl = selectedEntry.downloadUrl;
    }

    setOperationRunning(true);

    if (!sourceUrl.isEmpty())
    {
        // Need to download first
        QString downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (downloadDir.isEmpty())
            downloadDir = QDir::tempPath();

        QString fileName = sourceUrl.fileName();
        if (fileName.isEmpty())
            fileName = "linux-image.img";
        QString outputPath = QDir(downloadDir).filePath(fileName);

        m_statusLabel->setText(tr("Downloading..."));
        m_progressBar->setValue(0);

        // Disconnect any previous downloadComplete connections to avoid stacking
        disconnect(m_downloader, &DownloadManager::downloadComplete, nullptr, nullptr);

        connect(m_downloader, &DownloadManager::downloadComplete, this,
                [this, flashAfter](const QString& filePath) {
                    m_speedLabel->clear();

                    if (m_cancelled)
                        return;

                    // Check if decompression is needed
                    if (Decompressor::isCompressed(filePath) ||
                        filePath.endsWith(".7z", Qt::CaseInsensitive))
                    {
                        decompressAndMaybeFlash(filePath, flashAfter);
                    }
                    else if (flashAfter)
                    {
                        flashImage(filePath);
                    }
                    else
                    {
                        m_statusLabel->setText(tr("Download complete: %1").arg(filePath));
                        setOperationRunning(false);
                        emit statusMessage(tr("Download complete"));
                    }
                });

        m_downloader->startDownload(sourceUrl, outputPath);
    }
    else if (!localPath.isEmpty())
    {
        // Local file — check if it needs decompression
        if (!QFileInfo::exists(localPath))
        {
            QMessageBox::warning(this, tr("File Not Found"),
                                 tr("The specified image file does not exist:\n%1").arg(localPath));
            setOperationRunning(false);
            return;
        }

        if (Decompressor::isCompressed(localPath) ||
            localPath.endsWith(".7z", Qt::CaseInsensitive))
        {
            decompressAndMaybeFlash(localPath, flashAfter);
        }
        else if (flashAfter)
        {
            flashImage(localPath);
        }
        else
        {
            m_statusLabel->setText(tr("Image is already a local file, no download needed: %1").arg(localPath));
            setOperationRunning(false);
        }
    }
    else
    {
        QMessageBox::warning(this, tr("No Image"),
                             tr("No image selected or specified."));
        setOperationRunning(false);
    }
}

void LinuxFlasherTab::decompressAndMaybeFlash(const QString& downloadedPath, bool flashAfter)
{
    m_statusLabel->setText(tr("Decompressing..."));
    m_progressBar->setValue(0);

    QString outputDir = QFileInfo(downloadedPath).absolutePath();

    if (downloadedPath.endsWith(".7z", Qt::CaseInsensitive))
    {
        // Use 7-Zip extractor (async via QProcess)
        if (!SevenZipExtractor::isAvailable())
        {
            m_statusLabel->setText(tr("7-Zip not found. Please install 7-Zip to decompress .7z files."));
            setOperationRunning(false);
            return;
        }

        auto* extractor = new SevenZipExtractor(this);

        connect(extractor, &SevenZipExtractor::progressChanged, this,
                [this](int percent) {
                    m_progressBar->setValue(percent);
                });

        connect(extractor, &SevenZipExtractor::extractionComplete, this,
                [this, flashAfter, extractor](const QString& outDir) {
                    extractor->deleteLater();

                    if (m_cancelled)
                        return;

                    // Find the extracted .img or .iso file
                    QDir dir(outDir);
                    QStringList imgFiles = dir.entryList({"*.img", "*.iso"}, QDir::Files, QDir::Size);
                    if (imgFiles.isEmpty())
                    {
                        m_statusLabel->setText(tr("Decompression complete but no .img/.iso file found in output."));
                        setOperationRunning(false);
                        return;
                    }

                    QString extractedPath = dir.filePath(imgFiles.first());
                    m_statusLabel->setText(tr("Decompressed: %1").arg(extractedPath));

                    if (flashAfter)
                        flashImage(extractedPath);
                    else
                    {
                        setOperationRunning(false);
                        emit statusMessage(tr("Decompression complete"));
                    }
                });

        connect(extractor, &SevenZipExtractor::extractionError, this,
                [this, extractor](const QString& error) {
                    extractor->deleteLater();
                    m_statusLabel->setText(tr("Decompression failed: %1").arg(error));
                    setOperationRunning(false);
                    emit statusMessage(tr("Decompression failed"));
                });

        extractor->extract(downloadedPath, outputDir);
    }
    else
    {
        // Use Decompressor (blocking — run on worker thread)
        auto* thread = QThread::create([this, downloadedPath, outputDir, flashAfter]() {
            auto result = Decompressor::decompressAuto(downloadedPath, outputDir,
                [this](qint64 done, qint64 total) {
                    if (m_cancelled)
                        return;
                    int pct = (total > 0) ? static_cast<int>((done * 100) / total) : 0;
                    QMetaObject::invokeMethod(m_progressBar, "setValue",
                                              Qt::QueuedConnection, Q_ARG(int, pct));
                    QMetaObject::invokeMethod(m_statusLabel, "setText",
                                              Qt::QueuedConnection,
                                              Q_ARG(QString, tr("Decompressing... %1 / %2")
                                                                 .arg(formatSize(static_cast<uint64_t>(done)))
                                                                 .arg(formatSize(static_cast<uint64_t>(total)))));
                });

            if (m_cancelled)
                return;

            if (result.isOk())
            {
                QString extractedPath = result.value();
                QMetaObject::invokeMethod(this, [this, extractedPath, flashAfter]() {
                    m_statusLabel->setText(tr("Decompressed: %1").arg(extractedPath));
                    if (flashAfter)
                        flashImage(extractedPath);
                    else
                    {
                        setOperationRunning(false);
                        emit statusMessage(tr("Decompression complete"));
                    }
                }, Qt::QueuedConnection);
            }
            else
            {
                QString errMsg = QString::fromStdString(result.error().message);
                QMetaObject::invokeMethod(this, [this, errMsg]() {
                    m_statusLabel->setText(tr("Decompression failed: %1").arg(errMsg));
                    setOperationRunning(false);
                    emit statusMessage(tr("Decompression failed"));
                }, Qt::QueuedConnection);
            }
        });

        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
    }
}

void LinuxFlasherTab::flashImage(const QString& imagePath)
{
    if (m_cancelled)
    {
        setOperationRunning(false);
        return;
    }

    if (m_targetDriveCombo->currentData().isNull())
    {
        m_statusLabel->setText(tr("No target drive selected for flashing."));
        setOperationRunning(false);
        return;
    }

    int targetDiskId = m_targetDriveCombo->currentData().toInt();
    m_statusLabel->setText(tr("Flashing to Disk %1...").arg(targetDiskId));
    m_progressBar->setValue(0);

    std::wstring imgPathW = imagePath.toStdWString();

    auto* thread = QThread::create([this, imgPathW, targetDiskId]() {
        auto result = VirtualDisk::flashToDisk(imgPathW, targetDiskId,
            [this](const std::string& stage, int pct) {
                if (m_cancelled)
                    return;
                QString stageStr = QString::fromStdString(stage);
                QMetaObject::invokeMethod(m_progressBar, "setValue",
                                          Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_statusLabel, "setText",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, tr("Flashing: %1 (%2%)")
                                                             .arg(stageStr).arg(pct)));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            setOperationRunning(false);
            if (result.isOk())
            {
                m_statusLabel->setText(tr("Flash complete! You may now safely remove the drive."));
                QMessageBox::information(this, tr("Flash Complete"),
                                         tr("The Linux image has been flashed successfully.\n\n"
                                            "You may safely eject the drive."));
                emit statusMessage(tr("Linux image flashed successfully"));
            }
            else
            {
                QString errMsg = QString::fromStdString(result.error().message);
                m_statusLabel->setText(tr("Flash failed: %1").arg(errMsg));
                QMessageBox::critical(this, tr("Flash Failed"),
                                      tr("Failed to flash the image:\n%1").arg(errMsg));
                emit statusMessage(tr("Flash failed"));
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void LinuxFlasherTab::setOperationRunning(bool running)
{
    m_progressBar->setVisible(running);
    m_cancelBtn->setVisible(running);
    m_downloadFlashBtn->setEnabled(!running);
    m_downloadOnlyBtn->setEnabled(!running);

    if (!running)
    {
        m_progressBar->setValue(0);
        m_speedLabel->clear();
    }
}

QString LinuxFlasherTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

} // namespace spw
