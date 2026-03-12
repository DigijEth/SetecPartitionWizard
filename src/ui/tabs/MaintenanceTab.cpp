#include "MaintenanceTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/disk/RawDiskHandle.h"
#include "core/maintenance/SecureErase.h"
#include "core/recovery/BootRepair.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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

    // ===== Secure Erase Section =====
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
    m_eraseBtn->setMinimumHeight(50);
    m_eraseBtn->setStyleSheet(
        "QPushButton { background-color: #cc0000; color: white; font-size: 16px; "
        "font-weight: bold; border: 2px solid #880000; border-radius: 6px; }"
        "QPushButton:hover { background-color: #ee0000; }"
        "QPushButton:pressed { background-color: #aa0000; }");
    m_eraseBtn->setToolTip(tr("WARNING: This permanently destroys ALL data on the selected disk!"));
    connect(m_eraseBtn, &QPushButton::clicked, this, &MaintenanceTab::onSecureErase);
    eraseLayout->addWidget(m_eraseBtn, 6, 0, 1, 3);

    layout->addWidget(eraseGroup);

    // ===== Boot Repair Section =====
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

    layout->addWidget(bootGroup);
    layout->addStretch();
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

    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));
        m_eraseDiskCombo->addItem(label, disk.id);
        m_bootDiskCombo->addItem(label, disk.id);
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
