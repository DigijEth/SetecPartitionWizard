#include "ImagingTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/imaging/DiskCloner.h"
#include "core/imaging/ImageCreator.h"
#include "core/imaging/ImageRestorer.h"
#include "core/imaging/IsoFlasher.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

ImagingTab::ImagingTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

ImagingTab::~ImagingTab() = default;

void ImagingTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    // ===== Clone Disk =====
    auto* cloneGroup = new QGroupBox(tr("Clone Disk"));
    auto* cloneLayout = new QGridLayout(cloneGroup);

    cloneLayout->addWidget(new QLabel(tr("Source:")), 0, 0);
    m_cloneSourceCombo = new QComboBox();
    cloneLayout->addWidget(m_cloneSourceCombo, 0, 1, 1, 2);

    cloneLayout->addWidget(new QLabel(tr("Destination:")), 1, 0);
    m_cloneDestCombo = new QComboBox();
    cloneLayout->addWidget(m_cloneDestCombo, 1, 1, 1, 2);

    cloneLayout->addWidget(new QLabel(tr("Mode:")), 2, 0);
    m_cloneModeCombo = new QComboBox();
    m_cloneModeCombo->addItems({tr("Raw (sector-by-sector)"), tr("Smart (skip free space)")});
    cloneLayout->addWidget(m_cloneModeCombo, 2, 1);

    m_cloneVerifyCheck = new QCheckBox(tr("Verify after clone"));
    m_cloneVerifyCheck->setChecked(true);
    cloneLayout->addWidget(m_cloneVerifyCheck, 2, 2);

    m_cloneProgress = new QProgressBar();
    m_cloneProgress->setVisible(false);
    cloneLayout->addWidget(m_cloneProgress, 3, 0, 1, 2);

    m_cloneSpeedLabel = new QLabel();
    cloneLayout->addWidget(m_cloneSpeedLabel, 3, 2);

    m_cloneBtn = new QPushButton(tr("Clone"));
    m_cloneBtn->setObjectName("applyButton");
    connect(m_cloneBtn, &QPushButton::clicked, this, &ImagingTab::onCloneDisk);
    cloneLayout->addWidget(m_cloneBtn, 4, 2, Qt::AlignRight);

    layout->addWidget(cloneGroup);

    // ===== Create Image =====
    auto* imageGroup = new QGroupBox(tr("Create Disk Image"));
    auto* imageLayout = new QGridLayout(imageGroup);

    imageLayout->addWidget(new QLabel(tr("Source:")), 0, 0);
    m_imageSourceCombo = new QComboBox();
    imageLayout->addWidget(m_imageSourceCombo, 0, 1, 1, 2);

    imageLayout->addWidget(new QLabel(tr("Output File:")), 1, 0);
    m_imageOutputEdit = new QLineEdit();
    imageLayout->addWidget(m_imageOutputEdit, 1, 1);
    auto* imgBrowseBtn = new QPushButton(tr("Browse..."));
    connect(imgBrowseBtn, &QPushButton::clicked, this, &ImagingTab::onBrowseImageOutput);
    imageLayout->addWidget(imgBrowseBtn, 1, 2);

    imageLayout->addWidget(new QLabel(tr("Format:")), 2, 0);
    m_imageFormatCombo = new QComboBox();
    m_imageFormatCombo->addItems({tr("Raw (.img)"), tr("Compressed SPW (.spw)")});
    imageLayout->addWidget(m_imageFormatCombo, 2, 1);

    m_imageCreateProgress = new QProgressBar();
    m_imageCreateProgress->setVisible(false);
    imageLayout->addWidget(m_imageCreateProgress, 3, 0, 1, 2);

    m_imageCreateSpeedLabel = new QLabel();
    imageLayout->addWidget(m_imageCreateSpeedLabel, 3, 2);

    m_imageCreateBtn = new QPushButton(tr("Create Image"));
    m_imageCreateBtn->setObjectName("applyButton");
    connect(m_imageCreateBtn, &QPushButton::clicked, this, &ImagingTab::onCreateImage);
    imageLayout->addWidget(m_imageCreateBtn, 4, 2, Qt::AlignRight);

    layout->addWidget(imageGroup);

    // ===== Restore Image =====
    auto* restoreGroup = new QGroupBox(tr("Restore Image"));
    auto* restoreLayout = new QGridLayout(restoreGroup);

    restoreLayout->addWidget(new QLabel(tr("Image File:")), 0, 0);
    m_restoreInputEdit = new QLineEdit();
    connect(m_restoreInputEdit, &QLineEdit::textChanged, this, &ImagingTab::onRestoreInputChanged);
    restoreLayout->addWidget(m_restoreInputEdit, 0, 1);
    auto* restBrowseBtn = new QPushButton(tr("Browse..."));
    connect(restBrowseBtn, &QPushButton::clicked, this, &ImagingTab::onBrowseRestoreInput);
    restoreLayout->addWidget(restBrowseBtn, 0, 2);

    m_restoreImageInfo = new QLabel(tr("No image selected"));
    m_restoreImageInfo->setWordWrap(true);
    m_restoreImageInfo->setStyleSheet("color: #6c7086; padding: 4px;");
    restoreLayout->addWidget(m_restoreImageInfo, 1, 0, 1, 3);

    restoreLayout->addWidget(new QLabel(tr("Destination:")), 2, 0);
    m_restoreDestCombo = new QComboBox();
    restoreLayout->addWidget(m_restoreDestCombo, 2, 1);

    m_restoreVerifyCheck = new QCheckBox(tr("Verify after restore"));
    m_restoreVerifyCheck->setChecked(true);
    restoreLayout->addWidget(m_restoreVerifyCheck, 2, 2);

    m_restoreProgress = new QProgressBar();
    m_restoreProgress->setVisible(false);
    restoreLayout->addWidget(m_restoreProgress, 3, 0, 1, 2);

    m_restoreSpeedLabel = new QLabel();
    restoreLayout->addWidget(m_restoreSpeedLabel, 3, 2);

    m_restoreBtn = new QPushButton(tr("Restore"));
    m_restoreBtn->setObjectName("applyButton");
    connect(m_restoreBtn, &QPushButton::clicked, this, &ImagingTab::onRestoreImage);
    restoreLayout->addWidget(m_restoreBtn, 4, 2, Qt::AlignRight);

    layout->addWidget(restoreGroup);

    // ===== Flash ISO/IMG =====
    auto* flashGroup = new QGroupBox(tr("Flash ISO/IMG to USB"));
    auto* flashLayout = new QGridLayout(flashGroup);

    flashLayout->addWidget(new QLabel(tr("Image:")), 0, 0);
    m_flashInputEdit = new QLineEdit();
    flashLayout->addWidget(m_flashInputEdit, 0, 1);
    auto* flashBrowseBtn = new QPushButton(tr("Browse..."));
    connect(flashBrowseBtn, &QPushButton::clicked, this, &ImagingTab::onBrowseFlashInput);
    flashLayout->addWidget(flashBrowseBtn, 0, 2);

    flashLayout->addWidget(new QLabel(tr("Target USB:")), 1, 0);
    m_flashTargetCombo = new QComboBox();
    flashLayout->addWidget(m_flashTargetCombo, 1, 1);

    m_flashVerifyCheck = new QCheckBox(tr("Verify after flash"));
    m_flashVerifyCheck->setChecked(true);
    flashLayout->addWidget(m_flashVerifyCheck, 1, 2);

    m_flashProgress = new QProgressBar();
    m_flashProgress->setVisible(false);
    flashLayout->addWidget(m_flashProgress, 2, 0, 1, 2);

    m_flashSpeedLabel = new QLabel();
    flashLayout->addWidget(m_flashSpeedLabel, 2, 2);

    m_flashBtn = new QPushButton(tr("Flash"));
    m_flashBtn->setObjectName("applyButton");
    connect(m_flashBtn, &QPushButton::clicked, this, &ImagingTab::onFlashIso);
    flashLayout->addWidget(m_flashBtn, 3, 2, Qt::AlignRight);

    layout->addWidget(flashGroup);

    layout->addStretch();
}

void ImagingTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombos();
}

void ImagingTab::populateDiskCombos()
{
    // Clear all combos
    m_cloneSourceCombo->clear();
    m_cloneDestCombo->clear();
    m_imageSourceCombo->clear();
    m_restoreDestCombo->clear();
    m_flashTargetCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));

        m_cloneSourceCombo->addItem(label, disk.id);
        m_cloneDestCombo->addItem(label, disk.id);
        m_imageSourceCombo->addItem(label, disk.id);
        m_restoreDestCombo->addItem(label, disk.id);

        // Flash target: only removable drives
        if (disk.isRemovable)
        {
            m_flashTargetCombo->addItem(label, disk.id);
        }
    }

    if (m_flashTargetCombo->count() == 0)
    {
        m_flashTargetCombo->addItem(tr("No removable drives detected"));
    }
}

void ImagingTab::onCloneDisk()
{
    int srcDiskId = m_cloneSourceCombo->currentData().toInt();
    int dstDiskId = m_cloneDestCombo->currentData().toInt();

    if (srcDiskId == dstDiskId)
    {
        QMessageBox::warning(this, tr("Invalid"), tr("Source and destination must be different disks."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Clone Disk"),
                                      tr("ALL data on Disk %1 will be OVERWRITTEN.\n\n"
                                         "Source: Disk %2\nDestination: Disk %1\n\nContinue?")
                                          .arg(dstDiskId).arg(srcDiskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    CloneConfig config;
    config.sourceDiskId = srcDiskId;
    config.destDiskId = dstDiskId;
    config.mode = m_cloneModeCombo->currentIndex() == 0 ? CloneMode::Raw : CloneMode::Smart;
    config.verifyAfterClone = m_cloneVerifyCheck->isChecked();

    m_cloneProgress->setVisible(true);
    m_cloneProgress->setValue(0);
    m_cloneBtn->setEnabled(false);

    auto* thread = QThread::create([this, config]() {
        DiskCloner cloner;
        cloner.clone(config, [this](const CloneProgress& progress) -> bool {
            int pct = static_cast<int>(progress.percentComplete);
            double speedMB = progress.speedBytesPerSec / (1024.0 * 1024.0);
            QMetaObject::invokeMethod(m_cloneProgress, "setValue",
                                      Qt::QueuedConnection, Q_ARG(int, pct));
            QMetaObject::invokeMethod(m_cloneSpeedLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString("%1 MB/s, ETA: %2s")
                                                         .arg(speedMB, 0, 'f', 1)
                                                         .arg(static_cast<int>(progress.etaSeconds))));
            return true;
        });
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_cloneProgress->setVisible(false);
        m_cloneBtn->setEnabled(true);
        m_cloneSpeedLabel->clear();
        QMessageBox::information(this, tr("Clone Complete"), tr("Disk cloning completed."));
        emit statusMessage(tr("Disk clone completed"));
    });

    thread->start();
}

void ImagingTab::onCreateImage()
{
    int srcDiskId = m_imageSourceCombo->currentData().toInt();
    QString outputPath = m_imageOutputEdit->text();

    if (outputPath.isEmpty())
    {
        QMessageBox::warning(this, tr("No Output"), tr("Please specify an output file."));
        return;
    }

    ImageCreateConfig config;
    config.sourceDiskId = srcDiskId;
    config.outputFilePath = outputPath.toStdWString();
    config.format = m_imageFormatCombo->currentIndex() == 0 ? ImageFormat::Raw : ImageFormat::SPW;
    config.enableCompression = (config.format == ImageFormat::SPW);

    m_imageCreateProgress->setVisible(true);
    m_imageCreateProgress->setValue(0);
    m_imageCreateBtn->setEnabled(false);

    auto* thread = QThread::create([this, config]() {
        ImageCreator creator;
        creator.createImage(config, [this](const ImageCreateProgress& progress) -> bool {
            int pct = static_cast<int>(progress.percentComplete);
            double speedMB = progress.speedBytesPerSec / (1024.0 * 1024.0);
            QMetaObject::invokeMethod(m_imageCreateProgress, "setValue",
                                      Qt::QueuedConnection, Q_ARG(int, pct));
            QString info = QString("%1 MB/s").arg(speedMB, 0, 'f', 1);
            if (progress.compressionRatio > 0)
                info += QString(", Ratio: %1:1").arg(progress.compressionRatio, 0, 'f', 1);
            QMetaObject::invokeMethod(m_imageCreateSpeedLabel, "setText",
                                      Qt::QueuedConnection, Q_ARG(QString, info));
            return true;
        });
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_imageCreateProgress->setVisible(false);
        m_imageCreateBtn->setEnabled(true);
        m_imageCreateSpeedLabel->clear();
        QMessageBox::information(this, tr("Image Created"), tr("Disk image created successfully."));
        emit statusMessage(tr("Image creation completed"));
    });

    thread->start();
}

void ImagingTab::onRestoreImage()
{
    QString inputPath = m_restoreInputEdit->text();
    int dstDiskId = m_restoreDestCombo->currentData().toInt();

    if (inputPath.isEmpty())
    {
        QMessageBox::warning(this, tr("No Input"), tr("Please specify an input image file."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Restore Image"),
                                      tr("ALL data on Disk %1 will be OVERWRITTEN with the image contents.\n\nContinue?")
                                          .arg(dstDiskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    ImageRestoreConfig config;
    config.inputFilePath = inputPath.toStdWString();
    config.destDiskId = dstDiskId;
    config.verifyAfterRestore = m_restoreVerifyCheck->isChecked();

    m_restoreProgress->setVisible(true);
    m_restoreProgress->setValue(0);
    m_restoreBtn->setEnabled(false);

    auto* thread = QThread::create([this, config]() {
        ImageRestorer restorer;
        restorer.restoreImage(config, [this](const ImageRestoreProgress& progress) -> bool {
            int pct = static_cast<int>(progress.percentComplete);
            double speedMB = progress.speedBytesPerSec / (1024.0 * 1024.0);
            QMetaObject::invokeMethod(m_restoreProgress, "setValue",
                                      Qt::QueuedConnection, Q_ARG(int, pct));
            QMetaObject::invokeMethod(m_restoreSpeedLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString("%1 MB/s, ETA: %2s")
                                                         .arg(speedMB, 0, 'f', 1)
                                                         .arg(static_cast<int>(progress.etaSeconds))));
            return true;
        });
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_restoreProgress->setVisible(false);
        m_restoreBtn->setEnabled(true);
        m_restoreSpeedLabel->clear();
        QMessageBox::information(this, tr("Restore Complete"), tr("Image restoration completed."));
        emit statusMessage(tr("Image restore completed"));
    });

    thread->start();
}

void ImagingTab::onFlashIso()
{
    QString inputPath = m_flashInputEdit->text();
    int targetDiskId = m_flashTargetCombo->currentData().toInt();

    if (inputPath.isEmpty())
    {
        QMessageBox::warning(this, tr("No Input"), tr("Please specify an ISO/IMG file."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Flash ISO/IMG"),
                                      tr("ALL data on the target USB drive (Disk %1) will be DESTROYED.\n\nContinue?")
                                          .arg(targetDiskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    FlashConfig config;
    config.inputFilePath = inputPath.toStdWString();
    config.targetDiskId = targetDiskId;
    config.verifyAfterFlash = m_flashVerifyCheck->isChecked();

    m_flashProgress->setVisible(true);
    m_flashProgress->setValue(0);
    m_flashBtn->setEnabled(false);

    auto* thread = QThread::create([this, config]() {
        IsoFlasher flasher;
        flasher.flash(config, [this](const FlashProgress& progress) -> bool {
            int pct = static_cast<int>(progress.percentComplete);
            double speedMB = progress.speedBytesPerSec / (1024.0 * 1024.0);
            QMetaObject::invokeMethod(m_flashProgress, "setValue",
                                      Qt::QueuedConnection, Q_ARG(int, pct));
            QMetaObject::invokeMethod(m_flashSpeedLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString("%1 MB/s, ETA: %2s")
                                                         .arg(speedMB, 0, 'f', 1)
                                                         .arg(static_cast<int>(progress.etaSeconds))));
            return true;
        });
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_flashProgress->setVisible(false);
        m_flashBtn->setEnabled(true);
        m_flashSpeedLabel->clear();
        QMessageBox::information(this, tr("Flash Complete"), tr("ISO/IMG flash completed."));
        emit statusMessage(tr("Flash completed"));
    });

    thread->start();
}

void ImagingTab::onBrowseImageOutput()
{
    QString file = QFileDialog::getSaveFileName(this, tr("Save Image As"),
                                                QString(),
                                                tr("Raw Image (*.img);;SPW Compressed (*.spw);;All Files (*)"));
    if (!file.isEmpty())
        m_imageOutputEdit->setText(file);
}

void ImagingTab::onBrowseRestoreInput()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Select Image File"),
                                                QString(),
                                                tr("Image Files (*.img *.spw);;All Files (*)"));
    if (!file.isEmpty())
        m_restoreInputEdit->setText(file);
}

void ImagingTab::onBrowseFlashInput()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Select ISO/IMG File"),
                                                QString(),
                                                tr("Disk Images (*.iso *.img);;All Files (*)"));
    if (!file.isEmpty())
        m_flashInputEdit->setText(file);
}

void ImagingTab::onRestoreInputChanged()
{
    QString path = m_restoreInputEdit->text();
    if (path.isEmpty())
    {
        m_restoreImageInfo->setText(tr("No image selected"));
        return;
    }

    // Try to read SPW image info
    auto infoResult = ImageRestorer::inspectImage(path.toStdWString());
    if (infoResult.isOk())
    {
        const auto& info = infoResult.value();
        m_restoreImageInfo->setText(
            QString("Model: %1\nSerial: %2\nSize: %3\nChunks: %4 (%5 sparse)\n%6")
                .arg(QString::fromStdString(info.diskModel))
                .arg(QString::fromStdString(info.diskSerial))
                .arg(formatSize(info.imageDataSize))
                .arg(info.chunkCount)
                .arg(info.sparseChunkCount)
                .arg(info.isCompressed ? tr("Compressed") : tr("Uncompressed")));
    }
    else
    {
        // Might be a raw image
        auto fmtResult = ImageRestorer::detectFormat(path.toStdWString());
        if (fmtResult.isOk() && fmtResult.value() == ImageFormat::Raw)
        {
            m_restoreImageInfo->setText(tr("Raw image file"));
        }
        else
        {
            m_restoreImageInfo->setText(tr("Unable to read image info"));
        }
    }
}

QString ImagingTab::formatSize(uint64_t bytes)
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
