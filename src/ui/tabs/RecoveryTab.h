#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/recovery/PartitionRecovery.h"
#include "core/recovery/FileRecovery.h"

#include <QWidget>

class QComboBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QTableWidget;
class QCheckBox;
class QLineEdit;

namespace spw
{

class RecoveryTab : public QWidget
{
    Q_OBJECT

public:
    explicit RecoveryTab(QWidget* parent = nullptr);
    ~RecoveryTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onRecoveryTypeChanged();
    void onStartPartitionScan();
    void onStartFileRecoveryScan();
    void onRecoverSelectedPartitions();
    void onRecoverSelectedFiles();
    void onStartBootRepair();
    void onBrowseOutputFolder();

private:
    void setupUi();
    void setupPartitionRecoveryPage();
    void setupFileRecoveryPage();
    void setupBootRepairPage();
    void populateDiskCombo();
    void populatePartitionCombo();

    static QString formatSize(uint64_t bytes);

    // UI elements
    QComboBox* m_diskCombo = nullptr;

    // Recovery type buttons
    QPushButton* m_partRecoveryBtn = nullptr;
    QPushButton* m_fileRecoveryBtn = nullptr;
    QPushButton* m_bootRepairBtn = nullptr;

    // Stacked pages
    QStackedWidget* m_stackedWidget = nullptr;

    // Partition Recovery page
    QRadioButton* m_quickScanRadio = nullptr;
    QRadioButton* m_deepScanRadio = nullptr;
    QPushButton* m_partScanBtn = nullptr;
    QProgressBar* m_partScanProgress = nullptr;
    QTableWidget* m_partResultsTable = nullptr;
    QPushButton* m_recoverPartBtn = nullptr;

    // File Recovery page
    QComboBox* m_partitionCombo = nullptr;
    QComboBox* m_fileTypeFilter = nullptr;
    QPushButton* m_fileScanBtn = nullptr;
    QProgressBar* m_fileScanProgress = nullptr;
    QTableWidget* m_fileResultsTable = nullptr;
    QPushButton* m_recoverFileBtn = nullptr;
    QLineEdit* m_outputFolderEdit = nullptr;
    QPushButton* m_browseOutputBtn = nullptr;

    // Boot Repair page
    QCheckBox* m_repairMbr = nullptr;
    QCheckBox* m_repairGpt = nullptr;
    QCheckBox* m_repairBootSector = nullptr;
    QCheckBox* m_repairBcd = nullptr;
    QCheckBox* m_repairBootloader = nullptr;
    QPushButton* m_bootRepairStartBtn = nullptr;
    QProgressBar* m_bootRepairProgress = nullptr;
    QLabel* m_bootRepairStatus = nullptr;

    // Data
    SystemDiskSnapshot m_snapshot;
    std::vector<RecoveredPartition> m_recoveredPartitions;
    std::vector<RecoverableFile> m_recoveredFiles;
    std::atomic<bool> m_cancelFlag{false};
};

} // namespace spw
