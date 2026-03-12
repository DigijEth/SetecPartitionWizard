#include "SdCardTab.h"

#include "core/maintenance/SdCardRecovery.h"
#include "core/maintenance/SdCardAnalyzer.h"
#include "core/disk/DiskEnumerator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QFrame>

namespace spw
{

// ============================================================================
// Helpers
// ============================================================================

QString SdCardTab::formatSize(uint64_t bytes)
{
    if (bytes >= 1099511627776ULL)
        return QString("%1 TB").arg(bytes / 1099511627776.0, 0, 'f', 2);
    if (bytes >= 1073741824ULL)
        return QString("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
    if (bytes >= 1048576ULL)
        return QString("%1 MB").arg(bytes / 1048576.0, 0, 'f', 0);
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

QString SdCardTab::formatSpeed(double mbps)
{
    if (mbps <= 0) return tr("N/A");
    return QString("%1 MB/s").arg(mbps, 0, 'f', 1);
}

QString SdCardTab::verdictString(CounterfeitVerdict v)
{
    switch (v)
    {
    case CounterfeitVerdict::Genuine:       return tr("✓  GENUINE");
    case CounterfeitVerdict::LikelySpoofed: return tr("✗  COUNTERFEIT DETECTED");
    case CounterfeitVerdict::Suspicious:    return tr("⚠  SUSPICIOUS");
    case CounterfeitVerdict::TestFailed:    return tr("—  TEST FAILED");
    default:                               return tr("?  UNTESTED");
    }
}

QString SdCardTab::verdictStyle(CounterfeitVerdict v)
{
    switch (v)
    {
    case CounterfeitVerdict::Genuine:       return "color: #a8e6a0; font-size: 18px; font-weight: bold;";
    case CounterfeitVerdict::LikelySpoofed: return "color: #ff6b6b; font-size: 18px; font-weight: bold;";
    case CounterfeitVerdict::Suspicious:    return "color: #ffd93d; font-size: 18px; font-weight: bold;";
    default:                               return "color: #aaaaaa; font-size: 16px;";
    }
}

// ============================================================================
// Constructor
// ============================================================================

SdCardTab::SdCardTab(QWidget* parent) : QWidget(parent)
{
    setupUi();
}

SdCardTab::~SdCardTab() = default;

void SdCardTab::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    // ---- Top: card selector bar ----
    setupCardSelectorPanel();

    auto* selectorGroup = new QGroupBox(tr("SD / microSD Card Selection"));
    auto* selectorLayout = new QVBoxLayout(selectorGroup);

    auto* selectorRow = new QHBoxLayout();
    selectorRow->addWidget(m_scanBtn);
    selectorRow->addWidget(m_cardCombo, 1);
    selectorLayout->addLayout(selectorRow);
    selectorLayout->addWidget(m_cardSummaryLabel);

    mainLayout->addWidget(selectorGroup);

    // ---- Middle: inner tab widget ----
    m_innerTabs = new QTabWidget();

    // Tab 1: Card Info
    auto* infoWidget = new QWidget();
    setupInfoPanel();
    auto* infoLayout = new QVBoxLayout(infoWidget);
    {
        auto* form = new QGroupBox(tr("Device Information"));
        auto* fl = new QFormLayout(form);
        fl->setLabelAlignment(Qt::AlignRight);
        fl->addRow(tr("Model:"),        m_infoModel);
        fl->addRow(tr("Vendor:"),       m_infoVendor);
        fl->addRow(tr("Manufacturer:"), m_infoManufacturer);
        fl->addRow(tr("Serial:"),       m_infoSerial);
        fl->addRow(tr("Capacity:"),     m_infoCapacity);
        fl->addRow(tr("Bus Type:"),     m_infoBusType);
        fl->addRow(tr("Interface:"),    m_infoInterface);
        fl->addRow(tr("Write Protect:"),m_infoWriteProt);
        fl->addRow(tr("Status:"),       m_infoStatus);
        infoLayout->addWidget(form);
        infoLayout->addStretch();
        auto* refreshBtn = new QPushButton(tr("Refresh Info"));
        connect(refreshBtn, &QPushButton::clicked, this, &SdCardTab::onRefreshInfo);
        infoLayout->addWidget(refreshBtn);
    }
    m_innerTabs->addTab(infoWidget, tr("Card Info"));

    // Tab 2: Counterfeit Detection
    auto* cntWidget = new QWidget();
    setupCounterfeitPanel();
    auto* cntLayout = new QVBoxLayout(cntWidget);
    {
        auto* explainLabel = new QLabel(
            tr("Counterfeit SD cards report a large capacity (e.g. 64 GB) but contain much less "
               "real NAND flash (e.g. 2–4 GB). Data written beyond the real capacity silently "
               "wraps and overwrites earlier data.\n\n"
               "This test writes unique signatures at geometrically distributed positions across "
               "the disk and reads them back. It restores original data after each probe."));
        explainLabel->setWordWrap(true);
        explainLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
        cntLayout->addWidget(explainLabel);

        auto* warnLabel = new QLabel(
            tr("⚠ This test writes to the card. Keep the card inserted until complete."));
        warnLabel->setWordWrap(true);
        warnLabel->setStyleSheet("color: #ffd93d; font-weight: bold;");
        cntLayout->addWidget(warnLabel);

        cntLayout->addWidget(m_counterVerdict);
        cntLayout->addWidget(m_counterProgress);
        cntLayout->addWidget(m_counterLog, 1);
        cntLayout->addWidget(m_counterBtn);
    }
    m_innerTabs->addTab(cntWidget, tr("Counterfeit Check"));

    // Tab 3: Speed Test
    auto* speedWidget = new QWidget();
    setupSpeedPanel();
    auto* speedLayout = new QVBoxLayout(speedWidget);
    {
        auto* explainLabel = new QLabel(
            tr("Benchmarks sequential read/write speeds and random 4K IOPS. "
               "Compare against the card's rated speed class:\n"
               "  Class 10 / UHS-I: ≥10 MB/s seq write\n"
               "  UHS-I U3 / V30: ≥30 MB/s seq write\n"
               "  V60: ≥60 MB/s  •  V90: ≥90 MB/s"));
        explainLabel->setWordWrap(true);
        explainLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
        speedLayout->addWidget(explainLabel);

        auto* resultsGroup = new QGroupBox(tr("Results"));
        auto* rfl = new QFormLayout(resultsGroup);
        rfl->setLabelAlignment(Qt::AlignRight);
        rfl->addRow(tr("Sequential Read:"),   m_speedSeqRead);
        rfl->addRow(tr("Sequential Write:"),  m_speedSeqWrite);
        rfl->addRow(tr("Random 4K Read:"),    m_speedRandRead);
        rfl->addRow(tr("Random 4K Write:"),   m_speedRandWrite);
        rfl->addRow(tr("Notes:"),             m_speedNotes);
        speedLayout->addWidget(resultsGroup);
        speedLayout->addWidget(m_speedProgress);
        speedLayout->addWidget(m_speedBtn);
        speedLayout->addStretch();
    }
    m_innerTabs->addTab(speedWidget, tr("Speed Test"));

    // Tab 4: Surface Scan / Health
    auto* healthWidget = new QWidget();
    setupHealthPanel();
    auto* healthLayout = new QVBoxLayout(healthWidget);
    {
        auto* explainLabel = new QLabel(
            tr("Reads every sector on the card to find bad or slow sectors. "
               "Even one bad sector can cause data corruption. "
               "A slow sector (>500ms read) often precedes failure."));
        explainLabel->setWordWrap(true);
        explainLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
        healthLayout->addWidget(explainLabel);

        auto* statsGroup = new QGroupBox(tr("Scan Results"));
        auto* sfl = new QFormLayout(statsGroup);
        sfl->setLabelAlignment(Qt::AlignRight);
        sfl->addRow(tr("Sectors Scanned:"), m_healthScanned);
        sfl->addRow(tr("Bad Sectors:"),     m_healthBad);
        sfl->addRow(tr("Slow Sectors:"),    m_healthSlow);
        sfl->addRow(tr("Overall:"),         m_healthResult);
        healthLayout->addWidget(statsGroup);
        healthLayout->addWidget(m_healthProgress);

        auto* btnRow = new QHBoxLayout();
        btnRow->addWidget(m_scanSurfaceBtn);
        btnRow->addWidget(m_cancelScanBtn);
        healthLayout->addLayout(btnRow);
        healthLayout->addStretch();
    }
    m_innerTabs->addTab(healthWidget, tr("Surface Scan"));

    // Tab 5: Repair / Format
    auto* repairWidget = new QWidget();
    setupRepairPanel();
    auto* repairLayout = new QVBoxLayout(repairWidget);
    {
        auto* explainLabel = new QLabel(
            tr("Repair a card that Windows cannot see. This cleans the partition table, "
               "creates a new partition, and formats it. All existing data will be erased."));
        explainLabel->setWordWrap(true);
        explainLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
        repairLayout->addWidget(explainLabel);

        auto* optGroup = new QGroupBox(tr("Format Options"));
        auto* ofl = new QFormLayout(optGroup);
        ofl->setLabelAlignment(Qt::AlignRight);
        ofl->addRow(tr("Filesystem:"),   m_repairFsCombo);
        ofl->addRow(tr("Volume Label:"), m_repairLabel);
        ofl->addRow(tr("Clean Table:"),  m_repairCleanChk);
        repairLayout->addWidget(optGroup);
        repairLayout->addWidget(m_repairProgress);
        repairLayout->addWidget(m_repairStatus);

        auto* btnRow = new QHBoxLayout();
        btnRow->addWidget(m_repairBtn);
        btnRow->addWidget(m_eraseBtn);
        repairLayout->addLayout(btnRow);
        repairLayout->addStretch();
    }
    m_innerTabs->addTab(repairWidget, tr("Repair / Format"));

    mainLayout->addWidget(m_innerTabs, 1);
}

void SdCardTab::setupCardSelectorPanel()
{
    m_scanBtn = new QPushButton(tr("Scan for Cards"));
    m_scanBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(m_scanBtn, &QPushButton::clicked, this, &SdCardTab::onScanCards);

    m_cardCombo = new QComboBox();
    m_cardCombo->setPlaceholderText(tr("Click 'Scan for Cards' to detect SD/MMC media..."));
    connect(m_cardCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SdCardTab::onCardSelected);

    m_cardSummaryLabel = new QLabel(tr("No card selected."));
    m_cardSummaryLabel->setStyleSheet("color: #aaaaaa; font-style: italic;");
}

void SdCardTab::setupInfoPanel()
{
    auto makeInfo = [](const QString& def = tr("—")) -> QLabel* {
        auto* l = new QLabel(def);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return l;
    };
    m_infoModel       = makeInfo();
    m_infoVendor      = makeInfo();
    m_infoManufacturer= makeInfo();
    m_infoSerial      = makeInfo();
    m_infoCapacity    = makeInfo();
    m_infoBusType     = makeInfo();
    m_infoInterface   = makeInfo();
    m_infoWriteProt   = makeInfo();
    m_infoStatus      = makeInfo();
}

void SdCardTab::setupCounterfeitPanel()
{
    m_counterVerdict = new QLabel(tr("Not tested yet"));
    m_counterVerdict->setAlignment(Qt::AlignCenter);
    m_counterVerdict->setStyleSheet("color: #aaaaaa; font-size: 18px; padding: 12px;");
    m_counterVerdict->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    m_counterProgress = new QProgressBar();
    m_counterProgress->setVisible(false);
    m_counterProgress->setRange(0, 100);

    m_counterLog = new QTextEdit();
    m_counterLog->setReadOnly(true);
    m_counterLog->setPlaceholderText(tr("Test output will appear here..."));
    m_counterLog->setFont(QFont("Courier New", 9));

    m_counterBtn = new QPushButton(tr("Run Counterfeit Check"));
    m_counterBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-size: 14px; "
        "font-weight: bold; border-radius: 5px; }"
        "QPushButton:hover { background-color: #e0b584; }"
        "QPushButton:disabled { background-color: #555; color: #888; }");
    connect(m_counterBtn, &QPushButton::clicked, this, &SdCardTab::onCheckCounterfeit);
}

void SdCardTab::setupSpeedPanel()
{
    auto makeResult = [](const QString& def = tr("—")) -> QLabel* {
        auto* l = new QLabel(def);
        l->setStyleSheet("font-size: 14px; font-weight: bold;");
        return l;
    };
    m_speedSeqRead   = makeResult();
    m_speedSeqWrite  = makeResult();
    m_speedRandRead  = makeResult();
    m_speedRandWrite = makeResult();
    m_speedNotes     = new QLabel(tr("—"));
    m_speedNotes->setWordWrap(true);
    m_speedNotes->setStyleSheet("color: #aaaaaa;");

    m_speedProgress = new QProgressBar();
    m_speedProgress->setVisible(false);

    m_speedBtn = new QPushButton(tr("Run Speed Test"));
    connect(m_speedBtn, &QPushButton::clicked, this, &SdCardTab::onRunSpeedTest);
}

void SdCardTab::setupHealthPanel()
{
    m_healthScanned = new QLabel(tr("—"));
    m_healthBad     = new QLabel(tr("—"));
    m_healthSlow    = new QLabel(tr("—"));
    m_healthResult  = new QLabel(tr("—"));
    m_healthResult->setStyleSheet("font-weight: bold;");

    m_healthProgress = new QProgressBar();
    m_healthProgress->setVisible(false);

    m_scanSurfaceBtn = new QPushButton(tr("Start Surface Scan"));
    connect(m_scanSurfaceBtn, &QPushButton::clicked, this, &SdCardTab::onSurfaceScan);

    m_cancelScanBtn = new QPushButton(tr("Cancel"));
    m_cancelScanBtn->setEnabled(false);
    connect(m_cancelScanBtn, &QPushButton::clicked, this, &SdCardTab::onCancelOperation);
}

void SdCardTab::setupRepairPanel()
{
    m_repairFsCombo = new QComboBox();
    m_repairFsCombo->addItems({
        tr("FAT32  (recommended for ≤ 32 GB)"),
        tr("exFAT  (recommended for > 32 GB)"),
        tr("NTFS   (Windows only)")
    });

    m_repairLabel = new QLineEdit(QStringLiteral("SD_CARD"));
    m_repairLabel->setMaxLength(11);
    m_repairLabel->setPlaceholderText(tr("Max 11 characters"));

    m_repairCleanChk = new QCheckBox(
        tr("Clean partition table (required for unreadable cards)"));
    m_repairCleanChk->setChecked(true);

    m_repairProgress = new QProgressBar();
    m_repairProgress->setVisible(false);

    m_repairStatus = new QLabel();
    m_repairStatus->setWordWrap(true);

    m_repairBtn = new QPushButton(tr("Repair && Format"));
    m_repairBtn->setStyleSheet(
        "QPushButton { background-color: #d4a574; color: #1e1e2e; font-size: 13px; "
        "font-weight: bold; border-radius: 5px; }"
        "QPushButton:hover { background-color: #e0b584; }");
    connect(m_repairBtn, &QPushButton::clicked, this, &SdCardTab::onRepairCard);

    m_eraseBtn = new QPushButton(tr("Secure Erase"));
    m_eraseBtn->setStyleSheet(
        "QPushButton { background-color: #cc3333; color: white; font-size: 13px; "
        "font-weight: bold; border-radius: 5px; }"
        "QPushButton:hover { background-color: #ee4444; }");
    connect(m_eraseBtn, &QPushButton::clicked, this, &SdCardTab::onSecureErase);
}

// ============================================================================
// refreshDisks — called when main disk list updates
// ============================================================================
void SdCardTab::refreshDisks(const SystemDiskSnapshot& /*snapshot*/)
{
    // Don't auto-scan on disk refresh — only when user explicitly clicks Scan
}

// ============================================================================
// Scan for SD cards
// ============================================================================
void SdCardTab::onScanCards()
{
    if (m_operationRunning) return;

    m_scanBtn->setEnabled(false);
    m_cardCombo->clear();
    m_cards.clear();
    m_cardSummaryLabel->setText(tr("Scanning..."));

    auto* thread = QThread::create([this]() {
        auto result = SdCardRecovery::detectSdCards();
        if (result.isOk())
            m_cards = std::move(result.value());
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        m_scanBtn->setEnabled(true);

        if (m_cards.empty())
        {
            m_cardSummaryLabel->setText(
                tr("No SD/MMC cards found. Make sure the card is inserted and the reader is connected."));
            emit statusMessage(tr("SD card scan: no cards found"));
            return;
        }

        for (const auto& card : m_cards)
        {
            QString statusStr;
            switch (card.status)
            {
            case SdCardStatus::Healthy:         statusStr = tr("Healthy"); break;
            case SdCardStatus::NoPartitionTable: statusStr = tr("No Partition Table"); break;
            case SdCardStatus::CorruptPartition: statusStr = tr("Corrupt"); break;
            case SdCardStatus::RawFilesystem:    statusStr = tr("RAW"); break;
            case SdCardStatus::NoMedia:          statusStr = tr("No Media"); break;
            default:                             statusStr = tr("Unknown"); break;
            }

            m_cardCombo->addItem(
                QString("Disk %1 — %2  [%3]  %4")
                    .arg(card.diskId)
                    .arg(QString::fromStdWString(card.model))
                    .arg(formatSize(card.sizeBytes))
                    .arg(statusStr),
                card.diskId);
        }

        m_cardSummaryLabel->setText(tr("Found %1 card(s).").arg(m_cards.size()));
        emit statusMessage(tr("SD card scan complete — %1 card(s)").arg(m_cards.size()));

        if (!m_cards.empty())
            onCardSelected(0);
    });

    thread->start();
}

// ============================================================================
// Card selected — update summary and fetch identity
// ============================================================================
void SdCardTab::onCardSelected(int index)
{
    if (index < 0 || index >= static_cast<int>(m_cards.size()))
        return;

    const auto& card = m_cards[static_cast<size_t>(index)];

    // Basic status label
    QString statusStr;
    switch (card.status)
    {
    case SdCardStatus::Healthy:          statusStr = tr("✓ Healthy"); break;
    case SdCardStatus::NoPartitionTable: statusStr = tr("✗ No Partition Table — needs repair"); break;
    case SdCardStatus::CorruptPartition: statusStr = tr("⚠ Corrupt — needs repair"); break;
    case SdCardStatus::RawFilesystem:    statusStr = tr("⚠ RAW filesystem — needs formatting"); break;
    case SdCardStatus::NoMedia:          statusStr = tr("— No media in reader"); break;
    default:                             statusStr = tr("? Unknown"); break;
    }

    m_cardSummaryLabel->setText(QString("  %1  |  %2  |  %3")
        .arg(QString::fromStdWString(card.model))
        .arg(formatSize(card.sizeBytes))
        .arg(statusStr));

    // Update basic info immediately
    m_infoModel->setText(QString::fromStdWString(card.model));
    m_infoCapacity->setText(formatSize(card.sizeBytes));
    m_infoInterface->setText(card.interfaceType == DiskInterfaceType::MMC ? tr("MMC/SD native") :
                             card.interfaceType == DiskInterfaceType::USB  ? tr("USB reader") : tr("Other"));
    m_infoStatus->setText(statusStr);
    m_infoWriteProt->setText(SdCardAnalyzer::isWriteProtected(card.diskId) ? tr("Write Protected") : tr("Writable"));

    // Fetch full identity in background
    int diskId = card.diskId;
    auto* thread = QThread::create([this, diskId]() {
        auto idResult = SdCardAnalyzer::queryIdentity(diskId);
        if (idResult.isOk())
        {
            const auto& id = idResult.value();
            QMetaObject::invokeMethod(this, [this, id]() {
                m_infoVendor->setText(id.vendorId.empty() ? tr("—") : QString::fromStdWString(id.vendorId));
                m_infoSerial->setText(id.serialNumberStr.empty() ? tr("—") : QString::fromStdWString(id.serialNumberStr));
                m_infoBusType->setText(id.busType.empty() ? tr("—") : QString::fromStdWString(id.busType));
                if (id.cidValid)
                {
                    QString mfr = QString::fromLatin1(SdCardAnalyzer::manufacturerName(id.manufacturerId));
                    m_infoManufacturer->setText(QString("%1 (MID 0x%2)")
                        .arg(mfr).arg(id.manufacturerId, 2, 16, QChar('0')));
                }
                else
                {
                    m_infoManufacturer->setText(id.productId.empty() ? tr("—") : QString::fromStdWString(id.productId));
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ============================================================================
// Refresh info
// ============================================================================
void SdCardTab::onRefreshInfo()
{
    int idx = m_cardCombo->currentIndex();
    if (idx >= 0 && idx < static_cast<int>(m_cards.size()))
        onCardSelected(idx);
}

// ============================================================================
// Counterfeit check
// ============================================================================
void SdCardTab::onCheckCounterfeit()
{
    int idx = m_cardCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cards.size())) return;

    auto reply = QMessageBox::warning(this, tr("Counterfeit Check"),
        tr("This test writes small probe signatures to the card to verify actual capacity.\n"
           "It will restore the original data after each probe, but keep the card inserted.\n\n"
           "Continue?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    int diskId = m_cards[static_cast<size_t>(idx)].diskId;

    setOperationRunning(true);
    m_counterProgress->setVisible(true);
    m_counterProgress->setValue(0);
    m_counterLog->clear();
    m_counterVerdict->setText(tr("Testing..."));
    m_counterVerdict->setStyleSheet("color: #aaaaaa; font-size: 16px;");

    auto* thread = QThread::create([this, diskId]() {
        auto result = SdCardAnalyzer::checkCounterfeit(diskId,
            [this](const std::string& stage, int pct) {
                QMetaObject::invokeMethod(m_counterProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_counterLog, "append",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(stage)));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            m_counterProgress->setVisible(false);

            if (result.isError())
            {
                m_counterVerdict->setText(tr("Error: %1").arg(
                    QString::fromStdString(result.error().message)));
                m_counterVerdict->setStyleSheet("color: #ff6b6b; font-size: 14px;");
                return;
            }

            const auto& r = result.value();
            m_counterVerdict->setText(verdictString(r.verdict));
            m_counterVerdict->setStyleSheet(verdictStyle(r.verdict));

            m_counterLog->append(QString("\n=== RESULT ==="));
            m_counterLog->append(QString::fromStdString(r.summaryMessage));
            m_counterLog->append(QString("Reported capacity: %1").arg(formatSize(r.reportedCapacityBytes)));
            if (r.verifiedCapacityBytes != r.reportedCapacityBytes)
                m_counterLog->append(QString("Verified capacity: ~%1").arg(formatSize(r.verifiedCapacityBytes)));
            m_counterLog->append(QString("Probes: %1 total, %2 failed (%.0f%%)")
                .arg(r.probeCount).arg(r.failCount).arg(r.failPercent));
            if (!r.manufacturerName.empty())
                m_counterLog->append(QString("Manufacturer: %1%2")
                    .arg(QString::fromStdString(r.manufacturerName))
                    .arg(r.unknownManufacturer ? tr(" [UNVERIFIED]") : QString()));
            if (r.suspiciousVendorString)
                m_counterLog->append(tr("⚠ Generic/suspicious vendor string"));

            emit statusMessage(tr("Counterfeit check complete"));
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() {
        setOperationRunning(false);
    });
    thread->start();
}

// ============================================================================
// Speed test
// ============================================================================
void SdCardTab::onRunSpeedTest()
{
    int idx = m_cardCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cards.size())) return;

    int diskId = m_cards[static_cast<size_t>(idx)].diskId;

    setOperationRunning(true);
    m_speedProgress->setVisible(true);
    m_speedProgress->setValue(0);
    m_speedSeqRead->setText(tr("Testing..."));
    m_speedSeqWrite->setText(tr("Testing..."));
    m_speedRandRead->setText(tr("Testing..."));
    m_speedRandWrite->setText(tr("Testing..."));
    m_speedNotes->setText(tr("—"));

    auto* thread = QThread::create([this, diskId]() {
        auto result = SdCardAnalyzer::benchmarkSpeed(diskId, 64 * 1024 * 1024,
            [this](const std::string& stage, int pct) {
                QMetaObject::invokeMethod(m_speedProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, pct));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            m_speedProgress->setVisible(false);
            if (result.isError()) return;
            const auto& r = result.value();

            auto styleForSpeed = [](double mbps, double threshold) -> QString {
                if (mbps <= 0) return "color: #888;";
                return mbps >= threshold ? "color: #a8e6a0; font-size: 14px; font-weight: bold;"
                                         : "color: #ff9944; font-size: 14px; font-weight: bold;";
            };

            m_speedSeqRead->setText(formatSpeed(r.seqReadMBps));
            m_speedSeqRead->setStyleSheet(styleForSpeed(r.seqReadMBps, 10.0));

            m_speedSeqWrite->setText(formatSpeed(r.seqWriteMBps));
            m_speedSeqWrite->setStyleSheet(styleForSpeed(r.seqWriteMBps, 10.0));

            m_speedRandRead->setText(QString("%1 IOPS").arg(r.randRead4kIOPS, 0, 'f', 0));
            m_speedRandWrite->setText(r.writeProtected ? tr("(write protected)")
                                     : QString("%1 IOPS").arg(r.randWrite4kIOPS, 0, 'f', 0));
            m_speedNotes->setText(r.notes.empty() ? tr("—") : QString::fromStdString(r.notes));

            emit statusMessage(tr("Speed test complete — %.1f MB/s read, %.1f MB/s write")
                .arg(r.seqReadMBps).arg(r.seqWriteMBps));
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() { setOperationRunning(false); });
    thread->start();
}

// ============================================================================
// Surface scan
// ============================================================================
void SdCardTab::onSurfaceScan()
{
    int idx = m_cardCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cards.size())) return;

    int diskId = m_cards[static_cast<size_t>(idx)].diskId;

    setOperationRunning(true);
    m_cancelFlag.store(false);
    m_healthProgress->setVisible(true);
    m_healthProgress->setValue(0);
    m_healthScanned->setText(tr("0"));
    m_healthBad->setText(tr("0"));
    m_healthSlow->setText(tr("0"));
    m_healthResult->setText(tr("Scanning..."));
    m_cancelScanBtn->setEnabled(true);

    auto* thread = QThread::create([this, diskId]() {
        auto result = SdCardAnalyzer::surfaceScan(diskId, &m_cancelFlag,
            [this](uint64_t cur, uint64_t total, uint64_t bad, int pct) {
                QMetaObject::invokeMethod(m_healthProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_healthScanned, "setText", Qt::QueuedConnection,
                    Q_ARG(QString, QString::number(cur)));
                QMetaObject::invokeMethod(m_healthBad, "setText", Qt::QueuedConnection,
                    Q_ARG(QString, bad > 0
                        ? QString("<span style='color:#ff6b6b'>%1</span>").arg(bad)
                        : QString("0")));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            m_healthProgress->setVisible(false);
            m_cancelScanBtn->setEnabled(false);
            if (result.isError()) return;
            const auto& r = result.value();
            m_healthScanned->setText(QString::number(r.sectorsScanned));
            m_healthBad->setText(r.badSectors > 0
                ? QString("<span style='color:#ff6b6b'>%1</span>").arg(r.badSectors)
                : tr("0 ✓"));
            m_healthSlow->setText(r.slowSectors > 0
                ? QString("<span style='color:#ffd93d'>%1</span>").arg(r.slowSectors)
                : tr("0"));

            if (r.badSectors == 0 && r.slowSectors == 0)
                m_healthResult->setStyleSheet("color: #a8e6a0; font-weight: bold;");
            else
                m_healthResult->setStyleSheet("color: #ff9944; font-weight: bold;");
            m_healthResult->setText(QString::fromStdString(r.summary));
            emit statusMessage(tr("Surface scan complete"));
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() { setOperationRunning(false); });
    thread->start();
}

// ============================================================================
// Repair / Format
// ============================================================================
void SdCardTab::onRepairCard()
{
    int idx = m_cardCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cards.size())) return;

    const auto& card = m_cards[static_cast<size_t>(idx)];

    auto reply = QMessageBox::warning(this, tr("Repair && Format"),
        tr("This will ERASE ALL DATA on:\n\nDisk %1: %2 (%3)\n\n"
           "The card will be repartitioned and formatted. Continue?")
            .arg(card.diskId)
            .arg(QString::fromStdWString(card.model))
            .arg(formatSize(card.sizeBytes)),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    SdFixConfig config;
    config.action = m_repairCleanChk->isChecked()
                    ? SdFixAction::CleanAndFormat : SdFixAction::FormatOnly;
    switch (m_repairFsCombo->currentIndex())
    {
    case 0: config.targetFs = FilesystemType::FAT32;  break;
    case 1: config.targetFs = FilesystemType::ExFAT;  break;
    case 2: config.targetFs = FilesystemType::NTFS;   break;
    }
    config.volumeLabel = m_repairLabel->text().toStdWString();

    int diskId = card.diskId;

    setOperationRunning(true);
    m_repairProgress->setVisible(true);
    m_repairProgress->setValue(0);
    m_repairStatus->setText(tr("Working..."));

    auto* thread = QThread::create([this, diskId, config]() {
        auto result = SdCardRecovery::fixCard(diskId, config,
            [this](const std::string& stage, int pct) {
                QMetaObject::invokeMethod(m_repairProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_repairStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(stage)));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            m_repairProgress->setVisible(false);
            if (result.isError())
            {
                m_repairStatus->setText(tr("Failed: %1").arg(
                    QString::fromStdString(result.error().message)));
                m_repairStatus->setStyleSheet("color: #ff6b6b;");
            }
            else
            {
                m_repairStatus->setText(tr("✓ Repair complete. Card is ready to use."));
                m_repairStatus->setStyleSheet("color: #a8e6a0; font-weight: bold;");
                emit statusMessage(tr("SD card repair complete"));
                onScanCards(); // Rescan to update status
            }
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() { setOperationRunning(false); });
    thread->start();
}

// ============================================================================
// Secure erase
// ============================================================================
void SdCardTab::onSecureErase()
{
    int idx = m_cardCombo->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(m_cards.size())) return;

    const auto& card = m_cards[static_cast<size_t>(idx)];

    auto reply = QMessageBox::critical(this, tr("Secure Erase"),
        tr("PERMANENTLY DESTROY ALL DATA on:\n\nDisk %1: %2 (%3)\n\n"
           "This action is IRREVERSIBLE. Continue?")
            .arg(card.diskId)
            .arg(QString::fromStdWString(card.model))
            .arg(formatSize(card.sizeBytes)),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    SdFixConfig config;
    config.action    = SdFixAction::CleanAndFormat;
    config.targetFs  = FilesystemType::FAT32;
    config.volumeLabel = L"ERASED";

    int diskId = card.diskId;
    setOperationRunning(true);
    m_repairProgress->setVisible(true);
    m_repairStatus->setText(tr("Securely erasing..."));

    auto* thread = QThread::create([this, diskId, config]() {
        auto result = SdCardRecovery::fixCard(diskId, config,
            [this](const std::string& stage, int pct) {
                QMetaObject::invokeMethod(m_repairProgress, "setValue",
                    Qt::QueuedConnection, Q_ARG(int, pct));
                QMetaObject::invokeMethod(m_repairStatus, "setText",
                    Qt::QueuedConnection, Q_ARG(QString, QString::fromStdString(stage)));
            });

        QMetaObject::invokeMethod(this, [this, result]() {
            m_repairProgress->setVisible(false);
            m_repairStatus->setText(result.isOk()
                ? tr("✓ Secure erase complete.")
                : tr("Failed: %1").arg(QString::fromStdString(result.error().message)));
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this]() { setOperationRunning(false); });
    thread->start();
}

// ============================================================================
// Cancel
// ============================================================================
void SdCardTab::onCancelOperation()
{
    m_cancelFlag.store(true);
    m_cancelScanBtn->setEnabled(false);
}

// ============================================================================
// Helpers
// ============================================================================
void SdCardTab::setOperationRunning(bool running)
{
    m_operationRunning = running;
    m_scanBtn->setEnabled(!running);
    m_counterBtn->setEnabled(!running);
    m_speedBtn->setEnabled(!running);
    m_scanSurfaceBtn->setEnabled(!running);
    m_repairBtn->setEnabled(!running);
    m_eraseBtn->setEnabled(!running);
}

} // namespace spw
