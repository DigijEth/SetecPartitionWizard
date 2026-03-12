#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/operations/OperationQueue.h"

#include <QWidget>

class QSplitter;
class QTreeView;
class QTableView;
class QListWidget;
class QPushButton;
class QStandardItemModel;
class QMenu;
class QLabel;
class QProgressDialog;

namespace spw
{

class DiskMapWidget;

class DiskPartitionTab : public QWidget
{
    Q_OBJECT

public:
    explicit DiskPartitionTab(QWidget* parent = nullptr);
    ~DiskPartitionTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onDiskTreeSelectionChanged();
    void onPartitionTableContextMenu(const QPoint& pos);
    void onDiskMapContextMenu(int partitionIndex, const QPoint& globalPos);
    void onDiskMapPartitionClicked(int partitionIndex);
    void onApplyOperations();
    void onUndoOperation();
    void onClearOperations();

    // Context menu actions
    void onCreatePartition();
    void onDeletePartition();
    void onResizePartition();
    void onFormatPartition();
    void onSetLabel();
    void onSetFlags();
    void onCheckFilesystem();

private:
    void setupUi();
    void populateDiskTree(const SystemDiskSnapshot& snapshot);
    void populatePartitionTable(DiskId diskId);
    void updateDiskMap(DiskId diskId);
    void updateOperationList();
    void showContextMenu(int partitionIndex, const QPoint& globalPos);

    // Find the currently selected partition info
    int selectedPartitionIndex() const;
    DiskId selectedDiskId() const;

    static QString formatSize(uint64_t bytes);
    static QString interfaceTypeString(DiskInterfaceType type);
    static QString mediaTypeString(MediaType type);
    static QString filesystemString(FilesystemType fs);
    static QString partitionTableTypeString(PartitionTableType pt);

    QSplitter* m_mainSplitter = nullptr;
    QSplitter* m_rightSplitter = nullptr;

    // Left panel: disk tree
    QTreeView* m_diskTree = nullptr;
    QStandardItemModel* m_diskTreeModel = nullptr;

    // Center: partition map
    DiskMapWidget* m_diskMap = nullptr;

    // Bottom: partition detail table
    QTableView* m_partitionTable = nullptr;
    QStandardItemModel* m_partitionModel = nullptr;

    // Right: operation list
    QListWidget* m_operationListWidget = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_undoBtn = nullptr;
    QPushButton* m_clearBtn = nullptr;

    // Backend
    OperationQueue m_opQueue;
    SystemDiskSnapshot m_snapshot;
    DiskId m_selectedDiskId = -1;
};

} // namespace spw
