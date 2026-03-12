#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/disk/SmartReader.h"
#include "core/diagnostics/Benchmark.h"
#include "core/diagnostics/SurfaceScan.h"

#include <QWidget>
#include <atomic>

class QComboBox;
class QGroupBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTableWidget;

namespace spw
{

class DiagnosticsTab : public QWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsTab(QWidget* parent = nullptr);
    ~DiagnosticsTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onDiskChanged(int index);
    void onRefreshSmart();
    void onRunBenchmark();
    void onStartSurfaceScan();

private:
    void setupUi();
    void populateDiskCombo();
    void displaySmartData(const SmartData& data);
    void clearSmartData();
    void updateBenchmarkDisplay(const BenchmarkResults& results);
    void clearBenchmarkDisplay();

    static QString formatSize(uint64_t bytes);
    static QString smartStatusString(SmartStatus status);
    static QColor smartStatusColor(SmartStatus status);

    // Disk selector
    QComboBox* m_diskCombo = nullptr;
    QPushButton* m_refreshBtn = nullptr;

    // SMART section
    QGroupBox* m_smartGroup = nullptr;
    QLabel* m_healthIcon = nullptr;
    QLabel* m_healthLabel = nullptr;
    QTableWidget* m_smartTable = nullptr;

    // Benchmark section
    QGroupBox* m_benchGroup = nullptr;
    QProgressBar* m_seqReadBar = nullptr;
    QProgressBar* m_seqWriteBar = nullptr;
    QProgressBar* m_rnd4kReadBar = nullptr;
    QProgressBar* m_rnd4kWriteBar = nullptr;
    QLabel* m_seqReadLabel = nullptr;
    QLabel* m_seqWriteLabel = nullptr;
    QLabel* m_rnd4kReadLabel = nullptr;
    QLabel* m_rnd4kWriteLabel = nullptr;
    QLabel* m_iopsLabel = nullptr;
    QLabel* m_latencyLabel = nullptr;
    QPushButton* m_benchBtn = nullptr;

    // Surface Scan section
    QGroupBox* m_scanGroup = nullptr;
    QRadioButton* m_readOnlyRadio = nullptr;
    QRadioButton* m_readWriteRadio = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QProgressBar* m_scanProgress = nullptr;
    QLabel* m_scanBadCountLabel = nullptr;
    QLabel* m_scanSpeedLabel = nullptr;

    // Data
    SystemDiskSnapshot m_snapshot;
    SmartData m_currentSmart;
    BenchmarkResults m_currentBench;
    std::atomic<bool> m_cancelFlag{false};
};

} // namespace spw
