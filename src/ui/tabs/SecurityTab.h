#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace spw
{

class SecurityTab : public QWidget
{
    Q_OBJECT

public:
    explicit SecurityTab(QWidget* parent = nullptr);
    ~SecurityTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    // FIDO2
    void onRefreshFido2Devices();
    void onSetChangePin();
    void onGenerateCredential();
    void onListResidentKeys();
    void onFactoryReset();

    // Vaults
    void onCreateVault();
    void onUnlockVault();
    void onLockVault();
    void onChangeVaultPassword();
    void onBrowseVaultPath();

    // Boot Auth
    void onCreateBootAuthKey();

private:
    void setupUi();
    void setupFido2Tab();
    void setupVaultTab();
    void setupBootAuthTab();
    void populateUsbDrives();

    static QString formatSize(uint64_t bytes);

    QTabWidget* m_subTabs = nullptr;

    // FIDO2 tab
    QComboBox* m_fido2DeviceCombo = nullptr;
    QLabel* m_fido2InfoLabel = nullptr;
    QListWidget* m_fido2KeyList = nullptr;

    // Vault tab
    QLineEdit* m_vaultPathEdit = nullptr;
    QSpinBox* m_vaultSizeSpin = nullptr;
    QComboBox* m_vaultAlgoCombo = nullptr;
    QLineEdit* m_vaultPasswordEdit = nullptr;
    QLineEdit* m_vaultConfirmEdit = nullptr;
    QCheckBox* m_vaultKeyFileCheck = nullptr;
    QListWidget* m_vaultList = nullptr;
    QProgressBar* m_vaultProgress = nullptr;

    // Boot Auth tab
    QComboBox* m_bootAuthUsbCombo = nullptr;
    QComboBox* m_bootAuthMethodCombo = nullptr;
    QLabel* m_bootAuthPcIdLabel = nullptr;

    // Data
    SystemDiskSnapshot m_snapshot;
};

} // namespace spw
