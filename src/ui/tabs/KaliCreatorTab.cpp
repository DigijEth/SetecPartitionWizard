#include "KaliCreatorTab.h"

#include "core/imaging/VirtualDisk.h"
#include "core/net/DownloadManager.h"
#include "core/imaging/Decompressor.h"
#include "core/imaging/SevenZipExtractor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QTextCursor>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

// ============================================================================
// Kali image variant definitions
// ============================================================================
struct KaliVariant
{
    const char* label;
    const char* url;
    bool isLive;
};

static const KaliVariant kUsbVariants[] = {
    { "Kali Installer (amd64)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-installer-amd64.iso",
      false },
    { "Kali Live (amd64)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-live-amd64.iso",
      true },
    { "Kali NetInstaller (amd64)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-installer-netinst-amd64.iso",
      false },
    { "Kali Installer (i386)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-installer-i386.iso",
      false },
    { "Kali Live (i386)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-live-i386.iso",
      true },
    { "Kali ARM64 (Raspberry Pi)",
      "https://kali.download/arm-images/kali-2024.4/kali-linux-2024.4-raspberry-pi-arm64.img.xz",
      false },
    { "Kali ARM64 (Pine64)",
      "https://kali.download/arm-images/kali-2024.4/kali-linux-2024.4-pinebook-pro-arm64.img.xz",
      false },
    { "Kali ARM64 (Generic)",
      "https://kali.download/arm-images/kali-2024.4/kali-linux-2024.4-arm64-generic.img.xz",
      false },
};
static constexpr int kUsbVariantCount = sizeof(kUsbVariants) / sizeof(kUsbVariants[0]);

struct KaliVmPrebuilt
{
    const char* label;
    const char* url;
};

static const KaliVmPrebuilt kPrebuiltVms[] = {
    { "Kali VMware (64-bit)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-vmware-amd64.7z" },
    { "Kali VirtualBox (64-bit)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-virtualbox-amd64.7z" },
    { "Kali Hyper-V (64-bit)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-hyperv-amd64.7z" },
    { "Kali QEMU (64-bit)",
      "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-qemu-amd64.7z" },
};
static constexpr int kPrebuiltVmCount = sizeof(kPrebuiltVms) / sizeof(kPrebuiltVms[0]);

// ============================================================================
// Constructor / Destructor
// ============================================================================

KaliCreatorTab::KaliCreatorTab(QWidget* parent)
    : QWidget(parent)
    , m_downloader(new DownloadManager(this))
{
    setupUi();
}

KaliCreatorTab::~KaliCreatorTab() = default;

// ============================================================================
// formatSize
// ============================================================================

QString KaliCreatorTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 0);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

// ============================================================================
// setupUi
// ============================================================================

void KaliCreatorTab::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);

    auto* innerTabs = new QTabWidget();

    setupUsbTab(innerTabs);
    setupVmTab(innerTabs);
    setupContainerTab(innerTabs);
    setupCloudTab(innerTabs);

    mainLayout->addWidget(innerTabs);
}

// ============================================================================
// Sub-tab 1: USB / SD Card
// ============================================================================

void KaliCreatorTab::setupUsbTab(QTabWidget* tabs)
{
    auto* widget = new QWidget();
    auto* outerLayout = new QVBoxLayout(widget);

    auto* infoLabel = new QLabel(
        tr("Download a Kali Linux image from kali.org and flash it directly to a "
           "USB drive or SD card. ARM images (.img.xz) are automatically decompressed "
           "before flashing."));
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    outerLayout->addWidget(infoLabel);

    auto* group = new QGroupBox(tr("Flash Kali to USB / SD Card"));
    auto* grid = new QGridLayout(group);

    // Image source
    grid->addWidget(new QLabel(tr("Kali Image:")), 0, 0);
    m_usbImageCombo = new QComboBox();
    for (int i = 0; i < kUsbVariantCount; ++i)
        m_usbImageCombo->addItem(tr(kUsbVariants[i].label), i);
    connect(m_usbImageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KaliCreatorTab::onUsbImageChanged);
    grid->addWidget(m_usbImageCombo, 0, 1, 1, 2);

    // Target drive
    grid->addWidget(new QLabel(tr("Target Drive:")), 1, 0);
    m_usbTargetCombo = new QComboBox();
    grid->addWidget(m_usbTargetCombo, 1, 1, 1, 2);

    // Persistence
    m_usbPersistCheck = new QCheckBox(tr("Create persistence partition"));
    connect(m_usbPersistCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_usbPersistSizeSpin->setEnabled(checked);
        m_usbPersistLabel->setEnabled(checked);
    });
    grid->addWidget(m_usbPersistCheck, 2, 0, 1, 2);

    m_usbPersistLabel = new QLabel(tr("Persistence Size (GB):"));
    m_usbPersistLabel->setEnabled(false);
    grid->addWidget(m_usbPersistLabel, 3, 0);

    m_usbPersistSizeSpin = new QSpinBox();
    m_usbPersistSizeSpin->setRange(1, 128);
    m_usbPersistSizeSpin->setValue(4);
    m_usbPersistSizeSpin->setSuffix(tr(" GB"));
    m_usbPersistSizeSpin->setEnabled(false);
    grid->addWidget(m_usbPersistSizeSpin, 3, 1);

    // Progress + status
    m_usbProgress = new QProgressBar();
    m_usbProgress->setVisible(false);
    grid->addWidget(m_usbProgress, 4, 0, 1, 3);

    m_usbStatusLabel = new QLabel();
    m_usbStatusLabel->setWordWrap(true);
    grid->addWidget(m_usbStatusLabel, 5, 0, 1, 3);

    // Flash button
    m_usbFlashBtn = new QPushButton(tr("Flash"));
    m_usbFlashBtn->setObjectName("applyButton");
    connect(m_usbFlashBtn, &QPushButton::clicked, this, &KaliCreatorTab::onFlashToUsb);
    grid->addWidget(m_usbFlashBtn, 6, 2, Qt::AlignRight);

    outerLayout->addWidget(group);
    outerLayout->addStretch();

    // Initialize persistence state based on default selection
    onUsbImageChanged(m_usbImageCombo->currentIndex());

    tabs->addTab(widget, tr("USB / SD Card"));
}

// ============================================================================
// Sub-tab 2: Virtual Machine
// ============================================================================

void KaliCreatorTab::setupVmTab(QTabWidget* tabs)
{
    auto* widget = new QWidget();
    auto* outerLayout = new QVBoxLayout(widget);

    auto* infoLabel = new QLabel(
        tr("Create a virtual disk for Kali Linux, or download a pre-built VM from kali.org. "
           "QCOW2/VMDK creation requires qemu-img on PATH."));
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    outerLayout->addWidget(infoLabel);

    // ---- Create VM Disk group ----
    auto* createGroup = new QGroupBox(tr("Create New VM Disk"));
    auto* createGrid = new QGridLayout(createGroup);

    createGrid->addWidget(new QLabel(tr("VM Format:")), 0, 0);
    m_vmFormatCombo = new QComboBox();
    m_vmFormatCombo->addItems({
        tr("QCOW2 (QEMU/KVM)"),
        tr("VMDK  (VMware)"),
        tr("VDI   (VirtualBox)"),
        tr("VHDX  (Hyper-V)"),
    });
    createGrid->addWidget(m_vmFormatCombo, 0, 1, 1, 2);

    createGrid->addWidget(new QLabel(tr("VM Size:")), 1, 0);
    m_vmSizeSpin = new QSpinBox();
    m_vmSizeSpin->setRange(16, 512);
    m_vmSizeSpin->setValue(64);
    m_vmSizeSpin->setSuffix(tr(" GB"));
    createGrid->addWidget(m_vmSizeSpin, 1, 1);

    createGrid->addWidget(new QLabel(tr("Output Path:")), 2, 0);
    m_vmOutputEdit = new QLineEdit();
    m_vmOutputEdit->setPlaceholderText(tr("e.g. C:\\VMs\\kali.qcow2"));
    createGrid->addWidget(m_vmOutputEdit, 2, 1);
    auto* vmBrowseBtn = new QPushButton(tr("Browse..."));
    connect(vmBrowseBtn, &QPushButton::clicked, this, &KaliCreatorTab::onBrowseVmOutput);
    createGrid->addWidget(vmBrowseBtn, 2, 2);

    createGrid->addWidget(new QLabel(tr("Kali ISO:")), 3, 0);
    m_vmVersionCombo = new QComboBox();
    for (int i = 0; i < kUsbVariantCount; ++i)
    {
        // Only offer ISO images (not ARM .img.xz) for VMs
        QString url = QString::fromLatin1(kUsbVariants[i].url);
        if (url.endsWith(".iso"))
            m_vmVersionCombo->addItem(tr(kUsbVariants[i].label), i);
    }
    createGrid->addWidget(m_vmVersionCombo, 3, 1, 1, 2);

    m_vmCreateBtn = new QPushButton(tr("Create VM Disk"));
    m_vmCreateBtn->setObjectName("applyButton");
    connect(m_vmCreateBtn, &QPushButton::clicked, this, &KaliCreatorTab::onCreateVmDisk);
    createGrid->addWidget(m_vmCreateBtn, 4, 2, Qt::AlignRight);

    outerLayout->addWidget(createGroup);

    // ---- Download Pre-built VM group ----
    auto* prebuiltGroup = new QGroupBox(tr("Download Pre-built Kali VM"));
    auto* prebuiltLayout = new QVBoxLayout(prebuiltGroup);

    auto* prebuiltInfo = new QLabel(
        tr("Download official pre-built Kali VMs from kali.org. "
           "These come as .7z archives containing ready-to-use VM images."));
    prebuiltInfo->setWordWrap(true);
    prebuiltInfo->setStyleSheet("color: #aaaaaa; font-style: italic;");
    prebuiltLayout->addWidget(prebuiltInfo);

    m_vmDownloadBtn = new QPushButton(tr("Download Pre-built VM"));
    m_vmDownloadBtn->setObjectName("applyButton");
    connect(m_vmDownloadBtn, &QPushButton::clicked, this, &KaliCreatorTab::onDownloadPrebuiltVm);
    prebuiltLayout->addWidget(m_vmDownloadBtn);

    outerLayout->addWidget(prebuiltGroup);

    // Progress + status (shared)
    m_vmProgress = new QProgressBar();
    m_vmProgress->setVisible(false);
    outerLayout->addWidget(m_vmProgress);

    m_vmStatusLabel = new QLabel();
    m_vmStatusLabel->setWordWrap(true);
    outerLayout->addWidget(m_vmStatusLabel);

    outerLayout->addStretch();
    tabs->addTab(widget, tr("Virtual Machine"));
}

// ============================================================================
// Sub-tab 3: Containers
// ============================================================================

void KaliCreatorTab::setupContainerTab(QTabWidget* tabs)
{
    auto* widget = new QWidget();
    auto* outerLayout = new QVBoxLayout(widget);

    auto* infoLabel = new QLabel(
        tr("Pull official Kali Linux container images using Docker or Podman. "
           "The selected runtime must be installed and accessible on your PATH."));
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    outerLayout->addWidget(infoLabel);

    auto* group = new QGroupBox(tr("Pull Kali Container Image"));
    auto* grid = new QGridLayout(group);

    grid->addWidget(new QLabel(tr("Runtime:")), 0, 0);
    m_containerRuntimeCombo = new QComboBox();
    m_containerRuntimeCombo->addItems({tr("Docker"), tr("Podman")});
    connect(m_containerRuntimeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KaliCreatorTab::onContainerRuntimeChanged);
    grid->addWidget(m_containerRuntimeCombo, 0, 1, 1, 2);

    grid->addWidget(new QLabel(tr("Image Tag:")), 1, 0);
    m_containerTagCombo = new QComboBox();
    m_containerTagCombo->addItems({
        QStringLiteral("kalilinux/kali-rolling"),
        QStringLiteral("kalilinux/kali-last-release"),
        QStringLiteral("kalilinux/kali-experimental"),
    });
    connect(m_containerTagCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &KaliCreatorTab::onContainerTagChanged);
    grid->addWidget(m_containerTagCombo, 1, 1, 1, 2);

    grid->addWidget(new QLabel(tr("Pull Command:")), 2, 0);
    m_containerCmdPreview = new QLineEdit();
    m_containerCmdPreview->setReadOnly(true);
    grid->addWidget(m_containerCmdPreview, 2, 1, 1, 2);

    m_containerPullBtn = new QPushButton(tr("Pull Image"));
    m_containerPullBtn->setObjectName("applyButton");
    connect(m_containerPullBtn, &QPushButton::clicked, this, &KaliCreatorTab::onPullContainerImage);
    grid->addWidget(m_containerPullBtn, 3, 2, Qt::AlignRight);

    outerLayout->addWidget(group);

    // Output log
    auto* logGroup = new QGroupBox(tr("Pull Output"));
    auto* logLayout = new QVBoxLayout(logGroup);
    m_containerLog = new QPlainTextEdit();
    m_containerLog->setReadOnly(true);
    m_containerLog->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    logLayout->addWidget(m_containerLog);
    outerLayout->addWidget(logGroup, 1);

    // Initialize preview
    updateContainerPullPreview();

    tabs->addTab(widget, tr("Containers"));
}

// ============================================================================
// Sub-tab 4: Cloud Image
// ============================================================================

void KaliCreatorTab::setupCloudTab(QTabWidget* tabs)
{
    auto* widget = new QWidget();
    auto* outerLayout = new QVBoxLayout(widget);

    auto* infoLabel = new QLabel(
        tr("Download official Kali Linux cloud images for deployment on AWS, Azure, GCP, "
           "or self-hosted cloud infrastructure (OpenStack, Proxmox, etc.)."));
    infoLabel->setWordWrap(true);
    infoLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    outerLayout->addWidget(infoLabel);

    auto* group = new QGroupBox(tr("Download Kali Cloud Image"));
    auto* grid = new QGridLayout(group);

    grid->addWidget(new QLabel(tr("Cloud Format:")), 0, 0);
    m_cloudFormatCombo = new QComboBox();
    m_cloudFormatCombo->addItems({
        tr("Raw (.img)  -- direct disk image"),
        tr("QCOW2       -- OpenStack / Proxmox / KVM"),
        tr("OVA         -- VMware vSphere / generic import"),
    });
    grid->addWidget(m_cloudFormatCombo, 0, 1, 1, 2);

    m_cloudInfoLabel = new QLabel(
        tr("These are the official Kali cloud images hosted at cdimage.kali.org. "
           "Raw and QCOW2 images are typically compressed with .xz and will be "
           "decompressed automatically after download."));
    m_cloudInfoLabel->setWordWrap(true);
    m_cloudInfoLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
    grid->addWidget(m_cloudInfoLabel, 1, 0, 1, 3);

    grid->addWidget(new QLabel(tr("Output Path:")), 2, 0);
    m_cloudOutputEdit = new QLineEdit();
    m_cloudOutputEdit->setPlaceholderText(tr("e.g. C:\\Images\\kali-cloud.img"));
    grid->addWidget(m_cloudOutputEdit, 2, 1);
    auto* cloudBrowseBtn = new QPushButton(tr("Browse..."));
    connect(cloudBrowseBtn, &QPushButton::clicked, this, &KaliCreatorTab::onBrowseCloudOutput);
    grid->addWidget(cloudBrowseBtn, 2, 2);

    m_cloudProgress = new QProgressBar();
    m_cloudProgress->setVisible(false);
    grid->addWidget(m_cloudProgress, 3, 0, 1, 3);

    m_cloudStatusLabel = new QLabel();
    m_cloudStatusLabel->setWordWrap(true);
    grid->addWidget(m_cloudStatusLabel, 4, 0, 1, 3);

    m_cloudDownloadBtn = new QPushButton(tr("Download Cloud Image"));
    m_cloudDownloadBtn->setObjectName("applyButton");
    connect(m_cloudDownloadBtn, &QPushButton::clicked, this, &KaliCreatorTab::onDownloadCloudImage);
    grid->addWidget(m_cloudDownloadBtn, 5, 2, Qt::AlignRight);

    outerLayout->addWidget(group);
    outerLayout->addStretch();

    tabs->addTab(widget, tr("Cloud Image"));
}

// ============================================================================
// refreshDisks
// ============================================================================

void KaliCreatorTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateRemovableDrives();
}

void KaliCreatorTab::populateRemovableDrives()
{
    m_usbTargetCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        if (!disk.isRemovable)
            continue;

        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));
        m_usbTargetCombo->addItem(label, disk.id);
    }

    if (m_usbTargetCombo->count() == 0)
        m_usbTargetCombo->addItem(tr("No removable drives detected"));
}

// ============================================================================
// USB / SD Card slots
// ============================================================================

void KaliCreatorTab::onUsbImageChanged(int index)
{
    if (index < 0 || index >= kUsbVariantCount)
        return;

    bool isLive = kUsbVariants[index].isLive;
    m_usbPersistCheck->setEnabled(isLive);
    if (!isLive)
    {
        m_usbPersistCheck->setChecked(false);
        m_usbPersistSizeSpin->setEnabled(false);
        m_usbPersistLabel->setEnabled(false);
    }
}

void KaliCreatorTab::onFlashToUsb()
{
    int variantIdx = m_usbImageCombo->currentData().toInt();
    if (variantIdx < 0 || variantIdx >= kUsbVariantCount)
    {
        QMessageBox::warning(this, tr("Flash"), tr("No Kali image selected."));
        return;
    }

    if (!m_usbTargetCombo->currentData().isValid())
    {
        QMessageBox::warning(this, tr("Flash"), tr("No removable drive selected. Insert a USB drive or SD card."));
        return;
    }

    int targetDiskId = m_usbTargetCombo->currentData().toInt();
    const KaliVariant& variant = kUsbVariants[variantIdx];

    auto reply = QMessageBox::warning(this, tr("Flash Kali to USB"),
        tr("ALL data on Disk %1 will be DESTROYED.\n\n"
           "Image: %2\nSource: %3\n\nContinue?")
            .arg(targetDiskId)
            .arg(tr(variant.label))
            .arg(QString::fromLatin1(variant.url)),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_usbFlashBtn->setEnabled(false);
    m_usbProgress->setVisible(true);
    m_usbProgress->setRange(0, 0);
    m_usbStatusLabel->setText(tr("Downloading Kali image..."));

    QUrl downloadUrl(QString::fromLatin1(variant.url));
    QString fileName = downloadUrl.fileName();
    QString tempDir = QDir::tempPath();
    QString downloadPath = tempDir + "/" + fileName;

    // Disconnect any prior connections from this downloader
    disconnect(m_downloader, nullptr, nullptr, nullptr);

    connect(m_downloader, &DownloadManager::progressChanged,
            this, [this](qint64 received, qint64 total) {
        if (total > 0)
        {
            m_usbProgress->setRange(0, 100);
            int pct = static_cast<int>((received * 100) / total);
            m_usbProgress->setValue(pct);
            m_usbStatusLabel->setText(tr("Downloading... %1 / %2")
                .arg(formatSize(static_cast<uint64_t>(received)))
                .arg(formatSize(static_cast<uint64_t>(total))));
        }
    });

    connect(m_downloader, &DownloadManager::downloadError,
            this, [this](const QString& error) {
        m_usbProgress->setVisible(false);
        m_usbFlashBtn->setEnabled(true);
        m_usbStatusLabel->setText(tr("Download failed: %1").arg(error));
    });

    connect(m_downloader, &DownloadManager::downloadComplete,
            this, [this, targetDiskId, downloadPath](const QString& filePath) {
        m_usbStatusLabel->setText(tr("Download complete. Preparing to flash..."));

        // Determine if decompression is needed (.xz or .gz)
        QString imagePath = filePath;
        bool needsDecompress = Decompressor::isCompressed(filePath);

        if (needsDecompress)
        {
            m_usbStatusLabel->setText(tr("Decompressing %1...").arg(QFileInfo(filePath).fileName()));
            m_usbProgress->setRange(0, 0); // indeterminate during decompression

            auto* decompThread = QThread::create([this, filePath, targetDiskId]() {
                QString decompressedName = Decompressor::decompressedName(filePath);
                QString outputDir = QFileInfo(filePath).absolutePath();
                auto result = Decompressor::decompressAuto(filePath, outputDir,
                    [this](qint64 done, qint64 total) {
                        if (total > 0)
                        {
                            int pct = static_cast<int>((done * 100) / total);
                            QMetaObject::invokeMethod(m_usbProgress, "setRange",
                                Qt::QueuedConnection, Q_ARG(int, 0), Q_ARG(int, 100));
                            QMetaObject::invokeMethod(m_usbProgress, "setValue",
                                Qt::QueuedConnection, Q_ARG(int, pct));
                        }
                    });

                if (result.isError())
                {
                    QMetaObject::invokeMethod(this, [this, result]() {
                        m_usbProgress->setVisible(false);
                        m_usbFlashBtn->setEnabled(true);
                        m_usbStatusLabel->setText(tr("Decompression failed: %1")
                            .arg(QString::fromStdString(result.error().message)));
                    }, Qt::QueuedConnection);
                    return;
                }

                QString decompPath = result.value();

                // Flash decompressed image to disk
                QMetaObject::invokeMethod(this, [this]() {
                    m_usbStatusLabel->setText(tr("Flashing image to disk..."));
                    m_usbProgress->setRange(0, 100);
                    m_usbProgress->setValue(0);
                }, Qt::QueuedConnection);

                auto flashResult = VirtualDisk::flashToDisk(
                    decompPath.toStdWString(), targetDiskId,
                    [this](const std::string& stage, int pct) {
                        QMetaObject::invokeMethod(m_usbProgress, "setValue",
                            Qt::QueuedConnection, Q_ARG(int, pct));
                        QMetaObject::invokeMethod(m_usbStatusLabel, "setText",
                            Qt::QueuedConnection,
                            Q_ARG(QString, QString::fromStdString(stage)));
                    });

                QMetaObject::invokeMethod(this, [this, flashResult]() {
                    m_usbProgress->setVisible(false);
                    m_usbFlashBtn->setEnabled(true);
                    if (flashResult.isOk())
                    {
                        m_usbStatusLabel->setText(tr("Kali image flashed successfully."));
                        emit statusMessage(tr("Kali USB flash completed"));
                    }
                    else
                    {
                        m_usbStatusLabel->setText(tr("Flash failed: %1")
                            .arg(QString::fromStdString(flashResult.error().message)));
                    }
                }, Qt::QueuedConnection);
            });

            connect(decompThread, &QThread::finished, decompThread, &QThread::deleteLater);
            decompThread->start();
        }
        else
        {
            // ISO files: flash directly
            m_usbStatusLabel->setText(tr("Flashing ISO to disk..."));
            m_usbProgress->setRange(0, 100);
            m_usbProgress->setValue(0);

            auto* flashThread = QThread::create([this, filePath, targetDiskId]() {
                auto flashResult = VirtualDisk::flashToDisk(
                    filePath.toStdWString(), targetDiskId,
                    [this](const std::string& stage, int pct) {
                        QMetaObject::invokeMethod(m_usbProgress, "setValue",
                            Qt::QueuedConnection, Q_ARG(int, pct));
                        QMetaObject::invokeMethod(m_usbStatusLabel, "setText",
                            Qt::QueuedConnection,
                            Q_ARG(QString, QString::fromStdString(stage)));
                    });

                QMetaObject::invokeMethod(this, [this, flashResult]() {
                    m_usbProgress->setVisible(false);
                    m_usbFlashBtn->setEnabled(true);
                    if (flashResult.isOk())
                    {
                        // Handle persistence if requested
                        m_usbStatusLabel->setText(tr("Kali image flashed successfully."));
                        emit statusMessage(tr("Kali USB flash completed"));
                    }
                    else
                    {
                        m_usbStatusLabel->setText(tr("Flash failed: %1")
                            .arg(QString::fromStdString(flashResult.error().message)));
                    }
                }, Qt::QueuedConnection);
            });

            connect(flashThread, &QThread::finished, flashThread, &QThread::deleteLater);
            flashThread->start();
        }
    });

    m_downloader->startDownload(downloadUrl, downloadPath);
}

// ============================================================================
// Virtual Machine slots
// ============================================================================

void KaliCreatorTab::onBrowseVmOutput()
{
    static const char* exts[] = { ".qcow2", ".vmdk", ".vdi", ".vhdx" };
    int fmtIdx = m_vmFormatCombo->currentIndex();
    QString ext = exts[fmtIdx < 4 ? fmtIdx : 0];

    QString path = QFileDialog::getSaveFileName(this, tr("Save VM Disk"),
        QString(), tr("Virtual Disk (*%1);;All Files (*)").arg(ext));
    if (!path.isEmpty())
        m_vmOutputEdit->setText(path);
}

void KaliCreatorTab::onCreateVmDisk()
{
    QString outputPath = m_vmOutputEdit->text().trimmed();
    if (outputPath.isEmpty())
    {
        QMessageBox::warning(this, tr("Create VM Disk"), tr("Please specify an output path."));
        return;
    }

    static const VirtualDiskFormat fmts[] = {
        VirtualDiskFormat::QCOW2,
        VirtualDiskFormat::VMDK,
        VirtualDiskFormat::VHD,   // VDI uses qemu-img convert, map to VHD for native
        VirtualDiskFormat::VHDX,
    };

    int fmtIdx = m_vmFormatCombo->currentIndex();
    VirtualDiskFormat fmt = fmts[fmtIdx < 4 ? fmtIdx : 0];

    // For VDI format, we need qemu-img
    bool needsQemuImg = (fmtIdx == 0 || fmtIdx == 1 || fmtIdx == 2);
    if (needsQemuImg && !VirtualDisk::qemuImgAvailable())
    {
        QMessageBox::warning(this, tr("Create VM Disk"),
            tr("This format requires qemu-img to be installed and on your PATH.\n\n"
               "Install QEMU for Windows from https://qemu.org or use VHDX format instead."));
        return;
    }

    uint64_t sizeGB = static_cast<uint64_t>(m_vmSizeSpin->value());
    uint64_t sizeBytes = sizeGB * 1024ULL * 1024 * 1024;

    VirtualDiskCreateParams params;
    params.filePath = outputPath.toStdWString();
    params.format = fmt;
    params.sizeBytes = sizeBytes;
    params.dynamic = true;

    m_vmCreateBtn->setEnabled(false);
    m_vmProgress->setVisible(true);
    m_vmProgress->setRange(0, 0);
    m_vmStatusLabel->setText(tr("Creating VM disk..."));

    auto* thread = QThread::create([this, params, fmtIdx, outputPath]() {
        // For VDI: create as RAW first, then convert via qemu-img
        if (fmtIdx == 2) // VDI
        {
            // Create a temporary raw image, then convert to VDI
            QString tmpPath = outputPath + ".tmp.raw";
            VirtualDiskCreateParams tmpParams = params;
            tmpParams.filePath = tmpPath.toStdWString();
            tmpParams.format = VirtualDiskFormat::RAW;

            QMetaObject::invokeMethod(m_vmStatusLabel, "setText",
                Qt::QueuedConnection, Q_ARG(QString, tr("Creating temporary raw image...")));

            auto createResult = VirtualDisk::create(tmpParams,
                [this](const std::string& stage, int pct) {
                    QMetaObject::invokeMethod(m_vmStatusLabel, "setText",
                        Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(stage)));
                });

            if (createResult.isError())
            {
                QMetaObject::invokeMethod(this, [this, createResult]() {
                    m_vmProgress->setVisible(false);
                    m_vmCreateBtn->setEnabled(true);
                    m_vmStatusLabel->setText(tr("Failed: %1")
                        .arg(QString::fromStdString(createResult.error().message)));
                }, Qt::QueuedConnection);
                return;
            }

            // Convert raw to VDI using qemu-img
            QMetaObject::invokeMethod(m_vmStatusLabel, "setText",
                Qt::QueuedConnection, Q_ARG(QString, tr("Converting to VDI format...")));

            QProcess qemuProc;
            qemuProc.setProcessChannelMode(QProcess::MergedChannels);
            qemuProc.start("qemu-img", {"convert", "-f", "raw", "-O", "vdi",
                                         tmpPath, outputPath});
            qemuProc.waitForFinished(600000);

            // Clean up temporary file
            QFile::remove(tmpPath);

            bool ok = (qemuProc.exitCode() == 0);
            QMetaObject::invokeMethod(this, [this, ok]() {
                m_vmProgress->setVisible(false);
                m_vmCreateBtn->setEnabled(true);
                m_vmStatusLabel->setText(ok
                    ? tr("VDI virtual disk created successfully.")
                    : tr("VDI conversion failed. Check that qemu-img is installed."));
                if (ok) emit statusMessage(tr("Kali VM disk created"));
            }, Qt::QueuedConnection);
        }
        else
        {
            auto result = VirtualDisk::create(params,
                [this](const std::string& stage, int pct) {
                    QMetaObject::invokeMethod(m_vmStatusLabel, "setText",
                        Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(stage)));
                });

            QMetaObject::invokeMethod(this, [this, result]() {
                m_vmProgress->setVisible(false);
                m_vmCreateBtn->setEnabled(true);
                m_vmStatusLabel->setText(result.isOk()
                    ? tr("VM disk created successfully.")
                    : tr("Failed: %1").arg(QString::fromStdString(result.error().message)));
                if (result.isOk()) emit statusMessage(tr("Kali VM disk created"));
            }, Qt::QueuedConnection);
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void KaliCreatorTab::onDownloadPrebuiltVm()
{
    // Show selection dialog for pre-built VMs
    QStringList items;
    for (int i = 0; i < kPrebuiltVmCount; ++i)
        items << tr(kPrebuiltVms[i].label);

    bool ok = false;
    // Use a simple approach: let user pick from items
    QString selected;
    {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Download Pre-built Kali VM"));
        auto* dlgLayout = new QVBoxLayout(&dlg);

        auto* vmCombo = new QComboBox();
        vmCombo->addItems(items);
        dlgLayout->addWidget(vmCombo);

        auto* btnRow = new QHBoxLayout();
        auto* okBtn = new QPushButton(tr("Download"));
        auto* cancelBtn = new QPushButton(tr("Cancel"));
        btnRow->addStretch();
        btnRow->addWidget(okBtn);
        btnRow->addWidget(cancelBtn);
        dlgLayout->addLayout(btnRow);

        connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted)
            return;

        ok = true;
        selected = vmCombo->currentText();
    }

    int vmIdx = items.indexOf(selected);
    if (vmIdx < 0 || vmIdx >= kPrebuiltVmCount)
        return;

    // Pick output directory
    QString outDir = QFileDialog::getExistingDirectory(this,
        tr("Select Download Directory"), QDir::homePath());
    if (outDir.isEmpty())
        return;

    const KaliVmPrebuilt& vm = kPrebuiltVms[vmIdx];
    QUrl url(QString::fromLatin1(vm.url));
    QString outputPath = outDir + "/" + url.fileName();

    m_vmDownloadBtn->setEnabled(false);
    m_vmProgress->setVisible(true);
    m_vmProgress->setRange(0, 0);
    m_vmStatusLabel->setText(tr("Downloading %1...").arg(tr(vm.label)));

    disconnect(m_downloader, nullptr, nullptr, nullptr);

    connect(m_downloader, &DownloadManager::progressChanged,
            this, [this](qint64 received, qint64 total) {
        if (total > 0)
        {
            m_vmProgress->setRange(0, 100);
            int pct = static_cast<int>((received * 100) / total);
            m_vmProgress->setValue(pct);
            m_vmStatusLabel->setText(tr("Downloading... %1 / %2")
                .arg(formatSize(static_cast<uint64_t>(received)))
                .arg(formatSize(static_cast<uint64_t>(total))));
        }
    });

    connect(m_downloader, &DownloadManager::downloadError,
            this, [this](const QString& error) {
        m_vmProgress->setVisible(false);
        m_vmDownloadBtn->setEnabled(true);
        m_vmStatusLabel->setText(tr("Download failed: %1").arg(error));
    });

    connect(m_downloader, &DownloadManager::downloadComplete,
            this, [this, outDir](const QString& filePath) {
        m_vmStatusLabel->setText(tr("Download complete. Extracting .7z archive..."));
        m_vmProgress->setRange(0, 0);

        // Extract with SevenZipExtractor if available
        if (SevenZipExtractor::isAvailable())
        {
            auto* extractor = new SevenZipExtractor(this);
            connect(extractor, &SevenZipExtractor::progressChanged,
                    this, [this](int pct) {
                m_vmProgress->setRange(0, 100);
                m_vmProgress->setValue(pct);
            });
            connect(extractor, &SevenZipExtractor::extractionComplete,
                    this, [this, extractor](const QString& outputDir) {
                m_vmProgress->setVisible(false);
                m_vmDownloadBtn->setEnabled(true);
                m_vmStatusLabel->setText(tr("VM extracted to: %1").arg(outputDir));
                emit statusMessage(tr("Pre-built Kali VM downloaded and extracted"));
                extractor->deleteLater();
            });
            connect(extractor, &SevenZipExtractor::extractionError,
                    this, [this, extractor](const QString& error) {
                m_vmProgress->setVisible(false);
                m_vmDownloadBtn->setEnabled(true);
                m_vmStatusLabel->setText(tr("Extraction failed: %1\n\n"
                    "The .7z file was downloaded. Extract it manually with 7-Zip.").arg(error));
                extractor->deleteLater();
            });
            extractor->extract(filePath, outDir);
        }
        else
        {
            m_vmProgress->setVisible(false);
            m_vmDownloadBtn->setEnabled(true);
            m_vmStatusLabel->setText(
                tr("Downloaded to: %1\n\n"
                   "7-Zip is not installed. Install 7-Zip (7-zip.org) to auto-extract, "
                   "or extract the .7z archive manually.").arg(filePath));
            emit statusMessage(tr("Pre-built Kali VM downloaded (extract manually)"));
        }
    });

    m_downloader->startDownload(url, outputPath);
}

// ============================================================================
// Container slots
// ============================================================================

void KaliCreatorTab::onContainerRuntimeChanged(int /*index*/)
{
    updateContainerPullPreview();
}

void KaliCreatorTab::onContainerTagChanged(int /*index*/)
{
    updateContainerPullPreview();
}

void KaliCreatorTab::updateContainerPullPreview()
{
    QString runtime = (m_containerRuntimeCombo->currentIndex() == 0) ? "docker" : "podman";
    QString tag = m_containerTagCombo->currentText();
    m_containerCmdPreview->setText(QString("%1 pull %2").arg(runtime, tag));
}

void KaliCreatorTab::onPullContainerImage()
{
    QString runtime = (m_containerRuntimeCombo->currentIndex() == 0) ? "docker" : "podman";
    QString tag = m_containerTagCombo->currentText();

    // Check if runtime is available
    QProcess check;
    check.setProcessChannelMode(QProcess::MergedChannels);
    check.start(runtime, {"--version"});
    check.waitForFinished(5000);
    if (check.exitCode() != 0)
    {
        QMessageBox::warning(this, tr("Container Pull"),
            tr("%1 is not installed or not on your PATH.\n\n"
               "Install %1 and ensure it is accessible from the command line.")
                .arg(runtime));
        return;
    }

    m_containerPullBtn->setEnabled(false);
    m_containerLog->clear();
    m_containerLog->appendPlainText(tr("$ %1 pull %2\n").arg(runtime, tag));

    // Kill any previous process
    if (m_containerProcess)
    {
        m_containerProcess->kill();
        m_containerProcess->deleteLater();
    }

    m_containerProcess = new QProcess(this);
    m_containerProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_containerProcess, &QProcess::readyReadStandardOutput,
            this, [this]() {
        QByteArray data = m_containerProcess->readAll();
        QString text = QString::fromLocal8Bit(data);
        m_containerLog->appendPlainText(text);
        // Auto-scroll to bottom
        auto cursor = m_containerLog->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_containerLog->setTextCursor(cursor);
    });

    connect(m_containerProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, runtime, tag](int exitCode, QProcess::ExitStatus status) {
        m_containerPullBtn->setEnabled(true);
        if (exitCode == 0 && status == QProcess::NormalExit)
        {
            m_containerLog->appendPlainText(tr("\nImage pulled successfully."));
            emit statusMessage(tr("Kali container image pulled: %1").arg(tag));
        }
        else
        {
            m_containerLog->appendPlainText(
                tr("\nPull failed (exit code %1). Check that %2 daemon is running.")
                    .arg(exitCode).arg(runtime));
        }
    });

    connect(m_containerProcess, &QProcess::errorOccurred,
            this, [this, runtime](QProcess::ProcessError error) {
        m_containerPullBtn->setEnabled(true);
        m_containerLog->appendPlainText(
            tr("\nProcess error: could not start %1. Is it installed?").arg(runtime));
    });

    m_containerProcess->start(runtime, {"pull", tag});
}

// ============================================================================
// Cloud Image slots
// ============================================================================

void KaliCreatorTab::onBrowseCloudOutput()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Save Cloud Image"),
        QString(), tr("Disk Images (*.img *.qcow2 *.ova);;All Files (*)"));
    if (!path.isEmpty())
        m_cloudOutputEdit->setText(path);
}

void KaliCreatorTab::onDownloadCloudImage()
{
    QString outputPath = m_cloudOutputEdit->text().trimmed();
    if (outputPath.isEmpty())
    {
        QMessageBox::warning(this, tr("Download Cloud Image"),
            tr("Please specify an output path."));
        return;
    }

    // Build URL based on selected format
    static const char* cloudUrls[] = {
        "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-cloud-genericcloud-amd64.tar.xz",
        "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-cloud-genericcloud-amd64.tar.xz",
        "https://cdimage.kali.org/kali-2024.4/kali-linux-2024.4-cloud-genericcloud-amd64.tar.xz",
    };

    int fmtIdx = m_cloudFormatCombo->currentIndex();
    QUrl url(QString::fromLatin1(cloudUrls[fmtIdx < 3 ? fmtIdx : 0]));

    // For the download, we save the compressed file first, then decompress
    QString downloadFileName = url.fileName();
    QString downloadDir = QFileInfo(outputPath).absolutePath();
    QString downloadPath = downloadDir + "/" + downloadFileName;

    m_cloudDownloadBtn->setEnabled(false);
    m_cloudProgress->setVisible(true);
    m_cloudProgress->setRange(0, 0);
    m_cloudStatusLabel->setText(tr("Downloading Kali cloud image..."));

    disconnect(m_downloader, nullptr, nullptr, nullptr);

    connect(m_downloader, &DownloadManager::progressChanged,
            this, [this](qint64 received, qint64 total) {
        if (total > 0)
        {
            m_cloudProgress->setRange(0, 100);
            int pct = static_cast<int>((received * 100) / total);
            m_cloudProgress->setValue(pct);
            m_cloudStatusLabel->setText(tr("Downloading... %1 / %2")
                .arg(formatSize(static_cast<uint64_t>(received)))
                .arg(formatSize(static_cast<uint64_t>(total))));
        }
    });

    connect(m_downloader, &DownloadManager::downloadError,
            this, [this](const QString& error) {
        m_cloudProgress->setVisible(false);
        m_cloudDownloadBtn->setEnabled(true);
        m_cloudStatusLabel->setText(tr("Download failed: %1").arg(error));
    });

    connect(m_downloader, &DownloadManager::downloadComplete,
            this, [this, outputPath, downloadDir](const QString& filePath) {
        // Check if decompression is needed
        if (Decompressor::isCompressed(filePath))
        {
            m_cloudStatusLabel->setText(tr("Decompressing cloud image..."));
            m_cloudProgress->setRange(0, 0);

            auto* thread = QThread::create([this, filePath, outputPath, downloadDir]() {
                auto result = Decompressor::decompressAuto(filePath, downloadDir,
                    [this](qint64 done, qint64 total) {
                        if (total > 0)
                        {
                            int pct = static_cast<int>((done * 100) / total);
                            QMetaObject::invokeMethod(m_cloudProgress, "setRange",
                                Qt::QueuedConnection, Q_ARG(int, 0), Q_ARG(int, 100));
                            QMetaObject::invokeMethod(m_cloudProgress, "setValue",
                                Qt::QueuedConnection, Q_ARG(int, pct));
                        }
                    });

                QMetaObject::invokeMethod(this, [this, result, outputPath]() {
                    m_cloudProgress->setVisible(false);
                    m_cloudDownloadBtn->setEnabled(true);
                    if (result.isOk())
                    {
                        // If output path differs from decompressed path, rename
                        QString decompPath = result.value();
                        if (decompPath != outputPath && !outputPath.isEmpty())
                        {
                            QFile::remove(outputPath); // remove if exists
                            QFile::rename(decompPath, outputPath);
                        }
                        m_cloudStatusLabel->setText(
                            tr("Cloud image saved to: %1").arg(outputPath));
                        emit statusMessage(tr("Kali cloud image downloaded"));
                    }
                    else
                    {
                        m_cloudStatusLabel->setText(tr("Decompression failed: %1")
                            .arg(QString::fromStdString(result.error().message)));
                    }
                }, Qt::QueuedConnection);
            });

            connect(thread, &QThread::finished, thread, &QThread::deleteLater);
            thread->start();
        }
        else
        {
            // No decompression needed, just rename to target
            if (filePath != outputPath)
            {
                QFile::remove(outputPath);
                QFile::rename(filePath, outputPath);
            }
            m_cloudProgress->setVisible(false);
            m_cloudDownloadBtn->setEnabled(true);
            m_cloudStatusLabel->setText(tr("Cloud image saved to: %1").arg(outputPath));
            emit statusMessage(tr("Kali cloud image downloaded"));
        }
    });

    m_downloader->startDownload(url, downloadPath);
}

} // namespace spw
