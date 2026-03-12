#include "RecoveryTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/disk/RawDiskHandle.h"
#include "core/recovery/PartitionRecovery.h"
#include "core/recovery/FileRecovery.h"
#include "core/recovery/BootRepair.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

RecoveryTab::RecoveryTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

RecoveryTab::~RecoveryTab() = default;

void RecoveryTab::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal);

    // Left: options panel
    auto* optionsPanel = new QWidget();
    auto* optLayout = new QVBoxLayout(optionsPanel);

    // Target disk selector
    auto* targetGroup = new QGroupBox(tr("Target Disk"));
    auto* targetLayout = new QVBoxLayout(targetGroup);
    m_diskCombo = new QComboBox();
    targetLayout->addWidget(m_diskCombo);
    optLayout->addWidget(targetGroup);

    // Recovery type buttons
    auto* typeGroup = new QGroupBox(tr("Recovery Type"));
    auto* typeLayout = new QVBoxLayout(typeGroup);
    m_partRecoveryBtn = new QPushButton(tr("Partition Recovery"));
    m_partRecoveryBtn->setCheckable(true);
    m_partRecoveryBtn->setChecked(true);
    m_fileRecoveryBtn = new QPushButton(tr("File Recovery"));
    m_fileRecoveryBtn->setCheckable(true);
    m_bootRepairBtn = new QPushButton(tr("Boot Repair"));
    m_bootRepairBtn->setCheckable(true);

    typeLayout->addWidget(m_partRecoveryBtn);
    typeLayout->addWidget(m_fileRecoveryBtn);
    typeLayout->addWidget(m_bootRepairBtn);
    optLayout->addWidget(typeGroup);

    connect(m_partRecoveryBtn, &QPushButton::clicked, this, &RecoveryTab::onRecoveryTypeChanged);
    connect(m_fileRecoveryBtn, &QPushButton::clicked, this, &RecoveryTab::onRecoveryTypeChanged);
    connect(m_bootRepairBtn, &QPushButton::clicked, this, &RecoveryTab::onRecoveryTypeChanged);

    optLayout->addStretch();
    splitter->addWidget(optionsPanel);

    // Right: stacked pages
    m_stackedWidget = new QStackedWidget();

    setupPartitionRecoveryPage();
    setupFileRecoveryPage();
    setupBootRepairPage();

    splitter->addWidget(m_stackedWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    layout->addWidget(splitter);
}

void RecoveryTab::setupPartitionRecoveryPage()
{
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* scanGroup = new QGroupBox(tr("Scan Options"));
    auto* scanLayout = new QVBoxLayout(scanGroup);

    m_quickScanRadio = new QRadioButton(tr("Quick Scan (1 MiB boundaries)"));
    m_quickScanRadio->setChecked(true);
    m_deepScanRadio = new QRadioButton(tr("Deep Scan (every sector)"));
    scanLayout->addWidget(m_quickScanRadio);
    scanLayout->addWidget(m_deepScanRadio);

    m_partScanBtn = new QPushButton(tr("Start Scan"));
    m_partScanBtn->setObjectName("applyButton");
    scanLayout->addWidget(m_partScanBtn);
    connect(m_partScanBtn, &QPushButton::clicked, this, &RecoveryTab::onStartPartitionScan);

    m_partScanProgress = new QProgressBar();
    m_partScanProgress->setVisible(false);
    scanLayout->addWidget(m_partScanProgress);

    layout->addWidget(scanGroup);

    // Results table
    auto* resLabel = new QLabel(tr("Recovered Partitions"));
    resLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    layout->addWidget(resLabel);

    m_partResultsTable = new QTableWidget(0, 6);
    m_partResultsTable->setHorizontalHeaderLabels(
        {tr("Start LBA"), tr("Size"), tr("Filesystem"), tr("Label"), tr("Confidence"), tr("Overlaps")});
    m_partResultsTable->setAlternatingRowColors(true);
    m_partResultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partResultsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_partResultsTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_partResultsTable);

    m_recoverPartBtn = new QPushButton(tr("Recover Selected"));
    m_recoverPartBtn->setObjectName("applyButton");
    m_recoverPartBtn->setEnabled(false);
    connect(m_recoverPartBtn, &QPushButton::clicked, this, &RecoveryTab::onRecoverSelectedPartitions);
    layout->addWidget(m_recoverPartBtn);

    m_stackedWidget->addWidget(page);
}

void RecoveryTab::setupFileRecoveryPage()
{
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* optGroup = new QGroupBox(tr("File Recovery Options"));
    auto* optLayout = new QVBoxLayout(optGroup);

    auto* partRow = new QHBoxLayout();
    partRow->addWidget(new QLabel(tr("Partition:")));
    m_partitionCombo = new QComboBox();
    partRow->addWidget(m_partitionCombo, 1);
    optLayout->addLayout(partRow);

    auto* typeRow = new QHBoxLayout();
    typeRow->addWidget(new QLabel(tr("File Types:")));
    m_fileTypeFilter = new QComboBox();
    m_fileTypeFilter->addItems({tr("All Files"), tr("Images (JPG, PNG, BMP, GIF)"),
                                tr("Documents (PDF, DOC, XLS)"), tr("Archives (ZIP, RAR, 7Z)"),
                                tr("Media (MP3, MP4, AVI)")});
    typeRow->addWidget(m_fileTypeFilter, 1);
    optLayout->addLayout(typeRow);

    m_fileScanBtn = new QPushButton(tr("Scan for Files"));
    m_fileScanBtn->setObjectName("applyButton");
    connect(m_fileScanBtn, &QPushButton::clicked, this, &RecoveryTab::onStartFileRecoveryScan);
    optLayout->addWidget(m_fileScanBtn);

    m_fileScanProgress = new QProgressBar();
    m_fileScanProgress->setVisible(false);
    optLayout->addWidget(m_fileScanProgress);

    layout->addWidget(optGroup);

    // Results
    auto* resLabel = new QLabel(tr("Recoverable Files"));
    resLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    layout->addWidget(resLabel);

    m_fileResultsTable = new QTableWidget(0, 5);
    m_fileResultsTable->setHorizontalHeaderLabels(
        {tr("Filename"), tr("Size"), tr("Type"), tr("Confidence"), tr("Source FS")});
    m_fileResultsTable->setAlternatingRowColors(true);
    m_fileResultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileResultsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileResultsTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_fileResultsTable);

    // Output folder
    auto* outRow = new QHBoxLayout();
    outRow->addWidget(new QLabel(tr("Output Folder:")));
    m_outputFolderEdit = new QLineEdit();
    outRow->addWidget(m_outputFolderEdit, 1);
    m_browseOutputBtn = new QPushButton(tr("Browse..."));
    connect(m_browseOutputBtn, &QPushButton::clicked, this, &RecoveryTab::onBrowseOutputFolder);
    outRow->addWidget(m_browseOutputBtn);
    layout->addLayout(outRow);

    m_recoverFileBtn = new QPushButton(tr("Recover Selected Files"));
    m_recoverFileBtn->setObjectName("applyButton");
    m_recoverFileBtn->setEnabled(false);
    connect(m_recoverFileBtn, &QPushButton::clicked, this, &RecoveryTab::onRecoverSelectedFiles);
    layout->addWidget(m_recoverFileBtn);

    m_stackedWidget->addWidget(page);
}

void RecoveryTab::setupBootRepairPage()
{
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* optGroup = new QGroupBox(tr("Boot Repair Options"));
    auto* optLayout = new QVBoxLayout(optGroup);

    m_repairMbr = new QCheckBox(tr("Repair MBR boot code"));
    m_repairGpt = new QCheckBox(tr("Repair GPT headers"));
    m_repairBootSector = new QCheckBox(tr("Restore backup boot sector (NTFS/FAT)"));
    m_repairBcd = new QCheckBox(tr("Rebuild Windows BCD store"));
    m_repairBootloader = new QCheckBox(tr("Reinstall Windows bootloader"));

    optLayout->addWidget(m_repairMbr);
    optLayout->addWidget(m_repairGpt);
    optLayout->addWidget(m_repairBootSector);
    optLayout->addWidget(m_repairBcd);
    optLayout->addWidget(m_repairBootloader);
    layout->addWidget(optGroup);

    m_bootRepairStartBtn = new QPushButton(tr("Start Repair"));
    m_bootRepairStartBtn->setObjectName("applyButton");
    connect(m_bootRepairStartBtn, &QPushButton::clicked, this, &RecoveryTab::onStartBootRepair);
    layout->addWidget(m_bootRepairStartBtn);

    m_bootRepairProgress = new QProgressBar();
    m_bootRepairProgress->setVisible(false);
    layout->addWidget(m_bootRepairProgress);

    m_bootRepairStatus = new QLabel();
    m_bootRepairStatus->setWordWrap(true);
    layout->addWidget(m_bootRepairStatus);

    layout->addStretch();
    m_stackedWidget->addWidget(page);
}

void RecoveryTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombo();
    populatePartitionCombo();
}

void RecoveryTab::populateDiskCombo()
{
    m_diskCombo->clear();
    for (const auto& disk : m_snapshot.disks)
    {
        QString label = QString("Disk %1: %2 (%3)")
                            .arg(disk.id)
                            .arg(QString::fromStdWString(disk.model))
                            .arg(formatSize(disk.sizeBytes));
        m_diskCombo->addItem(label, disk.id);
    }
}

void RecoveryTab::populatePartitionCombo()
{
    m_partitionCombo->clear();
    for (const auto& part : m_snapshot.partitions)
    {
        QString label;
        if (part.driveLetter != L'\0')
            label = QString("(%1:) ").arg(QChar(part.driveLetter));
        label += QString("Disk %1, Partition %2 - %3")
                     .arg(part.diskId)
                     .arg(part.index)
                     .arg(formatSize(part.sizeBytes));
        m_partitionCombo->addItem(label, QVariant::fromValue(static_cast<int>(part.index)));
    }
}

void RecoveryTab::onRecoveryTypeChanged()
{
    auto* sender = qobject_cast<QPushButton*>(this->sender());

    m_partRecoveryBtn->setChecked(sender == m_partRecoveryBtn);
    m_fileRecoveryBtn->setChecked(sender == m_fileRecoveryBtn);
    m_bootRepairBtn->setChecked(sender == m_bootRepairBtn);

    if (sender == m_partRecoveryBtn)
        m_stackedWidget->setCurrentIndex(0);
    else if (sender == m_fileRecoveryBtn)
        m_stackedWidget->setCurrentIndex(1);
    else if (sender == m_bootRepairBtn)
        m_stackedWidget->setCurrentIndex(2);
}

void RecoveryTab::onStartPartitionScan()
{
    int diskIdx = m_diskCombo->currentData().toInt();
    if (diskIdx < 0)
    {
        QMessageBox::warning(this, tr("No Disk"), tr("Please select a target disk."));
        return;
    }

    PartitionScanMode mode = m_quickScanRadio->isChecked()
                                 ? PartitionScanMode::Quick
                                 : PartitionScanMode::Deep;

    m_cancelFlag.store(false);
    m_partScanProgress->setVisible(true);
    m_partScanProgress->setValue(0);
    m_partScanBtn->setEnabled(false);
    m_partResultsTable->setRowCount(0);

    auto* thread = QThread::create([this, diskIdx, mode]() {
        auto diskResult = RawDiskHandle::open(diskIdx, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            return;

        auto& disk = diskResult.value();
        PartitionRecovery recovery(disk);

        auto scanResult = recovery.scan(mode,
            [this](uint64_t scanned, uint64_t total, size_t /*found*/) {
                if (total > 0)
                {
                    int pct = static_cast<int>((scanned * 100) / total);
                    QMetaObject::invokeMethod(m_partScanProgress, "setValue",
                                              Qt::QueuedConnection, Q_ARG(int, pct));
                }
            },
            &m_cancelFlag);

        if (scanResult.isOk())
        {
            m_recoveredPartitions = scanResult.value();
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_partScanProgress->setVisible(false);
        m_partScanBtn->setEnabled(true);

        m_partResultsTable->setRowCount(0);
        for (size_t i = 0; i < m_recoveredPartitions.size(); ++i)
        {
            const auto& rp = m_recoveredPartitions[i];
            int row = m_partResultsTable->rowCount();
            m_partResultsTable->insertRow(row);

            m_partResultsTable->setItem(row, 0, new QTableWidgetItem(
                QString::number(rp.startLba)));
            m_partResultsTable->setItem(row, 1, new QTableWidgetItem(
                formatSize(rp.sectorCount * rp.sectorSize)));
            m_partResultsTable->setItem(row, 2, new QTableWidgetItem(
                FilesystemDetector::filesystemName(rp.fsType)));
            m_partResultsTable->setItem(row, 3, new QTableWidgetItem(
                QString::fromStdString(rp.label)));
            m_partResultsTable->setItem(row, 4, new QTableWidgetItem(
                QString("%1%").arg(rp.confidence, 0, 'f', 1)));
            m_partResultsTable->setItem(row, 5, new QTableWidgetItem(
                rp.overlapsExisting ? tr("Yes") : tr("No")));
        }

        m_recoverPartBtn->setEnabled(!m_recoveredPartitions.empty());
        emit statusMessage(tr("Found %1 partition(s)").arg(m_recoveredPartitions.size()));
    });

    thread->start();
}

void RecoveryTab::onStartFileRecoveryScan()
{
    int diskIdx = m_diskCombo->currentData().toInt();
    int partIdx = m_partitionCombo->currentData().toInt();

    if (diskIdx < 0 || partIdx < 0)
    {
        QMessageBox::warning(this, tr("Selection Required"),
                             tr("Please select a disk and partition."));
        return;
    }

    // Find partition info
    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == diskIdx && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    m_cancelFlag.store(false);
    m_fileScanProgress->setVisible(true);
    m_fileScanProgress->setValue(0);
    m_fileScanBtn->setEnabled(false);
    m_fileResultsTable->setRowCount(0);

    SectorOffset startLba = partInfo->offsetBytes / partInfo->sizeBytes > 0 ? partInfo->offsetBytes / 512 : 0;
    SectorCount sectors = partInfo->sizeBytes / 512;
    FilesystemType fsType = partInfo->filesystemType;

    auto* thread = QThread::create([this, diskIdx, startLba, sectors, fsType]() {
        auto diskResult = RawDiskHandle::open(diskIdx, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            return;

        auto& disk = diskResult.value();
        FileRecovery recovery(disk, startLba, sectors, fsType);

        auto scanResult = recovery.scan(FileRecoveryMode::Both,
            [this](uint64_t scanned, uint64_t total, size_t /*found*/) {
                if (total > 0)
                {
                    int pct = static_cast<int>((scanned * 100) / total);
                    QMetaObject::invokeMethod(m_fileScanProgress, "setValue",
                                              Qt::QueuedConnection, Q_ARG(int, pct));
                }
            },
            &m_cancelFlag);

        if (scanResult.isOk())
        {
            m_recoveredFiles = scanResult.value();
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_fileScanProgress->setVisible(false);
        m_fileScanBtn->setEnabled(true);

        m_fileResultsTable->setRowCount(0);
        for (size_t i = 0; i < m_recoveredFiles.size(); ++i)
        {
            const auto& rf = m_recoveredFiles[i];
            int row = m_fileResultsTable->rowCount();
            m_fileResultsTable->insertRow(row);

            m_fileResultsTable->setItem(row, 0, new QTableWidgetItem(
                QString::fromStdString(rf.filename)));
            m_fileResultsTable->setItem(row, 1, new QTableWidgetItem(
                formatSize(rf.sizeBytes)));
            m_fileResultsTable->setItem(row, 2, new QTableWidgetItem(
                QString::fromStdString(rf.extension)));
            m_fileResultsTable->setItem(row, 3, new QTableWidgetItem(
                QString("%1%").arg(rf.confidence, 0, 'f', 1)));
            m_fileResultsTable->setItem(row, 4, new QTableWidgetItem(
                FilesystemDetector::filesystemName(rf.sourceFs)));
        }

        m_recoverFileBtn->setEnabled(!m_recoveredFiles.empty());
        emit statusMessage(tr("Found %1 recoverable file(s)").arg(m_recoveredFiles.size()));
    });

    thread->start();
}

void RecoveryTab::onRecoverSelectedPartitions()
{
    int diskIdx = m_diskCombo->currentData().toInt();
    auto selected = m_partResultsTable->selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        QMessageBox::information(this, tr("No Selection"), tr("Please select partitions to recover."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Recover Partitions"),
                                      tr("This will modify the partition table on Disk %1.\nContinue?").arg(diskIdx),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    auto* thread = QThread::create([this, diskIdx, selected]() {
        auto diskResult = RawDiskHandle::open(diskIdx, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
            return;

        auto& disk = diskResult.value();
        PartitionRecovery recovery(disk);

        for (const auto& idx : selected)
        {
            int row = idx.row();
            if (row >= 0 && row < static_cast<int>(m_recoveredPartitions.size()))
            {
                recovery.recover(m_recoveredPartitions[row]);
            }
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        QMessageBox::information(this, tr("Recovery Complete"),
                                 tr("Partition recovery completed. Refreshing disk list..."));
        emit statusMessage(tr("Partition recovery completed"));
    });

    thread->start();
}

void RecoveryTab::onRecoverSelectedFiles()
{
    QString outputDir = m_outputFolderEdit->text();
    if (outputDir.isEmpty())
    {
        QMessageBox::warning(this, tr("No Output"), tr("Please select an output folder."));
        return;
    }

    auto selected = m_fileResultsTable->selectionModel()->selectedRows();
    if (selected.isEmpty())
    {
        QMessageBox::information(this, tr("No Selection"), tr("Please select files to recover."));
        return;
    }

    int diskIdx = m_diskCombo->currentData().toInt();
    int partIdx = m_partitionCombo->currentData().toInt();

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == diskIdx && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    SectorOffset startLba = partInfo->offsetBytes / 512;
    SectorCount sectors = partInfo->sizeBytes / 512;
    FilesystemType fsType = partInfo->filesystemType;

    auto filesToRecover = selected;
    auto outputPath = outputDir.toStdString();

    auto* thread = QThread::create([this, diskIdx, startLba, sectors, fsType, filesToRecover, outputPath]() {
        auto diskResult = RawDiskHandle::open(diskIdx, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            return;

        auto& disk = diskResult.value();
        FileRecovery recovery(disk, startLba, sectors, fsType);

        for (const auto& idx : filesToRecover)
        {
            int row = idx.row();
            if (row >= 0 && row < static_cast<int>(m_recoveredFiles.size()))
            {
                std::string filePath = outputPath + "/" + m_recoveredFiles[row].filename;
                recovery.recoverFile(m_recoveredFiles[row], filePath);
            }
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        QMessageBox::information(this, tr("File Recovery Complete"),
                                 tr("File recovery completed."));
        emit statusMessage(tr("File recovery completed"));
    });

    thread->start();
}

void RecoveryTab::onStartBootRepair()
{
    int diskIdx = m_diskCombo->currentData().toInt();
    if (diskIdx < 0)
    {
        QMessageBox::warning(this, tr("No Disk"), tr("Please select a target disk."));
        return;
    }

    if (!m_repairMbr->isChecked() && !m_repairGpt->isChecked() &&
        !m_repairBootSector->isChecked() && !m_repairBcd->isChecked() &&
        !m_repairBootloader->isChecked())
    {
        QMessageBox::warning(this, tr("No Options"), tr("Please select at least one repair option."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Boot Repair"),
                                      tr("Boot repair will modify critical disk structures.\n"
                                         "Incorrect use can render a system unbootable.\n\nContinue?"),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    bool doMbr = m_repairMbr->isChecked();
    bool doGpt = m_repairGpt->isChecked();
    bool doBoot = m_repairBootSector->isChecked();
    bool doBcd = m_repairBcd->isChecked();
    bool doBootloader = m_repairBootloader->isChecked();

    m_bootRepairProgress->setVisible(true);
    m_bootRepairProgress->setRange(0, 0); // Indeterminate
    m_bootRepairStartBtn->setEnabled(false);
    m_bootRepairStatus->setText(tr("Repairing..."));

    auto* thread = QThread::create([this, diskIdx, doMbr, doGpt, doBoot, doBcd, doBootloader]() {
        auto diskResult = RawDiskHandle::open(diskIdx, DiskAccessMode::ReadWrite);
        if (diskResult.isError())
        {
            QMetaObject::invokeMethod(m_bootRepairStatus, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, tr("Failed to open disk: %1")
                                                         .arg(QString::fromStdString(diskResult.error().message))));
            return;
        }

        auto& disk = diskResult.value();
        BootRepair repair(disk);
        QStringList results;

        BootRepairProgress progress = [this](const std::string& step, int idx, int total) {
            Q_UNUSED(idx);
            Q_UNUSED(total);
            QMetaObject::invokeMethod(m_bootRepairStatus, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString::fromStdString(step)));
        };

        if (doMbr)
        {
            auto r = repair.repairMbr(progress);
            results << (r.isOk() ? tr("MBR: Repaired") : tr("MBR: Failed"));
        }
        if (doGpt)
        {
            auto r = repair.repairGpt(true, progress);
            results << (r.isOk() ? tr("GPT: Repaired") : tr("GPT: Failed"));
        }
        if (doBoot)
        {
            // Find first NTFS/FAT partition
            for (const auto& p : m_snapshot.partitions)
            {
                if (p.diskId == diskIdx &&
                    (p.filesystemType == FilesystemType::NTFS || p.filesystemType == FilesystemType::FAT32))
                {
                    SectorOffset startLba = p.offsetBytes / 512;
                    SectorCount sectors = p.sizeBytes / 512;
                    auto r = repair.repairBootSector(startLba, sectors, progress);
                    results << (r.isOk() ? tr("Boot Sector: Repaired") : tr("Boot Sector: Failed"));
                    break;
                }
            }
        }
        if (doBcd)
        {
            auto r = repair.repairBcd(L'S', progress); // Assume S: is ESP
            results << (r.isOk() ? tr("BCD: Repaired") : tr("BCD: Failed"));
        }
        if (doBootloader)
        {
            auto r = repair.repairBootloader(L'S', L'C', progress);
            results << (r.isOk() ? tr("Bootloader: Repaired") : tr("Bootloader: Failed"));
        }

        QString summary = results.join("\n");
        QMetaObject::invokeMethod(m_bootRepairStatus, "setText",
                                  Qt::QueuedConnection, Q_ARG(QString, summary));
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_bootRepairProgress->setVisible(false);
        m_bootRepairStartBtn->setEnabled(true);
        emit statusMessage(tr("Boot repair completed"));
    });

    thread->start();
}

void RecoveryTab::onBrowseOutputFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Output Folder"));
    if (!dir.isEmpty())
        m_outputFolderEdit->setText(dir);
}

QString RecoveryTab::formatSize(uint64_t bytes)
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
