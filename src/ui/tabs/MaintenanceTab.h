#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/maintenance/SecureErase.h"
#include "core/maintenance/SdCardRecovery.h"

#include <QWidget>
#include <atomic>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace spw
{

class MaintenanceTab : public QWidget
{
    Q_OBJECT

public:
    explicit MaintenanceTab(QWidget* parent = nullptr);
    ~MaintenanceTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onSecureErase();
    void onEraseMethodChanged();
    void onRepairMbr();
    void onRepairGpt();
    void onRepairBcd();
    void onReinstallBootloader();
    void onSdScan();
    void onSdFix();
    void onInstallGrub2();
    void onInstallWindowsBM();
    void onInstallSyslinux();
    void onInstallRefind();

private:
    void setupUi();
    void populateDiskCombo();

    static QString formatSize(uint64_t bytes);

    // Secure Erase
    QComboBox* m_eraseDiskCombo = nullptr;
    QComboBox* m_eraseMethodCombo = nullptr;
    QSpinBox* m_customPassSpin = nullptr;
    QCheckBox* m_verifyCheck = nullptr;
    QPushButton* m_eraseBtn = nullptr;
    QProgressBar* m_eraseProgress = nullptr;
    QLabel* m_eraseStatusLabel = nullptr;

    // Boot Repair
    QComboBox* m_bootDiskCombo = nullptr;
    QPushButton* m_mbrRepairBtn = nullptr;
    QPushButton* m_gptRepairBtn = nullptr;
    QPushButton* m_bcdRepairBtn = nullptr;
    QPushButton* m_bootloaderBtn = nullptr;
    QProgressBar* m_bootProgress = nullptr;
    QLabel* m_bootStatusLabel = nullptr;

    // Bootloader Install
    QComboBox* m_blDiskCombo = nullptr;
    QComboBox* m_blPartCombo = nullptr;
    QPushButton* m_grub2Btn = nullptr;
    QPushButton* m_winbmBtn = nullptr;
    QPushButton* m_syslinuxBtn = nullptr;
    QPushButton* m_refindBtn = nullptr;
    QProgressBar* m_blProgress = nullptr;
    QLabel* m_blStatusLabel = nullptr;

    // SD Card Recovery
    QComboBox* m_sdCardCombo = nullptr;
    QPushButton* m_sdScanBtn = nullptr;
    QPushButton* m_sdFixBtn = nullptr;
    QComboBox* m_sdFsCombo = nullptr;
    QLineEdit* m_sdLabelEdit = nullptr;
    QLabel* m_sdStatusLabel = nullptr;
    QProgressBar* m_sdProgress = nullptr;
    std::vector<SdCardInfo> m_detectedCards;

    // Data
    SystemDiskSnapshot m_snapshot;
    std::atomic<bool> m_cancelFlag{false};
};

} // namespace spw
