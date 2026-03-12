#include "DiagnosticsTab.h"

#include "core/disk/DiskEnumerator.h"
#include "core/disk/RawDiskHandle.h"
#include "core/disk/SmartReader.h"
#include "core/diagnostics/Benchmark.h"
#include "core/diagnostics/SurfaceScan.h"

#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

namespace spw
{

DiagnosticsTab::DiagnosticsTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

DiagnosticsTab::~DiagnosticsTab() = default;

void DiagnosticsTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    // Disk selector row
    auto* selectorLayout = new QHBoxLayout();
    selectorLayout->addWidget(new QLabel(tr("Select Disk:")));
    m_diskCombo = new QComboBox();
    connect(m_diskCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &DiagnosticsTab::onDiskChanged);
    selectorLayout->addWidget(m_diskCombo, 1);
    m_refreshBtn = new QPushButton(tr("Refresh"));
    connect(m_refreshBtn, &QPushButton::clicked, this, &DiagnosticsTab::onRefreshSmart);
    selectorLayout->addWidget(m_refreshBtn);
    layout->addLayout(selectorLayout);

    auto* splitter = new QSplitter(Qt::Horizontal);

    // ===== S.M.A.R.T. Panel =====
    m_smartGroup = new QGroupBox(tr("S.M.A.R.T. Health"));
    auto* smartLayout = new QVBoxLayout(m_smartGroup);

    auto* healthRow = new QHBoxLayout();
    m_healthIcon = new QLabel();
    m_healthIcon->setFixedSize(48, 48);
    m_healthIcon->setAlignment(Qt::AlignCenter);
    healthRow->addWidget(m_healthIcon);
    m_healthLabel = new QLabel(tr("Overall Health: --"));
    m_healthLabel->setStyleSheet("font-size: 16px; font-weight: bold; padding: 8px;");
    healthRow->addWidget(m_healthLabel, 1);
    smartLayout->addLayout(healthRow);

    m_smartTable = new QTableWidget(0, 7);
    m_smartTable->setHorizontalHeaderLabels(
        {tr("ID"), tr("Attribute"), tr("Value"), tr("Worst"), tr("Threshold"),
         tr("Raw Value"), tr("Status")});
    m_smartTable->setAlternatingRowColors(true);
    m_smartTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_smartTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_smartTable->horizontalHeader()->setStretchLastSection(true);
    smartLayout->addWidget(m_smartTable);

    splitter->addWidget(m_smartGroup);

    // ===== Right Panel: Benchmark + Surface Scan =====
    auto* rightPanel = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightPanel);

    // Benchmark
    m_benchGroup = new QGroupBox(tr("Benchmark"));
    auto* benchLayout = new QVBoxLayout(m_benchGroup);

    auto* benchGrid = new QGridLayout();
    benchGrid->addWidget(new QLabel(tr("Sequential Read:")), 0, 0);
    m_seqReadBar = new QProgressBar();
    m_seqReadBar->setRange(0, 7000);
    m_seqReadBar->setValue(0);
    m_seqReadBar->setFormat("%v MB/s");
    benchGrid->addWidget(m_seqReadBar, 0, 1);
    m_seqReadLabel = new QLabel(tr("-- MB/s"));
    benchGrid->addWidget(m_seqReadLabel, 0, 2);

    benchGrid->addWidget(new QLabel(tr("Sequential Write:")), 1, 0);
    m_seqWriteBar = new QProgressBar();
    m_seqWriteBar->setRange(0, 7000);
    m_seqWriteBar->setValue(0);
    m_seqWriteBar->setFormat("%v MB/s");
    benchGrid->addWidget(m_seqWriteBar, 1, 1);
    m_seqWriteLabel = new QLabel(tr("-- MB/s"));
    benchGrid->addWidget(m_seqWriteLabel, 1, 2);

    benchGrid->addWidget(new QLabel(tr("Random 4K Read:")), 2, 0);
    m_rnd4kReadBar = new QProgressBar();
    m_rnd4kReadBar->setRange(0, 1000000);
    m_rnd4kReadBar->setValue(0);
    m_rnd4kReadBar->setFormat("%v IOPS");
    benchGrid->addWidget(m_rnd4kReadBar, 2, 1);
    m_rnd4kReadLabel = new QLabel(tr("-- IOPS"));
    benchGrid->addWidget(m_rnd4kReadLabel, 2, 2);

    benchGrid->addWidget(new QLabel(tr("Random 4K Write:")), 3, 0);
    m_rnd4kWriteBar = new QProgressBar();
    m_rnd4kWriteBar->setRange(0, 1000000);
    m_rnd4kWriteBar->setValue(0);
    m_rnd4kWriteBar->setFormat("%v IOPS");
    benchGrid->addWidget(m_rnd4kWriteBar, 3, 1);
    m_rnd4kWriteLabel = new QLabel(tr("-- IOPS"));
    benchGrid->addWidget(m_rnd4kWriteLabel, 3, 2);

    benchLayout->addLayout(benchGrid);

    m_iopsLabel = new QLabel(tr("QD32 IOPS: Read -- / Write --"));
    m_iopsLabel->setStyleSheet("font-family: monospace;");
    benchLayout->addWidget(m_iopsLabel);

    m_latencyLabel = new QLabel(tr("Latency: Read -- us / Write -- us"));
    m_latencyLabel->setStyleSheet("font-family: monospace;");
    benchLayout->addWidget(m_latencyLabel);

    m_benchBtn = new QPushButton(tr("Run Benchmark"));
    m_benchBtn->setObjectName("applyButton");
    connect(m_benchBtn, &QPushButton::clicked, this, &DiagnosticsTab::onRunBenchmark);
    benchLayout->addWidget(m_benchBtn);

    rightLayout->addWidget(m_benchGroup);

    // Surface Scan
    m_scanGroup = new QGroupBox(tr("Surface Scan"));
    auto* scanLayout = new QVBoxLayout(m_scanGroup);

    auto* modeRow = new QHBoxLayout();
    m_readOnlyRadio = new QRadioButton(tr("Read-Only (safe)"));
    m_readOnlyRadio->setChecked(true);
    m_readWriteRadio = new QRadioButton(tr("Read-Write (DESTRUCTIVE)"));
    m_readWriteRadio->setStyleSheet("color: red;");
    modeRow->addWidget(m_readOnlyRadio);
    modeRow->addWidget(m_readWriteRadio);
    scanLayout->addLayout(modeRow);

    m_scanProgress = new QProgressBar();
    m_scanProgress->setValue(0);
    scanLayout->addWidget(m_scanProgress);

    m_scanBadCountLabel = new QLabel(tr("Bad sectors: --"));
    scanLayout->addWidget(m_scanBadCountLabel);

    m_scanSpeedLabel = new QLabel(tr("Speed: -- MB/s"));
    scanLayout->addWidget(m_scanSpeedLabel);

    m_scanBtn = new QPushButton(tr("Start Surface Scan"));
    m_scanBtn->setObjectName("applyButton");
    connect(m_scanBtn, &QPushButton::clicked, this, &DiagnosticsTab::onStartSurfaceScan);
    scanLayout->addWidget(m_scanBtn);

    rightLayout->addWidget(m_scanGroup);
    rightLayout->addStretch();

    splitter->addWidget(rightPanel);

    layout->addWidget(splitter);
}

void DiagnosticsTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateDiskCombo();
}

void DiagnosticsTab::populateDiskCombo()
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

void DiagnosticsTab::onDiskChanged(int index)
{
    if (index < 0)
        return;
    onRefreshSmart();
}

void DiagnosticsTab::onRefreshSmart()
{
    int diskId = m_diskCombo->currentData().toInt();

    auto* thread = QThread::create([this, diskId]() {
        auto diskResult = RawDiskHandle::open(diskId, DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            return;

        auto& diskHandle = diskResult.value();
        auto smartResult = SmartReader::readSmartData(diskHandle.nativeHandle(), diskId);
        if (smartResult.isOk())
        {
            m_currentSmart = smartResult.value();
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        displaySmartData(m_currentSmart);
    });

    thread->start();
}

void DiagnosticsTab::displaySmartData(const SmartData& data)
{
    // Overall health
    QString healthText;
    QColor healthColor;
    switch (data.overallHealth)
    {
    case SmartStatus::OK:
        healthText = tr("PASSED - Healthy");
        healthColor = QColor(0, 180, 0);
        m_healthIcon->setStyleSheet("background-color: #00b400; border-radius: 24px;");
        break;
    case SmartStatus::Warning:
        healthText = tr("WARNING - Issues Detected");
        healthColor = QColor(255, 180, 0);
        m_healthIcon->setStyleSheet("background-color: #ffb400; border-radius: 24px;");
        break;
    case SmartStatus::Critical:
        healthText = tr("CRITICAL - Drive Failing");
        healthColor = QColor(255, 0, 0);
        m_healthIcon->setStyleSheet("background-color: #ff0000; border-radius: 24px;");
        break;
    default:
        healthText = tr("Unknown");
        healthColor = QColor(128, 128, 128);
        m_healthIcon->setStyleSheet("background-color: #808080; border-radius: 24px;");
        break;
    }

    m_healthLabel->setText(tr("Overall Health: %1").arg(healthText));
    m_healthLabel->setStyleSheet(QString("font-size: 16px; font-weight: bold; color: %1; padding: 8px;")
                                     .arg(healthColor.name()));

    // Attributes table
    m_smartTable->setRowCount(0);

    if (data.isNvme)
    {
        // Show NVMe health info as pseudo-attributes
        struct NvmeRow
        {
            QString name;
            QString value;
        };
        const auto& h = data.nvmeHealth;
        QVector<NvmeRow> rows = {
            {tr("Temperature"), QString("%1 C").arg(h.temperature > 0 ? h.temperature - 273 : 0)},
            {tr("Available Spare"), QString("%1%").arg(h.availableSpare)},
            {tr("Spare Threshold"), QString("%1%").arg(h.availableSpareThreshold)},
            {tr("Percentage Used"), QString("%1%").arg(h.percentageUsed)},
            {tr("Data Read"), formatSize(h.dataUnitsRead * 512000ULL)},
            {tr("Data Written"), formatSize(h.dataUnitsWritten * 512000ULL)},
            {tr("Power Cycles"), QString::number(h.powerCycles)},
            {tr("Power-On Hours"), QString::number(h.powerOnHours)},
            {tr("Unsafe Shutdowns"), QString::number(h.unsafeShutdowns)},
            {tr("Media Errors"), QString::number(h.mediaErrors)},
            {tr("Error Log Entries"), QString::number(h.errorLogEntries)},
        };

        for (int i = 0; i < rows.size(); ++i)
        {
            int row = m_smartTable->rowCount();
            m_smartTable->insertRow(row);
            m_smartTable->setItem(row, 0, new QTableWidgetItem(QString::number(i + 1)));
            m_smartTable->setItem(row, 1, new QTableWidgetItem(rows[i].name));
            m_smartTable->setItem(row, 2, new QTableWidgetItem(rows[i].value));
            m_smartTable->setItem(row, 3, new QTableWidgetItem("-"));
            m_smartTable->setItem(row, 4, new QTableWidgetItem("-"));
            m_smartTable->setItem(row, 5, new QTableWidgetItem(rows[i].value));
            m_smartTable->setItem(row, 6, new QTableWidgetItem(smartStatusString(SmartStatus::OK)));
        }
    }
    else
    {
        for (const auto& attr : data.attributes)
        {
            int row = m_smartTable->rowCount();
            m_smartTable->insertRow(row);

            m_smartTable->setItem(row, 0, new QTableWidgetItem(
                QString("0x%1").arg(attr.id, 2, 16, QChar('0')).toUpper()));
            m_smartTable->setItem(row, 1, new QTableWidgetItem(
                QString::fromStdString(attr.name)));
            m_smartTable->setItem(row, 2, new QTableWidgetItem(
                QString::number(attr.currentValue)));
            m_smartTable->setItem(row, 3, new QTableWidgetItem(
                QString::number(attr.worstValue)));
            m_smartTable->setItem(row, 4, new QTableWidgetItem(
                QString::number(attr.threshold)));
            m_smartTable->setItem(row, 5, new QTableWidgetItem(
                QString::number(attr.rawValue)));

            auto* statusItem = new QTableWidgetItem(smartStatusString(attr.status));
            statusItem->setForeground(smartStatusColor(attr.status));
            m_smartTable->setItem(row, 6, statusItem);
        }
    }

    m_smartTable->resizeColumnsToContents();
}

void DiagnosticsTab::clearSmartData()
{
    m_healthLabel->setText(tr("Overall Health: --"));
    m_healthLabel->setStyleSheet("font-size: 16px; font-weight: bold; padding: 8px;");
    m_healthIcon->setStyleSheet("background-color: #808080; border-radius: 24px;");
    m_smartTable->setRowCount(0);
}

void DiagnosticsTab::onRunBenchmark()
{
    int diskId = m_diskCombo->currentData().toInt();

    // Find a volume letter on this disk for benchmarking
    std::string volumePath;
    for (const auto& part : m_snapshot.partitions)
    {
        if (part.diskId == diskId && part.driveLetter != L'\0')
        {
            volumePath = std::string(1, static_cast<char>(part.driveLetter)) + ":\\";
            break;
        }
    }

    if (volumePath.empty())
    {
        QMessageBox::warning(this, tr("No Volume"),
                             tr("No mounted volume found on this disk for benchmarking."));
        return;
    }

    m_cancelFlag.store(false);
    m_benchBtn->setEnabled(false);
    clearBenchmarkDisplay();

    auto* thread = QThread::create([this, volumePath]() {
        Benchmark bench(volumePath);
        BenchmarkConfig config;
        config.durationSeconds = 5;

        auto result = bench.run(config,
            [this](BenchmarkPhase phase, int pct, const BenchmarkResults& partial) {
                Q_UNUSED(pct);
                Q_UNUSED(phase);
                QMetaObject::invokeMethod(this, [this, partial]() {
                    updateBenchmarkDisplay(partial);
                }, Qt::QueuedConnection);
            },
            &m_cancelFlag);

        if (result.isOk())
        {
            m_currentBench = result.value();
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_benchBtn->setEnabled(true);
        updateBenchmarkDisplay(m_currentBench);
        emit statusMessage(tr("Benchmark completed"));
    });

    thread->start();
}

void DiagnosticsTab::updateBenchmarkDisplay(const BenchmarkResults& r)
{
    m_seqReadBar->setValue(static_cast<int>(r.seqReadMBps));
    m_seqReadLabel->setText(QString("%1 MB/s").arg(r.seqReadMBps, 0, 'f', 1));

    m_seqWriteBar->setValue(static_cast<int>(r.seqWriteMBps));
    m_seqWriteLabel->setText(QString("%1 MB/s").arg(r.seqWriteMBps, 0, 'f', 1));

    m_rnd4kReadBar->setValue(static_cast<int>(r.rnd4kReadIOPS));
    m_rnd4kReadLabel->setText(QString("%1 IOPS").arg(r.rnd4kReadIOPS, 0, 'f', 0));

    m_rnd4kWriteBar->setValue(static_cast<int>(r.rnd4kWriteIOPS));
    m_rnd4kWriteLabel->setText(QString("%1 IOPS").arg(r.rnd4kWriteIOPS, 0, 'f', 0));

    m_iopsLabel->setText(QString("QD32 IOPS: Read %1 / Write %2")
                             .arg(r.rnd4kReadIOPS_QD32, 0, 'f', 0)
                             .arg(r.rnd4kWriteIOPS_QD32, 0, 'f', 0));

    m_latencyLabel->setText(QString("Latency: Read %1 us / Write %2 us")
                                .arg(r.avgReadLatencyUs, 0, 'f', 1)
                                .arg(r.avgWriteLatencyUs, 0, 'f', 1));
}

void DiagnosticsTab::clearBenchmarkDisplay()
{
    m_seqReadBar->setValue(0);
    m_seqWriteBar->setValue(0);
    m_rnd4kReadBar->setValue(0);
    m_rnd4kWriteBar->setValue(0);
    m_seqReadLabel->setText(tr("-- MB/s"));
    m_seqWriteLabel->setText(tr("-- MB/s"));
    m_rnd4kReadLabel->setText(tr("-- IOPS"));
    m_rnd4kWriteLabel->setText(tr("-- IOPS"));
    m_iopsLabel->setText(tr("QD32 IOPS: Read -- / Write --"));
    m_latencyLabel->setText(tr("Latency: Read -- us / Write -- us"));
}

void DiagnosticsTab::onStartSurfaceScan()
{
    int diskId = m_diskCombo->currentData().toInt();

    if (m_readWriteRadio->isChecked())
    {
        auto reply = QMessageBox::critical(this, tr("DESTRUCTIVE SCAN"),
                                           tr("Read-Write mode will DESTROY ALL DATA on this disk!\n\n"
                                              "Are you absolutely sure?"),
                                           QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes)
            return;
    }

    SurfaceScanMode mode = m_readOnlyRadio->isChecked()
                               ? SurfaceScanMode::ReadOnly
                               : SurfaceScanMode::WriteVerify;

    m_cancelFlag.store(false);
    m_scanBtn->setEnabled(false);
    m_scanProgress->setValue(0);
    m_scanBadCountLabel->setText(tr("Bad sectors: 0"));

    auto* thread = QThread::create([this, diskId, mode]() {
        auto diskResult = RawDiskHandle::open(diskId,
                                              mode == SurfaceScanMode::WriteVerify
                                                  ? DiskAccessMode::ReadWrite
                                                  : DiskAccessMode::ReadOnly);
        if (diskResult.isError())
            return;

        auto& disk = diskResult.value();
        SurfaceScan scan(disk);

        auto result = scan.scanDisk(mode,
            [this](uint64_t scanned, uint64_t total, uint64_t badCount,
                   double speedMBps, double /*eta*/) {
                int pct = total > 0 ? static_cast<int>((scanned * 100) / total) : 0;
                QMetaObject::invokeMethod(m_scanProgress, "setValue",
                                          Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_scanBadCountLabel, "setText",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, QString("Bad sectors: %1").arg(badCount)));
                QMetaObject::invokeMethod(m_scanSpeedLabel, "setText",
                                          Qt::QueuedConnection,
                                          Q_ARG(QString, QString("Speed: %1 MB/s").arg(speedMBps, 0, 'f', 1)));
            },
            &m_cancelFlag);

        if (result.isOk())
        {
            const auto& r = result.value();
            QMetaObject::invokeMethod(m_scanBadCountLabel, "setText",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, QString("Bad sectors: %1 / %2 tested")
                                                         .arg(r.badSectorCount)
                                                         .arg(r.totalSectorsTested)));
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_scanBtn->setEnabled(true);
        m_scanProgress->setValue(100);
        emit statusMessage(tr("Surface scan completed"));
    });

    thread->start();
}

QString DiagnosticsTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 2);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

QString DiagnosticsTab::smartStatusString(SmartStatus status)
{
    switch (status)
    {
    case SmartStatus::OK:       return QStringLiteral("OK");
    case SmartStatus::Warning:  return QStringLiteral("Warning");
    case SmartStatus::Critical: return QStringLiteral("CRITICAL");
    default:                     return QStringLiteral("Unknown");
    }
}

QColor DiagnosticsTab::smartStatusColor(SmartStatus status)
{
    switch (status)
    {
    case SmartStatus::OK:       return QColor(0, 180, 0);
    case SmartStatus::Warning:  return QColor(255, 180, 0);
    case SmartStatus::Critical: return QColor(255, 0, 0);
    default:                     return QColor(128, 128, 128);
    }
}

} // namespace spw
