#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/maintenance/SdCardRecovery.h"
#include "core/maintenance/SdCardAnalyzer.h"

#include <QWidget>
#include <atomic>
#include <vector>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTextEdit;
class QCheckBox;
class QSpinBox;

namespace spw
{

class SdCardTab : public QWidget
{
    Q_OBJECT

public:
    explicit SdCardTab(QWidget* parent = nullptr);
    ~SdCardTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onScanCards();
    void onCardSelected(int index);
    void onRefreshInfo();
    void onCheckCounterfeit();
    void onRunSpeedTest();
    void onSurfaceScan();
    void onRepairCard();
    void onFormatCard() { onRepairCard(); }
    void onSecureErase();
    void onCancelOperation();

private:
    void setupUi();
    void setupCardSelectorPanel();
    void setupInfoPanel();
    void setupCounterfeitPanel();
    void setupSpeedPanel();
    void setupHealthPanel();
    void setupRepairPanel();

    void updateCardInfo(const SdCardInfo& card, const SdCardIdentity& identity);
    void setOperationRunning(bool running);

    static QString formatSize(uint64_t bytes);
    static QString formatSpeed(double mbps);
    static QString verdictString(CounterfeitVerdict v);
    static QString verdictStyle(CounterfeitVerdict v);

    // Card selector
    QComboBox*     m_cardCombo    = nullptr;
    QPushButton*   m_scanBtn      = nullptr;
    QLabel*        m_cardSummaryLabel = nullptr;

    // Info panel
    QLabel* m_infoModel      = nullptr;
    QLabel* m_infoSerial     = nullptr;
    QLabel* m_infoVendor     = nullptr;
    QLabel* m_infoCapacity   = nullptr;
    QLabel* m_infoBusType    = nullptr;
    QLabel* m_infoInterface  = nullptr;
    QLabel* m_infoWriteProt  = nullptr;
    QLabel* m_infoStatus     = nullptr;
    QLabel* m_infoManufacturer = nullptr;

    // Counterfeit
    QPushButton* m_counterBtn     = nullptr;
    QLabel*      m_counterVerdict = nullptr;
    QTextEdit*   m_counterLog     = nullptr;
    QProgressBar* m_counterProgress = nullptr;

    // Speed test
    QPushButton* m_speedBtn       = nullptr;
    QProgressBar* m_speedProgress = nullptr;
    QLabel* m_speedSeqRead        = nullptr;
    QLabel* m_speedSeqWrite       = nullptr;
    QLabel* m_speedRandRead       = nullptr;
    QLabel* m_speedRandWrite      = nullptr;
    QLabel* m_speedNotes          = nullptr;

    // Health / surface scan
    QPushButton* m_scanSurfaceBtn = nullptr;
    QPushButton* m_cancelScanBtn  = nullptr;
    QProgressBar* m_healthProgress = nullptr;
    QLabel* m_healthBad           = nullptr;
    QLabel* m_healthSlow          = nullptr;
    QLabel* m_healthScanned       = nullptr;
    QLabel* m_healthResult        = nullptr;

    // Repair / format
    QComboBox*   m_repairFsCombo  = nullptr;
    QLineEdit*   m_repairLabel    = nullptr;
    QCheckBox*   m_repairCleanChk = nullptr;
    QPushButton* m_repairBtn      = nullptr;
    QPushButton* m_eraseBtn       = nullptr;
    QProgressBar* m_repairProgress = nullptr;
    QLabel*      m_repairStatus   = nullptr;

    // Inner tab widget
    QTabWidget* m_innerTabs = nullptr;

    // State
    std::vector<SdCardInfo> m_cards;
    std::atomic<bool> m_cancelFlag{false};
    bool m_operationRunning = false;
};

} // namespace spw
