#include "MaintenanceTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/disk/RawDiskHandle.h"
#include "core/maintenance/SecureErase.h"
#include "core/maintenance/SdCardRecovery.h"
#include "core/recovery/BootRepair.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

MaintenanceTab::MaintenanceTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

MaintenanceTab::~MaintenanceTab() = default;

void MaintenanceTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    auto* innerTabs = new QTabWidget();

    // ===== Tab 1: Secure Erase =====
    auto* eraseWidget = new QWidget();
    auto* eraseOuterLayout = new QVBoxLayout(eraseWidget);

    auto* eraseGroup = new QGroupBox(tr("Secure Erase"));
    auto* eraseLayout = new QGridLayout(eraseGroup);

    eraseLayout->addWidget(new QLabel(tr("Target Disk:")), 0, 0);
    m_eraseDiskCombo = new QComboBox();
    eraseLayout->addWidget(m_eraseDiskCombo, 0, 1, 1, 2);

    eraseLayout->addWidget(new QLabel(tr("Erase Method:")), 1, 0);
    m_eraseMethodCombo = new QComboBox();
    m_eraseMethodCombo->addItems({
        tr("Zero Fill (1 pass) - Fast"),
        tr("DoD 5220.22-M (3 passes) - Standard"),
        tr("DoD 5220.22-M ECE (7 passes) - Enhanced"),
        tr("Gutmann (35 passes) - Maximum"),
        tr("Random Fill (N passes)"),
        tr("Custom Pattern")
    });
    connect(m_eraseMethodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MaintenanceTab::onEraseMethodChanged);
    eraseLayout->addWidget(m_eraseMethodCombo, 1, 1, 1, 2);

    eraseLayout->addWidget(new QLabel(tr("Custom Passes:")), 2, 0);
    m_customPassSpin = new QSpinBox();
    m_customPassSpin->setRange(1, 99);
    m_customPassSpin->setValue(3);
    m_customPassSpin->setEnabled(false);
    eraseLayout->addWidget(m_customPassSpin, 2, 1);

    m_verifyCheck = new QCheckBox(tr("Verify after erase"));
    m_verifyCheck->setChecked(true);
    eraseLayout->addWidget(m_verifyCheck, 3, 1);

    m_eraseProgress = new QProgressBar();
    m_eraseProgress->setVisible(false);
    eraseLayout->addWidget(m_eraseProgress, 4, 0, 1, 3);

    m_eraseStatusLabel = new QLabel();
    eraseLayout->addWidget(m_eraseStatusLabel, 5, 0, 1, 3);

    // BIG RED erase button
    m_eraseBtn = new QPushButton(tr("SECURE ERASE"));
    m_eraseBtn->setObjectName("cancelButton");
    m_eraseBtn->setStyleSheet(
        "QPushButton { background-color: #cc0000; color: white; font-size: 16px; "
        "font-weight: bold; border: 2px solid #880000; border-radius: 6px; }"
        "QPushButton:hover { background-color: #ee0000; }"
        "QPushButton:pressed { background-color: #aa0000; }");
    m_eraseBtn->setToolTip(tr("WARNING: This permanently destroys ALL data on the selected disk!"));
    connect(m_eraseBtn, &QPushButton::clicked, this, &MaintenanceTab::onSecureErase);
    eraseLayout->addWidget(m_eraseBtn, 6, 0, 1, 3);

    eraseOuterLayout->addWidget(eraseGroup);
    eraseOuterLayout->addStretch();
    innerTabs->addTab(eraseWidget, tr("Secure Erase"));

    // ===== Tab 2: Boot Repair =====
    auto* bootWidget = new QWidget();
    auto* bootOuterLayout = new QVBoxLayout(bootWidget);

    auto* bootGroup = new QGroupBox(tr("Boot Repair"));
    auto* bootLayout = new QVBoxLayout(bootGroup);

    auto* bootInfo = new QLabel(
        tr("Repair boot configuration for Windows and other operating systems."));
    bootLayout->addWidget(bootInfo);

    auto* bootDiskRow = new QHBoxLayout();
    bootDiskRow->addWidget(new QLabel(tr("Target Disk:")));
    m_bootDiskCombo = new QComboBox();
    bootDiskRow->addWidget(m_bootDiskCombo, 1);
    bootLayout->addLayout(bootDiskRow);

    auto* bootBtnLayout = new QHBoxLayout();

    m_mbrRepairBtn = new QPushButton(tr("Repair MBR"));
    m_mbrRepairBtn->setToolTip(tr("Rewrite the Master Boot Record with a standard boot loader"));
    connect(m_mbrRepairBtn, &QPushButton::clicked, this, &MaintenanceTab::onRepairMbr);
    bootBtnLayout->addWidget(m_mbrRepairBtn);

    m_gptRepairBtn = new QPushButton(tr("Repair GPT"));
    m_gptRepairBtn->setToolTip(tr("Rebuild GPT headers and verify partition entries"));
    connect(m_gptRepairBtn, &QPushButton::clicked, this, &MaintenanceTab::onRepairGpt);
    bootBtnLayout->addWidget(m_gptRepairBtn);

    m_bcdRepairBtn = new QPushButton(tr("Repair BCD"));
    m_bcdRepairBtn->setToolTip(tr("Rebuild the Windows Boot Configuration Data store"));
    connect(m_bcdRepairBtn, &QPushButton::clicked, this, &MaintenanceTab::onRepairBcd);
    bootBtnLayout->addWidget(m_bcdRepairBtn);

    m_bootloaderBtn = new QPushButton(tr("Reinstall Bootloader"));
    m_bootloaderBtn->setToolTip(tr("Reinstall the bootloader to the selected disk's boot sector"));
    connect(m_bootloaderBtn, &QPushButton::clicked, this, &MaintenanceTab::onReinstallBootloader);
    bootBtnLayout->addWidget(m_bootloaderBtn);

    bootLayout->addLayout(bootBtnLayout);

    m_bootProgress = new QProgressBar();
    m_bootProgress->setVisible(false);
    bootLayout->addWidget(m_bootProgress);

    m_bootStatusLabel = new QLabel();
    m_bootStatusLabel->setWordWrap(true);
    bootLayout->addWidget(m_bootStatusLabel);

    bootOuterLayout->addWidget(bootGroup);
    bootOuterLayout->addStretch();
    innerTabs->addTab(bootWidget, tr("Boot Repair"));

    // ===== Tab 3: Bootloader Install =====
    auto* blWidget = new QWidget();
    auto* blOuterLayout = new QVBoxLayout(blWidget);

    auto* blGroup = new QGroupBox(tr("Bootloader Installation"));
    auto* blLayout = new QVBoxLayout(blGroup);

    auto* blInfo = new QLabel(
        tr("Install a bootloader to the selected disk or partition. "
           "Requires the target disk to have a valid partition and filesystem. "
           "Run as Administrator."));
    blInfo->setWordWrap(true);
    blLayout->addWidget(blInfo);

    auto* blDiskRow = new QHBoxLayout();
    blDiskRow->addWidget(new QLabel(tr("Target Disk:")));
    m_blDiskCombo = new QComboBox();
    blDiskRow->addWidget(m_blDiskCombo, 1);
    blLayout->addLayout(blDiskRow);

    auto* blPartRow = new QHBoxLayout();
    blPartRow->addWidget(new QLabel(tr("Drive Letter:")));
    m_blPartCombo = new QComboBox();
    // Populate with available drive letters
    for (char c = 'A'; c <= 'Z'; ++c)
    {
        QString path = QString("%1:\\").arg(c);
        if (QDir(path).exists())
            m_blPartCombo->addItem(QString("%1:").arg(c));
    }
    blPartRow->addWidget(m_blPartCombo, 1);
    blLayout->addLayout(blPartRow);

    // Four bootloader buttons in a 2x2 grid
    auto* blBtnGrid = new QGridLayout();

    m_grub2Btn = new QPushButton(tr("Install GRUB2"));
    m_grub2Btn->setToolTip(tr("GNU GRUB 2 — the most common Linux bootloader.\n"
                               "Requires grub-install to be on PATH (from WSL or a GRUB package)."));
    connect(m_grub2Btn, &QPushButton::clicked, this, &MaintenanceTab::onInstallGrub2);
    blBtnGrid->addWidget(m_grub2Btn, 0, 0);

    m_winbmBtn = new QPushButton(tr("Install Windows Boot Manager"));
    m_winbmBtn->setToolTip(tr("Reinstall the Windows Boot Manager using bcdboot.exe.\n"
                               "The selected drive letter should be the Windows partition (usually C:)."));
    connect(m_winbmBtn, &QPushButton::clicked, this, &MaintenanceTab::onInstallWindowsBM);
    blBtnGrid->addWidget(m_winbmBtn, 0, 1);

    m_syslinuxBtn = new QPushButton(tr("Install SYSLINUX"));
    m_syslinuxBtn->setToolTip(tr("SYSLINUX — lightweight bootloader for FAT/FAT32 partitions.\n"
                                  "Used for bootable USB drives and rescue media.\n"
                                  "Requires syslinux.exe on PATH."));
    connect(m_syslinuxBtn, &QPushButton::clicked, this, &MaintenanceTab::onInstallSyslinux);
    blBtnGrid->addWidget(m_syslinuxBtn, 1, 0);

    m_refindBtn = new QPushButton(tr("Install rEFInd"));
    m_refindBtn->setToolTip(tr("rEFInd — graphical UEFI boot manager that auto-detects bootloaders.\n"
                                "Great for dual-boot and recovery setups.\n"
                                "Requires refind-install or the rEFInd binaries to be on PATH."));
    connect(m_refindBtn, &QPushButton::clicked, this, &MaintenanceTab::onInstallRefind);
    blBtnGrid->addWidget(m_refindBtn, 1, 1);

    blLayout->addLayout(blBtnGrid);

    m_blProgress = new QProgressBar();
    m_blProgress->setRange(0, 0); // Indeterminate
    m_blProgress->setVisible(false);
    blLayout->addWidget(m_blProgress);

    m_blStatusLabel = new QLabel();
    m_blStatusLabel->setWordWrap(true);
    blLayout->addWidget(m_blStatusLabel);

    blOuterLayout->addWidget(blGroup);
    blOuterLayout->addStretch();
    innerTabs->addTab(blWidget, tr("Bootloader Install"));

    // ===== Tab 4: SD Card Recovery =====
    auto* sdWidget = new QWidget();
    auto* sdOuterLayout = new QVBoxLayout(sdWidget);

    auto* sdGroup = new QGroupBox(tr("SD Card Recovery"));
    auto* sdLayout = new QGridLayout(sdGroup);

    auto* sdInfo = new QLabel(
        tr("Detect and fix SD/microSD cards that Windows cannot see.\n"
           "Repairs corrupted partition tables from interrupted formats, "
           "RAW cards, and uninitialized media."));
    sdInfo->setWordWrap(true);
    sdLayout->addWidget(sdInfo, 0, 0, 1, 3);

    m_sdScanBtn = new QPushButton(tr("Scan for SD Cards"));
    m_sdScanBtn->setToolTip(tr("Scan all disk interfaces for SD/MMC cards, including invisible ones"));
    connect(m_sdScanBtn, &QPushButton::clicked, this, &MaintenanceTab::onSdScan);
    sdLayout->addWidget(m_sdScanBtn, 1, 0);

    m_sdCardCombo = new QComboBox();
    sdLayout->addWidget(m_sdCardCombo, 1, 1, 1, 2);

    sdLayout->addWidget(new QLabel(tr("Format As:")), 2, 0);
    m_sdFsCombo = new QComboBox();
    m_sdFsCombo->addItems({
        tr("FAT32 (recommended for <= 32 GB)"),
        tr("exFAT (recommended for > 32 GB)"),
        tr("NTFS")
    });
    sdLayout->addWidget(m_sdFsCombo, 2, 1, 1, 2);

    sdLayout->addWidget(new QLabel(tr("Volume Label:")), 3, 0);
    m_sdLabelEdit = new QLineEdit(QStringLiteral("SD_CARD"));
    m_sdLabelEdit->setMaxLength(11);
    sdLayout->addWidget(m_sdLabelEdit, 3, 1, 1, 2);

    m_sdFixBtn = new QPushButton(tr("Fix SD Card"));
    m_sdFixBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-size: 14px; "
        "font-weight: bold; border: 2px solid #b08050; border-radius: 6px; }"
        "QPushButton:hover { background-color: #e0b584; }"
        "QPushButton:pressed { background-color: #c49060; }");
    m_sdFixBtn->setEnabled(false);
    connect(m_sdFixBtn, &QPushButton::clicked, this, &MaintenanceTab::onSdFix);
    sdLayout->addWidget(m_sdFixBtn, 4, 0, 1, 3);

    m_sdProgress = new QProgressBar();
    m_sdProgress->setVisible(false);
    sdLayout->addWidget(m_sdProgress, 5, 0, 1, 3);

    m_sdStatusLabel = new QLabel();
    m_sdStatusLabel->setWordWrap(true);
    sdLayout->addWidget(m_sdStatusLabel, 6, 0, 1, 3);

    sdOuterLayout->addWidget(sdGroup);
    sdOuterLayout->addStretch();
    innerTabs->addTab(sdWidget, tr("SD Card Recovery"));

    layout->addWidget(innerTabs);
}

void MaintenanceTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombo();
}

void MaintenanceTab::populateDiskCombo()
{
    m_eraseDiskCombo->clear();
    m_bootDiskCombo->clear();
    m_blDiskCombo->clear();

    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));
        m_eraseDiskCombo->addItem(label, disk.id);
        m_bootDiskCombo->addItem(label, disk.id);
        m_blDiskCombo->addItem(label, disk.id);
    }
}

void MaintenanceTab::onEraseMethodChanged()
{
    int idx = m_eraseMethodCombo->currentIndex();
    // Enable custom pass count for Random Fill (4) and Custom Pattern (5)
    m_customPassSpin->setEnabled(idx == 4 || idx == 5);
}

void MaintenanceTab::onSecureErase()
{
    int diskId = m_eraseDiskCombo->currentData().toInt();

    // Find disk name for confirmation
    QString diskName;
    for (const auto& disk : m_snapshot.disks)
    {
        if (disk.id == diskId)
        {
            diskName = QString::fromStdWString(disk.model);
            break;
        }
    }

    // First confirmation
    auto reply = QMessageBox::critical(this, tr("SECURE ERASE - CONFIRM"),
                                       tr("You are about to PERMANENTLY DESTROY all data on:\n\n"
                                          "Disk %1: %2\n\n"
                                          "This action is IRREVERSIBLE.\n\n"
                                          "Are you absolutely sure?")
                                           .arg(diskId).arg(diskName),
                                       QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    // Second confirmation: type disk name
    bool ok = false;
    QString typedName = QInputDialog::getText(this, tr("Final Confirmation"),
                                              tr("Type the disk model name to confirm:\n\n%1")
                                                  .arg(diskName),
                                              QLineEdit::Normal, QString(), &ok);
    if (!ok || typedName.trimmed() != diskName.trimmed())
    {
        QMessageBox::information(this, tr("Cancelled"),
                                 tr("Erase cancelled. The disk name did not match."));
        return;
    }

    // Build erase config
    EraseConfig config;
    switch (m_eraseMethodCombo->currentIndex())
    {
    case 0: config.method = EraseMethod::ZeroFill; break;
    case 1: config.method = EraseMethod::DoD_3Pass; break;
    case 2: config.method = EraseMethod::DoD_7Pass; break;
    case 3: config.method = EraseMethod::Gutmann; break;
    case 4:
        config.method = EraseMethod::RandomFill;
        config.passCount = m_customPassSpin->value();
        break;
    case 5:
        config.method = EraseMethod::CustomPattern;
        config.customPatternPasses = m_customPassSpin->value();
        config.customPattern = {0xAA, 0x55}; // Default alternating pattern
        break;
    }
    config.verify = m_verifyCheck->isChecked();

    m_cancelFlag.store(false);
    m_eraseProgress->setVisible(true);
    m_eraseProgress->setValue(0);
    m_eraseBtn->setEnabled(false);
    m_eraseStatusLabel->setText(tr("Erasing..."));

    auto* thread = QThread::create([this, diskId, config]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_eraseStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed to open disk: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        SecureErase erase(disk);

        auto result = erase.eraseDisk(config,
            [this](int currentPass, int totalPasses,
                   uint64_t bytesWritten, uint64_t totalBytes, double speedMBps) {
                int pct = totalBytes > 0 ? static_cast<int>((bytesWritten * 100) / totalBytes) : 0;
                QMetaObject::invokeMethod(m_eraseProgress, "setValue",
                                          Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_eraseStatusLabel, "setText",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, tr("Pass %1/%2 - %3 MB/s")
                                                             .arg(currentPass)
                                                             .arg(totalPasses)
                                                             .arg(speedMBps, 0, 'f', 1)));
            },
            &m_cancelFlag);

        QString resultMsg = result.isOk() ? tr("Erase completed successfully.")
                                           : tr("Erase failed: %1")
                                                 .arg(QString::fromStdString(result.error().message));
        QMetaObject::invokeMethod(m_eraseStatusLabel, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, resultMsg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_eraseProgress->setVisible(false);
        m_eraseBtn->setEnabled(true);
        emit statusMessage(tr("Secure erase completed"));
    });

    thread->start();
}

void MaintenanceTab::onRepairMbr()
{
    int diskId = m_bootDiskCombo->currentData().toInt();

    auto reply = QMessageBox::warning(this, tr("Repair MBR"),
                                      tr("This will rewrite the MBR boot code on Disk %1.\n"
                                         "Partition table entries will be preserved.\n\nContinue?")
                                          .arg(diskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_bootProgress->setVisible(true);
    m_bootProgress->setRange(0, 0);
    m_bootStatusLabel->setText(tr("Repairing MBR..."));

    auto* thread = QThread::create([this, diskId]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        BootRepair repair(disk);
        auto result = repair.repairMbr();

        QString msg = result.isOk() ? tr("MBR repaired successfully.")
                                     : tr("MBR repair failed: %1")
                                           .arg(QString::fromStdString(result.error().message));
        QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_bootProgress->setVisible(false);
        emit statusMessage(tr("MBR repair completed"));
    });

    thread->start();
}

void MaintenanceTab::onRepairGpt()
{
    int diskId = m_bootDiskCombo->currentData().toInt();

    auto reply = QMessageBox::warning(this, tr("Repair GPT"),
                                      tr("This will rebuild GPT headers on Disk %1.\n\nContinue?")
                                          .arg(diskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_bootProgress->setVisible(true);
    m_bootProgress->setRange(0, 0);
    m_bootStatusLabel->setText(tr("Repairing GPT..."));

    auto* thread = QThread::create([this, diskId]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        BootRepair repair(disk);
        auto result = repair.repairGpt(true); // Rebuild primary from backup

        QString msg = result.isOk() ? tr("GPT repaired successfully.")
                                     : tr("GPT repair failed: %1")
                                           .arg(QString::fromStdString(result.error().message));
        QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_bootProgress->setVisible(false);
        emit statusMessage(tr("GPT repair completed"));
    });

    thread->start();
}

void MaintenanceTab::onRepairBcd()
{
    int diskId = m_bootDiskCombo->currentData().toInt();
    Q_UNUSED(diskId);

    auto reply = QMessageBox::warning(this, tr("Repair BCD"),
                                      tr("This will rebuild the Windows Boot Configuration Data.\n\nContinue?"),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_bootProgress->setVisible(true);
    m_bootProgress->setRange(0, 0);
    m_bootStatusLabel->setText(tr("Repairing BCD..."));

    auto* thread = QThread::create([this, diskId]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        BootRepair repair(disk);
        auto result = repair.repairBcd(L'S'); // Assume S: is the ESP

        QString msg = result.isOk() ? tr("BCD repaired successfully.")
                                     : tr("BCD repair failed: %1")
                                           .arg(QString::fromStdString(result.error().message));
        QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_bootProgress->setVisible(false);
        emit statusMessage(tr("BCD repair completed"));
    });

    thread->start();
}

void MaintenanceTab::onReinstallBootloader()
{
    int diskId = m_bootDiskCombo->currentData().toInt();

    auto reply = QMessageBox::warning(this, tr("Reinstall Bootloader"),
                                      tr("This will reinstall the Windows bootloader on Disk %1.\n\nContinue?")
                                          .arg(diskId),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    m_bootProgress->setVisible(true);
    m_bootProgress->setRange(0, 0);
    m_bootStatusLabel->setText(tr("Reinstalling bootloader..."));

    auto* thread = QThread::create([this, diskId]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        BootRepair repair(disk);
        auto result = repair.repairBootloader(L'S', L'C');

        QString msg = result.isOk() ? tr("Bootloader reinstalled successfully.")
                                     : tr("Bootloader reinstall failed: %1")
                                           .arg(QString::fromStdString(result.error().message));
        QMetaObject::invokeMethod(m_bootStatusLabel, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_bootProgress->setVisible(false);
        emit statusMessage(tr("Bootloader reinstall completed"));
    });

    thread->start();
}

void MaintenanceTab::onSdScan()
{
    m_sdScanBtn->setEnabled(false);
    m_sdStatusLabel->setText(tr("Scanning for SD cards..."));
    m_sdCardCombo->clear();
    m_detectedCards.clear();
    m_sdFixBtn->setEnabled(false);

    auto* thread = QThread::create([this]() {
        auto result = SdCardRecovery::detectSdCards();
        if (result.isError())
        {
            QMetaObject::invokeMethod(m_sdStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Scan failed: %1")
                                                         .arg(QString::fromStdString(result.error().message))));
            return;
        }
        m_detectedCards = std::move(result.value());
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_sdScanBtn->setEnabled(true);

        if (m_detectedCards.empty())
        {
            m_sdStatusLabel->setText(
                tr("No SD/MMC cards detected.\n"
                   "Make sure the card is inserted in a reader and the reader is connected."));
            return;
        }

        for (const auto& card : m_detectedCards)
        {
            QString statusStr;
            switch (card.status)
            {
            case SdCardStatus::Healthy:          statusStr = tr("Healthy"); break;
            case SdCardStatus::NoPartitionTable:  statusStr = tr("NO PARTITION TABLE"); break;
            case SdCardStatus::CorruptPartition:  statusStr = tr("CORRUPT"); break;
            case SdCardStatus::RawFilesystem:     statusStr = tr("RAW"); break;
            case SdCardStatus::NoMedia:           statusStr = tr("No Media"); break;
            default:                              statusStr = tr("Unknown"); break;
            }

            QString label = QString("Disk %1: %2 (%3) [%4]")
                                .arg(card.diskId)
                                .arg(QString::fromStdWString(card.model))
                                .arg(formatSize(card.sizeBytes))
                                .arg(statusStr);
            m_sdCardCombo->addItem(label, card.diskId);
        }

        m_sdFixBtn->setEnabled(true);
        m_sdStatusLabel->setText(
            tr("Found %1 SD card(s). Select one and click Fix to repair.")
                .arg(m_detectedCards.size()));

        emit statusMessage(tr("SD card scan complete — %1 card(s) found")
                               .arg(m_detectedCards.size()));
    });

    thread->start();
}

void MaintenanceTab::onSdFix()
{
    int diskId = m_sdCardCombo->currentData().toInt();

    // Find the card info
    const SdCardInfo* cardInfo = nullptr;
    for (const auto& card : m_detectedCards)
    {
        if (card.diskId == diskId)
        {
            cardInfo = &card;
            break;
        }
    }
    if (!cardInfo)
        return;

    // Confirmation
    auto reply = QMessageBox::warning(this, tr("Fix SD Card"),
                                       tr("This will ERASE ALL DATA on:\n\n"
                                          "Disk %1: %2 (%3)\n\n"
                                          "The card will be cleaned, repartitioned, and formatted.\n\n"
                                          "Continue?")
                                           .arg(diskId)
                                           .arg(QString::fromStdWString(cardInfo->model))
                                           .arg(formatSize(cardInfo->sizeBytes)),
                                       QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    // Build config
    SdFixConfig config;
    config.action = SdFixAction::CleanAndFormat;
    switch (m_sdFsCombo->currentIndex())
    {
    case 0: config.targetFs = FilesystemType::FAT32; break;
    case 1: config.targetFs = FilesystemType::ExFAT; break;
    case 2: config.targetFs = FilesystemType::NTFS; break;
    }
    config.volumeLabel = m_sdLabelEdit->text().toStdWString();

    m_sdFixBtn->setEnabled(false);
    m_sdScanBtn->setEnabled(false);
    m_sdProgress->setVisible(true);
    m_sdProgress->setValue(0);
    m_sdStatusLabel->setText(tr("Fixing SD card..."));

    auto* thread = QThread::create([this, diskId, config]() {
        auto result = SdCardRecovery::fixCard(diskId, config,
            [this](const std::string& stage, int pct) {
                QMetaObject::invokeMethod(m_sdProgress, "setValue",
                                          Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_sdStatusLabel, "setText",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, QString::fromStdString(stage)));
            });

        if (result.isError())
        {
            QMetaObject::invokeMethod(m_sdStatusLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Fix failed: %1")
                                                         .arg(QString::fromStdString(result.error().message))));
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_sdProgress->setVisible(false);
        m_sdFixBtn->setEnabled(true);
        m_sdScanBtn->setEnabled(true);
        emit statusMessage(tr("SD card fix completed"));
    });

    thread->start();
}

// ============================================================================
// Bootloader installation helpers
// ============================================================================

// Run a command invisibly and return {stdout+stderr, exitCode}
static std::pair<QString, int> runCommand(const QString& program, const QStringList& args)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(5000))
        return {QString("Failed to start: %1").arg(program), -1};
    proc.waitForFinished(120000); // 2 min max
    return {QString::fromLocal8Bit(proc.readAll()), proc.exitCode()};
}

void MaintenanceTab::onInstallGrub2()
{
    int diskId = m_blDiskCombo->currentData().toInt();
    QString driveLetter = m_blPartCombo->currentText().left(1);

    auto reply = QMessageBox::question(this, tr("Install GRUB2"),
        tr("Install GNU GRUB 2 to Disk %1?\n\n"
           "This will write GRUB2 boot code to the MBR and install\n"
           "GRUB modules to the %2: partition.\n\n"
           "Requirements: grub-install must be on PATH (from WSL2 or Cygwin).\n\n"
           "Continue?").arg(diskId).arg(driveLetter),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_blProgress->setVisible(true);
    m_blStatusLabel->setText(tr("Installing GRUB2..."));
    m_grub2Btn->setEnabled(false);

    auto* thread = QThread::create([this, diskId, driveLetter]() {
        // Try grub-install via WSL path first, then native if available
        QString diskPath = QString("\\\\.\\PhysicalDrive%1").arg(diskId);
        QString mountPoint = QString("/mnt/%1").arg(driveLetter.toLower());

        // WSL path: grub-install through wsl.exe
        auto [wslOut, wslCode] = runCommand("wsl.exe",
            {"grub-install", "--target=i386-pc",
             QString("--boot-directory=%1/boot").arg(mountPoint),
             QString("/dev/sd%1").arg(char('a' + diskId))});

        QString msg;
        if (wslCode == 0)
            msg = tr("✓ GRUB2 installed successfully via WSL.\n") + wslOut;
        else
        {
            // Try native grub-install
            auto [natOut, natCode] = runCommand("grub-install",
                {"--target=i386-pc",
                 QString("--boot-directory=%1:\\boot").arg(driveLetter),
                 QString("\\\\.\\PhysicalDrive%1").arg(diskId)});
            if (natCode == 0)
                msg = tr("✓ GRUB2 installed successfully.\n") + natOut;
            else
                msg = tr("✗ GRUB2 install failed.\n\n"
                         "Make sure grub-install is installed (WSL2 recommended).\n"
                         "WSL output:\n") + wslOut + "\n\nNative output:\n" + natOut;
        }

        QMetaObject::invokeMethod(m_blStatusLabel, "setText",
            Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_blProgress->setVisible(false);
        m_grub2Btn->setEnabled(true);
        emit statusMessage(tr("GRUB2 install complete"));
    });
    thread->start();
}

void MaintenanceTab::onInstallWindowsBM()
{
    QString driveLetter = m_blPartCombo->currentText().left(1);

    auto reply = QMessageBox::question(this, tr("Install Windows Boot Manager"),
        tr("Reinstall the Windows Boot Manager to %1:?\n\n"
           "This runs: bcdboot %2:\\Windows /s %2:\n\n"
           "The selected drive should be your Windows partition (usually C:).\n\n"
           "Continue?").arg(driveLetter).arg(driveLetter),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_blProgress->setVisible(true);
    m_blStatusLabel->setText(tr("Installing Windows Boot Manager..."));
    m_winbmBtn->setEnabled(false);

    auto* thread = QThread::create([this, driveLetter]() {
        // bcdboot copies boot files and rebuilds BCD
        QString windowsDir = QString("%1:\\Windows").arg(driveLetter);
        auto [out, code] = runCommand("bcdboot.exe",
            {windowsDir, "/s", QString("%1:").arg(driveLetter), "/f", "ALL"});

        QString msg = (code == 0)
            ? tr("✓ Windows Boot Manager installed successfully.\n") + out
            : tr("✗ bcdboot failed (exit %1).\n").arg(code) + out +
              tr("\n\nEnsure the drive contains a valid Windows installation.");

        QMetaObject::invokeMethod(m_blStatusLabel, "setText",
            Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_blProgress->setVisible(false);
        m_winbmBtn->setEnabled(true);
        emit statusMessage(tr("Windows Boot Manager install complete"));
    });
    thread->start();
}

void MaintenanceTab::onInstallSyslinux()
{
    QString driveLetter = m_blPartCombo->currentText().left(1);

    auto reply = QMessageBox::question(this, tr("Install SYSLINUX"),
        tr("Install SYSLINUX to %1:?\n\n"
           "SYSLINUX is a lightweight bootloader for FAT/FAT32 partitions.\n"
           "It is commonly used for bootable USB drives and rescue media.\n\n"
           "Requirements: syslinux.exe must be on PATH or in the app directory.\n\n"
           "Continue?").arg(driveLetter),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_blProgress->setVisible(true);
    m_blStatusLabel->setText(tr("Installing SYSLINUX..."));
    m_syslinuxBtn->setEnabled(false);

    auto* thread = QThread::create([this, driveLetter]() {
        // syslinux -m -a X:  installs MBR + marks partition active
        auto [out, code] = runCommand("syslinux.exe",
            {"-m", "-a", QString("%1:").arg(driveLetter)});

        QString msg = (code == 0)
            ? tr("✓ SYSLINUX installed to %1:.\n").arg(driveLetter) + out
            : tr("✗ SYSLINUX install failed (exit %1).\n").arg(code) + out +
              tr("\n\nEnsure syslinux.exe is available (download from syslinux.org) "
                 "and the partition is FAT/FAT32.");

        QMetaObject::invokeMethod(m_blStatusLabel, "setText",
            Qt::QueuedConnection, Q_ARG(QString, msg));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_blProgress->setVisible(false);
        m_syslinuxBtn->setEnabled(true);
        emit statusMessage(tr("SYSLINUX install complete"));
    });
    thread->start();
}

void MaintenanceTab::onInstallRefind()
{
    QString driveLetter = m_blPartCombo->currentText().left(1);

    auto reply = QMessageBox::question(this, tr("Install rEFInd"),
        tr("Install rEFInd EFI boot manager to %1:?\n\n"
           "rEFInd is a graphical UEFI boot manager that automatically detects\n"
           "installed operating systems and bootloaders.\n\n"
           "Requirements:\n"
           "  • The partition must be your EFI System Partition (ESP)\n"
           "  • refind-install must be on PATH, OR the rEFInd binaries\n"
           "    (refind_x64.efi, etc.) must be present in the app directory.\n\n"
           "Continue?").arg(driveLetter),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_blProgress->setVisible(true);
    m_blStatusLabel->setText(tr("Installing rEFInd..."));
    m_refindBtn->setEnabled(false);

    auto* thread = QThread::create([this, driveLetter]() {
        QString efiDir = QString("%1:\\EFI\\refind").arg(driveLetter);

        // Try refind-install first
        auto [out1, code1] = runCommand("refind-install",
            {"--usedefault", QString("%1:\\").arg(driveLetter)});

        if (code1 == 0)
        {
            QMetaObject::invokeMethod(m_blStatusLabel, "setText",
                Qt::QueuedConnection,
                Q_ARG(QString, tr("✓ rEFInd installed via refind-install.\n") + out1));
            return;
        }

        // Manual copy fallback: look for refind_x64.efi next to our exe
        QString exeDir = QCoreApplication::applicationDirPath();
        QString refindEfi = exeDir + "/refind_x64.efi";
        if (QFile::exists(refindEfi))
        {
            QDir().mkpath(efiDir);
            bool ok = QFile::copy(refindEfi, efiDir + "/refind_x64.efi");
            // Also copy config if present
            QString refindConf = exeDir + "/refind.conf";
            if (QFile::exists(refindConf))
                QFile::copy(refindConf, efiDir + "/refind.conf");

            QString msg = ok
                ? tr("✓ rEFInd EFI binary copied to %1.\n"
                     "You may need to register it with your UEFI using efibootmgr or bcdedit.").arg(efiDir)
                : tr("✗ Failed to copy rEFInd to %1.").arg(efiDir);
            QMetaObject::invokeMethod(m_blStatusLabel, "setText",
                Qt::QueuedConnection, Q_ARG(QString, msg));
        }
        else
        {
            QMetaObject::invokeMethod(m_blStatusLabel, "setText",
                Qt::QueuedConnection,
                Q_ARG(QString,
                    tr("✗ rEFInd not found.\n\n"
                       "Download rEFInd from www.rodsbooks.com/refind/ and place\n"
                       "refind_x64.efi next to SetecPartitionWizard.exe, then retry.\n\n"
                       "refind-install output:\n") + out1));
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_blProgress->setVisible(false);
        m_refindBtn->setEnabled(true);
        emit statusMessage(tr("rEFInd install complete"));
    });
    thread->start();
}

QString MaintenanceTab::formatSize(uint64_t bytes)
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
