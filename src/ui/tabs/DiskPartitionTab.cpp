#include "DiskPartitionTab.h"
#include "ui/widgets/DiskMapWidget.h"

#include "core/disk/DiskEnumerator.h"
#include "core/disk/FilesystemDetector.h"
#include "core/operations/OperationQueue.h"
#include "core/operations/PartitionOperations.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTableView>
#include <QThread>
#include <QTreeView>
#include <QVBoxLayout>

namespace spw
{

DiskPartitionTab::DiskPartitionTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();

    connect(&m_opQueue, &OperationQueue::allOperationsFinished,
            this, [this](bool success, int completed, int total) {
                Q_UNUSED(completed);
                Q_UNUSED(total);
                if (success)
                {
                    QMessageBox::information(this, tr("Operations Complete"),
                                             tr("All %1 operations completed successfully.").arg(completed));
                }
                else
                {
                    QMessageBox::warning(this, tr("Operations Failed"),
                                         tr("Operation failed. %1 of %2 completed.").arg(completed).arg(total));
                }
                emit statusMessage(tr("Refreshing disk list after operations..."));
                // Request a full refresh
                auto result = DiskEnumerator::getSystemSnapshot();
                if (result.isOk())
                    refreshDisks(result.value());
            });
}

DiskPartitionTab::~DiskPartitionTab() = default;

void DiskPartitionTab::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);

    // Left panel: Disk tree view
    auto* leftPanel = new QWidget();
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto* diskLabel = new QLabel(tr("Physical Disks"));
    diskLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    leftLayout->addWidget(diskLabel);

    m_diskTreeModel = new QStandardItemModel(this);
    m_diskTreeModel->setHorizontalHeaderLabels({tr("Disk / Partition"), tr("Size"), tr("Type")});

    m_diskTree = new QTreeView();
    m_diskTree->setModel(m_diskTreeModel);
    m_diskTree->setHeaderHidden(false);
    m_diskTree->setAlternatingRowColors(true);
    m_diskTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_diskTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTree->setSelectionMode(QAbstractItemView::SingleSelection);
    leftLayout->addWidget(m_diskTree);

    connect(m_diskTree->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &DiskPartitionTab::onDiskTreeSelectionChanged);

    m_mainSplitter->addWidget(leftPanel);

    // Center + Bottom: partition map and table
    m_rightSplitter = new QSplitter(Qt::Vertical);

    // Disk map widget
    m_diskMap = new DiskMapWidget();
    m_rightSplitter->addWidget(m_diskMap);

    connect(m_diskMap, &DiskMapWidget::partitionClicked,
            this, &DiskPartitionTab::onDiskMapPartitionClicked);
    connect(m_diskMap, &DiskMapWidget::contextMenuRequested,
            this, &DiskPartitionTab::onDiskMapContextMenu);

    // Partition detail table
    m_partitionModel = new QStandardItemModel(this);
    m_partitionModel->setHorizontalHeaderLabels(
        {tr("#"), tr("Label"), tr("Drive Letter"), tr("Filesystem"),
         tr("Size"), tr("Used"), tr("Free"), tr("Status"), tr("Flags")});

    m_partitionTable = new QTableView();
    m_partitionTable->setModel(m_partitionModel);
    m_partitionTable->setAlternatingRowColors(true);
    m_partitionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_partitionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_partitionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_partitionTable->horizontalHeader()->setStretchLastSection(true);
    m_partitionTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_partitionTable, &QWidget::customContextMenuRequested,
            this, &DiskPartitionTab::onPartitionTableContextMenu);

    m_rightSplitter->addWidget(m_partitionTable);

    m_rightSplitter->setStretchFactor(0, 2);
    m_rightSplitter->setStretchFactor(1, 3);

    m_mainSplitter->addWidget(m_rightSplitter);

    // Right panel: pending operations list
    auto* opPanel = new QWidget();
    auto* opLayout = new QVBoxLayout(opPanel);
    opLayout->setContentsMargins(0, 0, 0, 0);

    auto* opLabel = new QLabel(tr("Pending Operations"));
    opLabel->setStyleSheet("font-weight: bold; padding: 4px;");
    opLayout->addWidget(opLabel);

    m_operationListWidget = new QListWidget();
    m_operationListWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    opLayout->addWidget(m_operationListWidget);

    auto* buttonLayout = new QHBoxLayout();
    m_applyBtn = new QPushButton(tr("Apply"));
    m_applyBtn->setObjectName("applyButton");
    m_applyBtn->setEnabled(false);
    m_undoBtn = new QPushButton(tr("Undo"));
    m_undoBtn->setEnabled(false);
    m_clearBtn = new QPushButton(tr("Clear"));
    m_clearBtn->setEnabled(false);
    buttonLayout->addWidget(m_applyBtn);
    buttonLayout->addWidget(m_undoBtn);
    buttonLayout->addWidget(m_clearBtn);
    opLayout->addLayout(buttonLayout);

    connect(m_applyBtn, &QPushButton::clicked, this, &DiskPartitionTab::onApplyOperations);
    connect(m_undoBtn, &QPushButton::clicked, this, &DiskPartitionTab::onUndoOperation);
    connect(m_clearBtn, &QPushButton::clicked, this, &DiskPartitionTab::onClearOperations);

    m_mainSplitter->addWidget(opPanel);

    // Set splitter proportions
    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 3);
    m_mainSplitter->setStretchFactor(2, 1);

    layout->addWidget(m_mainSplitter);
}

void DiskPartitionTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskTree(snapshot);

    // Re-select current disk if still valid
    if (m_selectedDiskId >= 0)
    {
        populatePartitionTable(m_selectedDiskId);
        updateDiskMap(m_selectedDiskId);
    }
}

void DiskPartitionTab::populateDiskTree(const SystemDiskSnapshot& snapshot)
{
    m_diskTreeModel->removeRows(0, m_diskTreeModel->rowCount());

    for (const auto& disk : snapshot.disks)
    {
        QString diskName = QString("Disk %1: %2")
                               .arg(disk.id)
                               .arg(QString::fromStdWString(disk.model));
        auto* diskItem = new QStandardItem(diskName);
        diskItem->setData(disk.id, Qt::UserRole);    // Store diskId
        diskItem->setData(-1, Qt::UserRole + 1);     // Not a partition
        diskItem->setIcon(QIcon::fromTheme("drive-harddisk"));

        auto* sizeItem = new QStandardItem(formatSize(disk.sizeBytes));
        auto* typeItem = new QStandardItem(
            QString("%1 / %2")
                .arg(interfaceTypeString(disk.interfaceType))
                .arg(partitionTableTypeString(disk.partitionTableType)));

        // Find partitions belonging to this disk
        for (const auto& part : snapshot.partitions)
        {
            if (part.diskId != disk.id)
                continue;

            QString partLabel;
            if (part.driveLetter != L'\0')
                partLabel = QString("(%1:) ").arg(QChar(part.driveLetter));
            if (!part.label.empty())
                partLabel += QString::fromStdWString(part.label);
            else
                partLabel += filesystemString(part.filesystemType);

            auto* partItem = new QStandardItem(partLabel);
            partItem->setData(disk.id, Qt::UserRole);
            partItem->setData(part.index, Qt::UserRole + 1);

            auto* partSizeItem = new QStandardItem(formatSize(part.sizeBytes));
            auto* partFsItem = new QStandardItem(filesystemString(part.filesystemType));

            diskItem->appendRow({partItem, partSizeItem, partFsItem});
        }

        m_diskTreeModel->appendRow({diskItem, sizeItem, typeItem});
    }

    m_diskTree->expandAll();
    m_diskTree->resizeColumnToContents(0);
}

void DiskPartitionTab::populatePartitionTable(DiskId diskId)
{
    m_partitionModel->removeRows(0, m_partitionModel->rowCount());

    for (const auto& part : m_snapshot.partitions)
    {
        if (part.diskId != diskId)
            continue;

        QList<QStandardItem*> row;
        row.append(new QStandardItem(QString::number(part.index)));

        // Label
        row.append(new QStandardItem(QString::fromStdWString(part.label)));

        // Drive letter
        if (part.driveLetter != L'\0')
            row.append(new QStandardItem(QString("%1:").arg(QChar(part.driveLetter))));
        else
            row.append(new QStandardItem(QStringLiteral("-")));

        // Filesystem
        row.append(new QStandardItem(filesystemString(part.filesystemType)));

        // Size
        row.append(new QStandardItem(formatSize(part.sizeBytes)));

        // Used / Free — look up volume info
        QString usedStr = QStringLiteral("-");
        QString freeStr = QStringLiteral("-");
        for (const auto& vol : m_snapshot.volumes)
        {
            if (vol.guidPath == part.volumeGuidPath && vol.totalBytes > 0)
            {
                uint64_t used = vol.totalBytes - vol.freeBytes;
                usedStr = formatSize(used);
                freeStr = formatSize(vol.freeBytes);
                break;
            }
        }
        row.append(new QStandardItem(usedStr));
        row.append(new QStandardItem(freeStr));

        // Status
        QStringList statusFlags;
        if (part.isActive)
            statusFlags << QStringLiteral("Active");
        if (part.isBootable)
            statusFlags << QStringLiteral("Boot");
        row.append(new QStandardItem(statusFlags.isEmpty() ? QStringLiteral("Normal") : statusFlags.join(", ")));

        // Flags
        QStringList flags;
        if (part.isActive)
            flags << QStringLiteral("Boot");
        if (part.mbrType != 0)
            flags << QString("MBR 0x%1").arg(part.mbrType, 2, 16, QChar('0'));
        row.append(new QStandardItem(flags.isEmpty() ? QStringLiteral("-") : flags.join(", ")));

        // Store partition index in first item
        row[0]->setData(part.index, Qt::UserRole);

        m_partitionModel->appendRow(row);
    }

    m_partitionTable->resizeColumnsToContents();
}

void DiskPartitionTab::updateDiskMap(DiskId diskId)
{
    // Collect partitions for this disk
    std::vector<PartitionInfo> diskPartitions;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == diskId)
            diskPartitions.push_back(p);
    }

    // Find disk info
    for (const auto& d : m_snapshot.disks)
    {
        if (d.id == diskId)
        {
            m_diskMap->setDisk(d, diskPartitions, m_snapshot.volumes);
            return;
        }
    }

    m_diskMap->clear();
}

void DiskPartitionTab::onDiskTreeSelectionChanged()
{
    auto indexes = m_diskTree->selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;

    auto idx = indexes.first();
    DiskId diskId = idx.data(Qt::UserRole).toInt();
    m_selectedDiskId = diskId;

    populatePartitionTable(diskId);
    updateDiskMap(diskId);
}

void DiskPartitionTab::onPartitionTableContextMenu(const QPoint& pos)
{
    auto index = m_partitionTable->indexAt(pos);
    int partIdx = -1;
    if (index.isValid())
    {
        auto* item = m_partitionModel->item(index.row(), 0);
        if (item)
            partIdx = item->data(Qt::UserRole).toInt();
    }
    showContextMenu(partIdx, m_partitionTable->viewport()->mapToGlobal(pos));
}

void DiskPartitionTab::onDiskMapContextMenu(int partitionIndex, const QPoint& globalPos)
{
    showContextMenu(partitionIndex, globalPos);
}

void DiskPartitionTab::onDiskMapPartitionClicked(int partitionIndex)
{
    // Select the corresponding row in partition table
    for (int r = 0; r < m_partitionModel->rowCount(); ++r)
    {
        auto* item = m_partitionModel->item(r, 0);
        if (item && item->data(Qt::UserRole).toInt() == partitionIndex)
        {
            m_partitionTable->selectRow(r);
            break;
        }
    }
}

void DiskPartitionTab::showContextMenu(int partitionIndex, const QPoint& globalPos)
{
    QMenu menu(this);

    auto* createAct = menu.addAction(tr("Create Partition..."));
    connect(createAct, &QAction::triggered, this, &DiskPartitionTab::onCreatePartition);

    if (partitionIndex >= 0)
    {
        menu.addSeparator();

        auto* deleteAct = menu.addAction(tr("Delete Partition"));
        connect(deleteAct, &QAction::triggered, this, &DiskPartitionTab::onDeletePartition);

        auto* resizeAct = menu.addAction(tr("Resize/Move..."));
        connect(resizeAct, &QAction::triggered, this, &DiskPartitionTab::onResizePartition);

        auto* formatAct = menu.addAction(tr("Format..."));
        connect(formatAct, &QAction::triggered, this, &DiskPartitionTab::onFormatPartition);

        menu.addSeparator();

        auto* labelAct = menu.addAction(tr("Set Label..."));
        connect(labelAct, &QAction::triggered, this, &DiskPartitionTab::onSetLabel);

        auto* flagsAct = menu.addAction(tr("Set Flags..."));
        connect(flagsAct, &QAction::triggered, this, &DiskPartitionTab::onSetFlags);

        menu.addSeparator();

        auto* checkAct = menu.addAction(tr("Check Filesystem"));
        connect(checkAct, &QAction::triggered, this, &DiskPartitionTab::onCheckFilesystem);
    }

    menu.exec(globalPos);
}

int DiskPartitionTab::selectedPartitionIndex() const
{
    auto indexes = m_partitionTable->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return -1;
    auto* item = m_partitionModel->item(indexes.first().row(), 0);
    return item ? item->data(Qt::UserRole).toInt() : -1;
}

DiskId DiskPartitionTab::selectedDiskId() const
{
    return m_selectedDiskId;
}

void DiskPartitionTab::onCreatePartition()
{
    if (m_selectedDiskId < 0)
    {
        QMessageBox::warning(this, tr("No Disk"), tr("Please select a disk first."));
        return;
    }

    // Find disk info
    const DiskInfo* diskInfo = nullptr;
    for (const auto& d : m_snapshot.disks)
    {
        if (d.id == m_selectedDiskId)
        {
            diskInfo = &d;
            break;
        }
    }
    if (!diskInfo)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Create Partition"));
    auto* form = new QFormLayout(&dlg);

    auto* sizeGbSpin = new QDoubleSpinBox();
    sizeGbSpin->setRange(0.001, static_cast<double>(diskInfo->sizeBytes) / (1024.0 * 1024.0 * 1024.0));
    sizeGbSpin->setDecimals(3);
    sizeGbSpin->setSuffix(QStringLiteral(" GB"));
    sizeGbSpin->setValue(1.0);
    form->addRow(tr("Size:"), sizeGbSpin);

    // Full filesystem list — label + corresponding enum value kept in sync
    struct FsEntry { const char* label; FilesystemType type; };
    static const FsEntry kCreateFsEntries[] = {
        // ── Windows / Modern ──
        { "NTFS",                        FilesystemType::NTFS        },
        { "FAT32",                       FilesystemType::FAT32       },
        { "FAT16",                       FilesystemType::FAT16       },
        { "FAT12  (floppy / tiny)",      FilesystemType::FAT12       },
        { "exFAT  (flash / large SD)",   FilesystemType::ExFAT       },
        { "ReFS   (Windows Server)",     FilesystemType::ReFS        },
        // ── Linux ──
        { "ext4",                        FilesystemType::Ext4        },
        { "ext3",                        FilesystemType::Ext3        },
        { "ext2",                        FilesystemType::Ext2        },
        { "Btrfs",                       FilesystemType::Btrfs       },
        { "XFS",                         FilesystemType::XFS         },
        { "ZFS",                         FilesystemType::ZFS         },
        { "JFS",                         FilesystemType::JFS         },
        { "ReiserFS",                    FilesystemType::ReiserFS    },
        { "F2FS   (flash-optimised)",    FilesystemType::F2FS        },
        { "JFFS2  (embedded flash)",     FilesystemType::JFFS2       },
        { "NILFS2",                      FilesystemType::NILFS2      },
        { "Linux Swap",                  FilesystemType::SWAP_LINUX  },
        // ── Apple ──
        { "HFS+   (Mac OS Extended)",    FilesystemType::HFSPlus     },
        { "HFS    (Classic Mac OS)",     FilesystemType::HFS         },
        { "APFS   (detection only)",     FilesystemType::APFS        },
        // ── Unix / BSD ──
        { "UFS    (BSD / Solaris)",      FilesystemType::UFS         },
        // ── Legacy / Retro ──
        { "HPFS   (OS/2)",              FilesystemType::HPFS        },
        { "VFAT   (long-name FAT)",     FilesystemType::VFAT        },
        { "UDF    (optical / universal)",FilesystemType::UDF         },
        { "ISO 9660 (CD-ROM)",          FilesystemType::ISO9660     },
        { "Minix",                      FilesystemType::Minix       },
        { "QNX4",                       FilesystemType::QNX4        },
        { "Amiga FFS",                  FilesystemType::AfFS        },
        { "BeOS BFS",                   FilesystemType::BFS_BeOS    },
        { "SquashFS (read-only)",       FilesystemType::SquashFS    },
        { "RomFS   (read-only)",        FilesystemType::RomFS       },
        // ── Console / Gaming ──
        { "FATX    (Xbox / Xbox 360)",  FilesystemType::FATX        },
        // ── Raw / unformatted ──
        { "Unformatted / Raw",          FilesystemType::Raw         },
    };
    constexpr int kCreateFsCount = static_cast<int>(std::size(kCreateFsEntries));

    auto* fsCombo = new QComboBox();
    for (int i = 0; i < kCreateFsCount; ++i)
        fsCombo->addItem(QString::fromLatin1(kCreateFsEntries[i].label));
    form->addRow(tr("Filesystem:"), fsCombo);

    auto* labelEdit = new QLineEdit();
    form->addRow(tr("Label:"), labelEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    // Build operation
    uint64_t sizeBytes = static_cast<uint64_t>(sizeGbSpin->value() * 1024.0 * 1024.0 * 1024.0);
    uint32_t sectorSize = diskInfo->sectorSize;
    SectorCount sectors = sizeBytes / sectorSize;

    // Find first large enough gap — simple: offset after last partition
    SectorOffset startLba = DEFAULT_ALIGNMENT_SECTORS_512;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId)
        {
            SectorOffset end = (p.offsetBytes + p.sizeBytes) / sectorSize;
            if (end > startLba)
                startLba = DiskGeometry::alignSectorUp(end, DEFAULT_ALIGNMENT_SECTORS_512);
        }
    }

    CreatePartitionOp::Params params;
    params.diskId = m_selectedDiskId;
    params.startLba = startLba;
    params.sectorCount = sectors;
    params.sectorSize = sectorSize;
    params.formatAfter = true;

    int fsIdx = fsCombo->currentIndex();
    if (fsIdx >= 0 && fsIdx < kCreateFsCount)
        params.formatOptions.targetFs = kCreateFsEntries[fsIdx].type;
    params.formatOptions.volumeLabel = labelEdit->text().toStdString();
    params.formatOptions.quickFormat = true;

    auto op = std::make_unique<CreatePartitionOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onDeletePartition()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    // Find partition info
    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    auto reply = QMessageBox::question(this, tr("Delete Partition"),
                                       tr("Are you sure you want to delete partition %1?\n"
                                          "This operation will be queued and applied when you click Apply.")
                                           .arg(partIdx));
    if (reply != QMessageBox::Yes)
        return;

    DeletePartitionOp::Params params;
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;
    params.sectorSize = 512; // Default
    params.driveLetter = partInfo->driveLetter;

    auto op = std::make_unique<DeletePartitionOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onResizePartition()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    double currentGb = static_cast<double>(partInfo->sizeBytes) / (1024.0 * 1024.0 * 1024.0);

    bool ok = false;
    double newGb = QInputDialog::getDouble(this, tr("Resize Partition"),
                                           tr("New size in GB (current: %1 GB):").arg(currentGb, 0, 'f', 2),
                                           currentGb, 0.001, 999999.0, 3, &ok);
    if (!ok)
        return;

    uint64_t newSizeBytes = static_cast<uint64_t>(newGb * 1024.0 * 1024.0 * 1024.0);
    uint32_t sectorSize = 512;
    SectorCount newSectors = newSizeBytes / sectorSize;
    SectorOffset startLba = partInfo->offsetBytes / sectorSize;

    ResizePartitionOp::Params params;
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;
    params.sectorSize = sectorSize;
    params.driveLetter = partInfo->driveLetter;
    params.newStartLba = startLba;
    params.newSectorCount = newSectors;

    auto op = std::make_unique<ResizePartitionOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onFormatPartition()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Format Partition"));
    auto* form = new QFormLayout(&dlg);

    struct FmtEntry { const char* label; FilesystemType type; };
    static const FmtEntry kFmtFsEntries[] = {
        // ── Windows / Modern ──
        { "NTFS",                        FilesystemType::NTFS        },
        { "FAT32",                       FilesystemType::FAT32       },
        { "FAT16",                       FilesystemType::FAT16       },
        { "FAT12  (floppy / tiny)",      FilesystemType::FAT12       },
        { "exFAT  (flash / large SD)",   FilesystemType::ExFAT       },
        { "ReFS   (Windows Server)",     FilesystemType::ReFS        },
        // ── Linux ──
        { "ext4",                        FilesystemType::Ext4        },
        { "ext3",                        FilesystemType::Ext3        },
        { "ext2",                        FilesystemType::Ext2        },
        { "Btrfs",                       FilesystemType::Btrfs       },
        { "XFS",                         FilesystemType::XFS         },
        { "ZFS",                         FilesystemType::ZFS         },
        { "JFS",                         FilesystemType::JFS         },
        { "ReiserFS",                    FilesystemType::ReiserFS    },
        { "F2FS   (flash-optimised)",    FilesystemType::F2FS        },
        { "JFFS2  (embedded flash)",     FilesystemType::JFFS2       },
        { "NILFS2",                      FilesystemType::NILFS2      },
        { "Linux Swap",                  FilesystemType::SWAP_LINUX  },
        // ── Apple ──
        { "HFS+   (Mac OS Extended)",    FilesystemType::HFSPlus     },
        { "HFS    (Classic Mac OS)",     FilesystemType::HFS         },
        // ── Unix / BSD ──
        { "UFS    (BSD / Solaris)",      FilesystemType::UFS         },
        // ── Legacy / Retro ──
        { "HPFS   (OS/2)",              FilesystemType::HPFS        },
        { "VFAT   (long-name FAT)",     FilesystemType::VFAT        },
        { "UDF    (optical)",           FilesystemType::UDF         },
        { "ISO 9660 (CD-ROM)",          FilesystemType::ISO9660     },
        { "Minix",                      FilesystemType::Minix       },
        { "QNX4",                       FilesystemType::QNX4        },
        { "Amiga FFS",                  FilesystemType::AfFS        },
        { "BeOS BFS",                   FilesystemType::BFS_BeOS    },
        { "SquashFS (read-only)",       FilesystemType::SquashFS    },
        { "RomFS   (read-only)",        FilesystemType::RomFS       },
        // ── Console / Gaming ──
        { "FATX    (Xbox / Xbox 360)",  FilesystemType::FATX        },
    };
    constexpr int kFmtFsCount = static_cast<int>(std::size(kFmtFsEntries));

    auto* fsCombo = new QComboBox();
    for (int i = 0; i < kFmtFsCount; ++i)
        fsCombo->addItem(QString::fromLatin1(kFmtFsEntries[i].label));
    form->addRow(tr("Filesystem:"), fsCombo);

    auto* labelEdit = new QLineEdit();
    form->addRow(tr("Label:"), labelEdit);

    auto* quickCheck = new QCheckBox(tr("Quick Format"));
    quickCheck->setChecked(true);
    form->addRow(quickCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    FormatPartitionOp::Params params;
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;

    if (partInfo->driveLetter != L'\0')
    {
        params.target.driveLetter = partInfo->driveLetter;
    }
    else
    {
        params.target.diskIndex = m_selectedDiskId;
        params.target.partitionOffsetBytes = partInfo->offsetBytes;
        params.target.partitionSizeBytes = partInfo->sizeBytes;
    }

    int fsIdx = fsCombo->currentIndex();
    if (fsIdx >= 0 && fsIdx < kFmtFsCount)
        params.options.targetFs = kFmtFsEntries[fsIdx].type;

    params.options.volumeLabel = labelEdit->text().toStdString();
    params.options.quickFormat = quickCheck->isChecked();

    auto op = std::make_unique<FormatPartitionOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onSetLabel()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    bool ok = false;
    QString newLabel = QInputDialog::getText(this, tr("Set Volume Label"),
                                             tr("New label:"),
                                             QLineEdit::Normal,
                                             QString::fromStdWString(partInfo->label), &ok);
    if (!ok)
        return;

    SetLabelOp::Params params;
    params.driveLetter = partInfo->driveLetter;
    params.newLabel = newLabel.toStdString();
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;
    params.partitionOffsetBytes = partInfo->offsetBytes;
    params.fsType = partInfo->filesystemType;

    auto op = std::make_unique<SetLabelOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onSetFlags()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Set Partition Flags"));
    auto* form = new QFormLayout(&dlg);

    auto* activeCheck = new QCheckBox(tr("Active (Bootable)"));
    activeCheck->setChecked(partInfo->isActive);
    form->addRow(activeCheck);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    SetFlagsOp::Params params;
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;
    params.setActive = activeCheck->isChecked();

    auto op = std::make_unique<SetFlagsOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onCheckFilesystem()
{
    int partIdx = selectedPartitionIndex();
    if (partIdx < 0 || m_selectedDiskId < 0)
        return;

    const PartitionInfo* partInfo = nullptr;
    for (const auto& p : m_snapshot.partitions)
    {
        if (p.diskId == m_selectedDiskId && p.index == partIdx)
        {
            partInfo = &p;
            break;
        }
    }
    if (!partInfo)
        return;

    CheckFilesystemOp::Params params;
    params.driveLetter = partInfo->driveLetter;
    params.diskId = m_selectedDiskId;
    params.partitionIndex = partIdx;
    params.partitionOffsetBytes = partInfo->offsetBytes;
    params.fsType = partInfo->filesystemType;
    params.repair = false;

    auto op = std::make_unique<CheckFilesystemOp>(params);
    m_opQueue.enqueue(std::move(op));
    updateOperationList();
}

void DiskPartitionTab::onApplyOperations()
{
    if (m_opQueue.pendingCount() == 0)
        return;

    auto reply = QMessageBox::warning(
        this, tr("Apply Operations"),
        tr("You are about to apply %1 pending operation(s).\n\n"
           "WARNING: These operations may modify your disk permanently.\n"
           "Make sure you have backed up important data.\n\n"
           "Continue?")
            .arg(m_opQueue.pendingCount()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    auto* progressDlg = new QProgressDialog(tr("Applying operations..."), tr("Cancel"), 0, 100, this);
    progressDlg->setWindowModality(Qt::WindowModal);
    progressDlg->setMinimumDuration(0);
    progressDlg->show();

    connect(&m_opQueue, &OperationQueue::queueProgress,
            progressDlg, [progressDlg](int overall, int /*current*/, const QString& status) {
                progressDlg->setValue(overall);
                progressDlg->setLabelText(status);
            });

    connect(progressDlg, &QProgressDialog::canceled, &m_opQueue, &OperationQueue::requestCancel);

    auto* thread = QThread::create([this]() {
        m_opQueue.applyAll();
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, progressDlg, &QProgressDialog::close);
    connect(thread, &QThread::finished, progressDlg, &QProgressDialog::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        updateOperationList();
    });

    thread->start();
}

void DiskPartitionTab::onUndoOperation()
{
    auto removed = m_opQueue.removeLast();
    if (removed)
    {
        updateOperationList();
    }
}

void DiskPartitionTab::onClearOperations()
{
    m_opQueue.clearPending();
    updateOperationList();
}

void DiskPartitionTab::updateOperationList()
{
    m_operationListWidget->clear();
    const auto& pending = m_opQueue.pending();
    for (const auto& op : pending)
    {
        m_operationListWidget->addItem(op->description());
    }

    bool hasPending = m_opQueue.pendingCount() > 0;
    m_applyBtn->setEnabled(hasPending);
    m_undoBtn->setEnabled(hasPending);
    m_clearBtn->setEnabled(hasPending);

    emit statusMessage(hasPending
                           ? tr("%1 pending operation(s)").arg(m_opQueue.pendingCount())
                           : tr("No pending operations"));
}

QString DiskPartitionTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    if (bytes >= 1024ULL)
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QString("%1 B").arg(bytes);
}

QString DiskPartitionTab::interfaceTypeString(DiskInterfaceType type)
{
    switch (type)
    {
    case DiskInterfaceType::SATA:        return QStringLiteral("SATA");
    case DiskInterfaceType::NVMe:        return QStringLiteral("NVMe");
    case DiskInterfaceType::USB:         return QStringLiteral("USB");
    case DiskInterfaceType::SCSI:        return QStringLiteral("SCSI");
    case DiskInterfaceType::SAS:         return QStringLiteral("SAS");
    case DiskInterfaceType::IDE:         return QStringLiteral("IDE");
    case DiskInterfaceType::MMC:         return QStringLiteral("MMC");
    case DiskInterfaceType::Firewire:    return QStringLiteral("FireWire");
    case DiskInterfaceType::Thunderbolt: return QStringLiteral("Thunderbolt");
    case DiskInterfaceType::Virtual:     return QStringLiteral("Virtual");
    default:                              return QStringLiteral("Unknown");
    }
}

QString DiskPartitionTab::mediaTypeString(MediaType type)
{
    switch (type)
    {
    case MediaType::HDD:          return QStringLiteral("HDD");
    case MediaType::SSD:          return QStringLiteral("SSD");
    case MediaType::NVMe:         return QStringLiteral("NVMe");
    case MediaType::USBFlash:     return QStringLiteral("USB Flash");
    case MediaType::SDCard:       return QStringLiteral("SD Card");
    case MediaType::CompactFlash: return QStringLiteral("CF");
    case MediaType::OpticalDrive: return QStringLiteral("Optical");
    case MediaType::FloppyDisk:   return QStringLiteral("Floppy");
    case MediaType::Virtual:      return QStringLiteral("Virtual");
    default:                       return QStringLiteral("Unknown");
    }
}

QString DiskPartitionTab::filesystemString(FilesystemType fs)
{
    switch (fs)
    {
    case FilesystemType::NTFS:         return QStringLiteral("NTFS");
    case FilesystemType::FAT32:        return QStringLiteral("FAT32");
    case FilesystemType::FAT16:        return QStringLiteral("FAT16");
    case FilesystemType::FAT12:        return QStringLiteral("FAT12");
    case FilesystemType::ExFAT:        return QStringLiteral("exFAT");
    case FilesystemType::ReFS:         return QStringLiteral("ReFS");
    case FilesystemType::Ext2:         return QStringLiteral("ext2");
    case FilesystemType::Ext3:         return QStringLiteral("ext3");
    case FilesystemType::Ext4:         return QStringLiteral("ext4");
    case FilesystemType::Btrfs:        return QStringLiteral("Btrfs");
    case FilesystemType::XFS:          return QStringLiteral("XFS");
    case FilesystemType::ZFS:          return QStringLiteral("ZFS");
    case FilesystemType::HFSPlus:      return QStringLiteral("HFS+");
    case FilesystemType::APFS:         return QStringLiteral("APFS");
    case FilesystemType::SWAP_LINUX:   return QStringLiteral("Linux Swap");
    case FilesystemType::Unallocated:  return QStringLiteral("Unallocated");
    case FilesystemType::Raw:          return QStringLiteral("RAW");
    default:                            return QStringLiteral("Unknown");
    }
}

QString DiskPartitionTab::partitionTableTypeString(PartitionTableType pt)
{
    switch (pt)
    {
    case PartitionTableType::MBR: return QStringLiteral("MBR");
    case PartitionTableType::GPT: return QStringLiteral("GPT");
    case PartitionTableType::APM: return QStringLiteral("APM");
    default:                       return QStringLiteral("Unknown");
    }
}

} // namespace spw
