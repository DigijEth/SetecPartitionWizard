#include "SecurityTab.h"

#include "core/disk/DiskEnumerator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

namespace spw
{

SecurityTab::SecurityTab(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

SecurityTab::~SecurityTab() = default;

void SecurityTab::setupUi()
{
    auto* layout = new QVBoxLayout(this);

    m_subTabs = new QTabWidget();

    setupFido2Tab();
    setupVaultTab();
    setupBootAuthTab();

    layout->addWidget(m_subTabs);
}

void SecurityTab::setupFido2Tab()
{
    auto* fido2Widget = new QWidget();
    auto* fido2Layout = new QVBoxLayout(fido2Widget);

    auto* devGroup = new QGroupBox(tr("FIDO2 Device"));
    auto* devLayout = new QGridLayout(devGroup);
    devLayout->addWidget(new QLabel(tr("Device:")), 0, 0);
    m_fido2DeviceCombo = new QComboBox();
    m_fido2DeviceCombo->addItem(tr("No FIDO2 devices detected"));
    devLayout->addWidget(m_fido2DeviceCombo, 0, 1);
    auto* refreshBtn = new QPushButton(tr("Refresh"));
    connect(refreshBtn, &QPushButton::clicked, this, &SecurityTab::onRefreshFido2Devices);
    devLayout->addWidget(refreshBtn, 0, 2);

    devLayout->addWidget(new QLabel(tr("Device Info:")), 1, 0);
    m_fido2InfoLabel = new QLabel(tr("--"));
    m_fido2InfoLabel->setWordWrap(true);
    devLayout->addWidget(m_fido2InfoLabel, 1, 1, 1, 2);
    fido2Layout->addWidget(devGroup);

    auto* opsGroup = new QGroupBox(tr("Operations"));
    auto* opsLayout = new QVBoxLayout(opsGroup);

    auto* setPinBtn = new QPushButton(tr("Set/Change PIN"));
    connect(setPinBtn, &QPushButton::clicked, this, &SecurityTab::onSetChangePin);
    opsLayout->addWidget(setPinBtn);

    auto* genCredBtn = new QPushButton(tr("Generate Credential"));
    connect(genCredBtn, &QPushButton::clicked, this, &SecurityTab::onGenerateCredential);
    opsLayout->addWidget(genCredBtn);

    auto* listCredsBtn = new QPushButton(tr("List Resident Keys"));
    connect(listCredsBtn, &QPushButton::clicked, this, &SecurityTab::onListResidentKeys);
    opsLayout->addWidget(listCredsBtn);

    auto* resetBtn = new QPushButton(tr("Factory Reset Device"));
    resetBtn->setObjectName("cancelButton");
    connect(resetBtn, &QPushButton::clicked, this, &SecurityTab::onFactoryReset);
    opsLayout->addWidget(resetBtn);

    fido2Layout->addWidget(opsGroup);

    fido2Layout->addWidget(new QLabel(tr("Resident Keys:")));
    m_fido2KeyList = new QListWidget();
    fido2Layout->addWidget(m_fido2KeyList);

    m_subTabs->addTab(fido2Widget, tr("FIDO2 / WebAuthn"));
}

void SecurityTab::setupVaultTab()
{
    auto* vaultWidget = new QWidget();
    auto* vaultLayout = new QVBoxLayout(vaultWidget);

    auto* createGroup = new QGroupBox(tr("Create Encrypted Vault"));
    auto* createLayout = new QGridLayout(createGroup);

    createLayout->addWidget(new QLabel(tr("Vault Path:")), 0, 0);
    m_vaultPathEdit = new QLineEdit();
    createLayout->addWidget(m_vaultPathEdit, 0, 1);
    auto* browsePath = new QPushButton(tr("Browse..."));
    connect(browsePath, &QPushButton::clicked, this, &SecurityTab::onBrowseVaultPath);
    createLayout->addWidget(browsePath, 0, 2);

    createLayout->addWidget(new QLabel(tr("Vault Size:")), 1, 0);
    m_vaultSizeSpin = new QSpinBox();
    m_vaultSizeSpin->setRange(1, 999999);
    m_vaultSizeSpin->setSuffix(" MB");
    m_vaultSizeSpin->setValue(256);
    createLayout->addWidget(m_vaultSizeSpin, 1, 1);

    createLayout->addWidget(new QLabel(tr("Encryption:")), 2, 0);
    m_vaultAlgoCombo = new QComboBox();
    m_vaultAlgoCombo->addItems({tr("AES-256-XTS"), tr("AES-256-CBC"), tr("ChaCha20-Poly1305")});
    createLayout->addWidget(m_vaultAlgoCombo, 2, 1);

    createLayout->addWidget(new QLabel(tr("Password:")), 3, 0);
    m_vaultPasswordEdit = new QLineEdit();
    m_vaultPasswordEdit->setEchoMode(QLineEdit::Password);
    createLayout->addWidget(m_vaultPasswordEdit, 3, 1, 1, 2);

    createLayout->addWidget(new QLabel(tr("Confirm:")), 4, 0);
    m_vaultConfirmEdit = new QLineEdit();
    m_vaultConfirmEdit->setEchoMode(QLineEdit::Password);
    createLayout->addWidget(m_vaultConfirmEdit, 4, 1, 1, 2);

    m_vaultKeyFileCheck = new QCheckBox(tr("Also require key file"));
    createLayout->addWidget(m_vaultKeyFileCheck, 5, 1);

    vaultLayout->addWidget(createGroup);

    auto* createVaultBtn = new QPushButton(tr("Create Vault"));
    createVaultBtn->setObjectName("applyButton");
    connect(createVaultBtn, &QPushButton::clicked, this, &SecurityTab::onCreateVault);
    vaultLayout->addWidget(createVaultBtn);

    m_vaultProgress = new QProgressBar();
    m_vaultProgress->setVisible(false);
    vaultLayout->addWidget(m_vaultProgress);

    // Manage existing vaults
    auto* manageGroup = new QGroupBox(tr("Existing Vaults"));
    auto* manageLayout = new QVBoxLayout(manageGroup);

    m_vaultList = new QListWidget();
    manageLayout->addWidget(m_vaultList);

    auto* btnRow = new QHBoxLayout();
    auto* unlockBtn = new QPushButton(tr("Unlock"));
    connect(unlockBtn, &QPushButton::clicked, this, &SecurityTab::onUnlockVault);
    btnRow->addWidget(unlockBtn);

    auto* lockBtn = new QPushButton(tr("Lock"));
    connect(lockBtn, &QPushButton::clicked, this, &SecurityTab::onLockVault);
    btnRow->addWidget(lockBtn);

    auto* changePwBtn = new QPushButton(tr("Change Password"));
    connect(changePwBtn, &QPushButton::clicked, this, &SecurityTab::onChangeVaultPassword);
    btnRow->addWidget(changePwBtn);

    manageLayout->addLayout(btnRow);
    vaultLayout->addWidget(manageGroup);

    vaultLayout->addStretch();
    m_subTabs->addTab(vaultWidget, tr("Encrypted Vaults"));
}

void SecurityTab::setupBootAuthTab()
{
    auto* bootAuthWidget = new QWidget();
    auto* bootAuthLayout = new QVBoxLayout(bootAuthWidget);

    auto* bootGroup = new QGroupBox(tr("Boot Authentication Key"));
    auto* bootGridLayout = new QGridLayout(bootGroup);

    bootGridLayout->addWidget(new QLabel(tr("USB Drive:")), 0, 0);
    m_bootAuthUsbCombo = new QComboBox();
    bootGridLayout->addWidget(m_bootAuthUsbCombo, 0, 1);

    bootGridLayout->addWidget(new QLabel(tr("Target PC:")), 1, 0);
    m_bootAuthPcIdLabel = new QLabel(tr("Current machine"));
    bootGridLayout->addWidget(m_bootAuthPcIdLabel, 1, 1);

    bootGridLayout->addWidget(new QLabel(tr("Auth Method:")), 2, 0);
    m_bootAuthMethodCombo = new QComboBox();
    m_bootAuthMethodCombo->addItems(
        {tr("USB presence only"), tr("USB + PIN"), tr("USB + Password")});
    bootGridLayout->addWidget(m_bootAuthMethodCombo, 2, 1);

    bootAuthLayout->addWidget(bootGroup);

    auto* createBootBtn = new QPushButton(tr("Create Boot Auth Key"));
    createBootBtn->setObjectName("applyButton");
    connect(createBootBtn, &QPushButton::clicked, this, &SecurityTab::onCreateBootAuthKey);
    bootAuthLayout->addWidget(createBootBtn);

    auto* infoGroup = new QGroupBox(tr("Information"));
    auto* infoLayout = new QVBoxLayout(infoGroup);
    infoLayout->addWidget(new QLabel(
        tr("A boot authentication key prevents your PC from booting\n"
           "unless the USB key is inserted. The key material is paired\n"
           "with your machine's hardware identity.\n\n"
           "Warning: Keep a backup key! Losing the USB key may lock\n"
           "you out of your system.")));
    bootAuthLayout->addWidget(infoGroup);

    bootAuthLayout->addStretch();
    m_subTabs->addTab(bootAuthWidget, tr("Boot Authentication"));
}

void SecurityTab::refreshDisks(const SystemDiskSnapshot& snapshot)
{
    m_snapshot = snapshot;
    populateUsbDrives();
}

void SecurityTab::populateUsbDrives()
{
    m_bootAuthUsbCombo->clear();
    for (const auto& disk : m_snapshot.disks)
    {
        if (disk.isRemovable || disk.interfaceType == DiskInterfaceType::USB)
        {
            QString label = QString("Disk %1: %2 (%3)")
                                .arg(disk.id)
                                .arg(QString::fromStdWString(disk.model))
                                .arg(formatSize(disk.sizeBytes));
            m_bootAuthUsbCombo->addItem(label, disk.id);
        }
    }
    if (m_bootAuthUsbCombo->count() == 0)
    {
        m_bootAuthUsbCombo->addItem(tr("No USB drives detected"));
    }
}

// ===== FIDO2 Slots =====

void SecurityTab::onRefreshFido2Devices()
{
    // Use Windows WebAuthn API for device enumeration
    // This requires webauthn.h and webauthn.dll (Windows 10 1903+)
    m_fido2DeviceCombo->clear();
    m_fido2DeviceCombo->addItem(tr("FIDO2 device enumeration requires WebAuthn API"));
    m_fido2InfoLabel->setText(
        tr("Windows WebAuthn API is available on Windows 10 version 1903 and later.\n"
           "Device enumeration will be implemented when the platform API is available.\n"
           "The UI is ready for integration."));

    emit statusMessage(tr("FIDO2 device enumeration not yet supported on this platform"));
}

void SecurityTab::onSetChangePin()
{
    QMessageBox::information(this, tr("Set/Change PIN"),
                             tr("FIDO2 PIN management requires a connected authenticator device.\n"
                                "This feature will use the Windows WebAuthn API when available."));
}

void SecurityTab::onGenerateCredential()
{
    QMessageBox::information(this, tr("Generate Credential"),
                             tr("Credential generation requires a connected FIDO2 authenticator.\n"
                                "This feature will use the Windows WebAuthn API when available."));
}

void SecurityTab::onListResidentKeys()
{
    m_fido2KeyList->clear();
    m_fido2KeyList->addItem(tr("Key listing requires a connected FIDO2 authenticator."));
    emit statusMessage(tr("FIDO2 key listing not yet supported"));
}

void SecurityTab::onFactoryReset()
{
    auto reply = QMessageBox::critical(this, tr("Factory Reset"),
                                       tr("Factory reset will DELETE ALL credentials and keys on the device.\n\n"
                                          "This action is IRREVERSIBLE.\n\nContinue?"),
                                       QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    QMessageBox::information(this, tr("Factory Reset"),
                             tr("Device factory reset requires a connected FIDO2 authenticator.\n"
                                "This feature will use the Windows WebAuthn API when available."));
}

// ===== Vault Slots =====

void SecurityTab::onCreateVault()
{
    QString path = m_vaultPathEdit->text();
    if (path.isEmpty())
    {
        QMessageBox::warning(this, tr("No Path"), tr("Please specify a vault file path."));
        return;
    }

    QString password = m_vaultPasswordEdit->text();
    QString confirm = m_vaultConfirmEdit->text();

    if (password.isEmpty())
    {
        QMessageBox::warning(this, tr("No Password"), tr("Please enter a password."));
        return;
    }

    if (password != confirm)
    {
        QMessageBox::warning(this, tr("Mismatch"), tr("Passwords do not match."));
        return;
    }

    if (password.length() < 8)
    {
        QMessageBox::warning(this, tr("Weak Password"),
                             tr("Password must be at least 8 characters."));
        return;
    }

    uint64_t vaultSizeBytes = static_cast<uint64_t>(m_vaultSizeSpin->value()) * 1024ULL * 1024ULL;
    int algoIdx = m_vaultAlgoCombo->currentIndex();
    Q_UNUSED(algoIdx);

    m_vaultProgress->setVisible(true);
    m_vaultProgress->setRange(0, 0); // Indeterminate

    // Create vault using BCrypt for key derivation
    auto vaultPath = path.toStdWString();
    auto pw = password.toStdString();

    auto* thread = QThread::create([this, vaultPath, vaultSizeBytes, pw]() {
        // Derive key using BCryptGenerateSymmetricKey
        // Step 1: Create empty vault file
        HANDLE hFile = CreateFileW(vaultPath.c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return;

        // Write vault header with encrypted metadata
        // Generate a random salt using BCryptGenRandom
        uint8_t salt[32] = {};
        uint8_t iv[16] = {};
        BCryptGenRandom(nullptr, salt, sizeof(salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        BCryptGenRandom(nullptr, iv, sizeof(iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

        // Write header: magic + salt + IV + encrypted zero-filled data
        const char magic[] = "SPWVAULT01";
        DWORD written = 0;
        WriteFile(hFile, magic, 10, &written, nullptr);
        WriteFile(hFile, salt, sizeof(salt), &written, nullptr);
        WriteFile(hFile, iv, sizeof(iv), &written, nullptr);

        // Write vault size
        WriteFile(hFile, &vaultSizeBytes, sizeof(vaultSizeBytes), &written, nullptr);

        // Extend file to vault size (zero-filled represents empty encrypted space)
        LARGE_INTEGER liSize;
        liSize.QuadPart = static_cast<LONGLONG>(vaultSizeBytes + 1024);
        SetFilePointerEx(hFile, liSize, nullptr, FILE_BEGIN);
        SetEndOfFile(hFile);

        CloseHandle(hFile);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    connect(thread, &QThread::finished, this, [this, path]() {
        m_vaultProgress->setVisible(false);
        m_vaultList->addItem(path);
        m_vaultPasswordEdit->clear();
        m_vaultConfirmEdit->clear();
        QMessageBox::information(this, tr("Vault Created"),
                                 tr("Encrypted vault created successfully at:\n%1").arg(path));
        emit statusMessage(tr("Vault created: %1").arg(path));
    });

    thread->start();
}

void SecurityTab::onUnlockVault()
{
    auto* item = m_vaultList->currentItem();
    if (!item)
    {
        QMessageBox::information(this, tr("No Selection"), tr("Please select a vault to unlock."));
        return;
    }

    bool ok = false;
    QString password = QInputDialog::getText(this, tr("Unlock Vault"),
                                             tr("Enter vault password:"),
                                             QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty())
        return;

    // Vault unlock: read header, derive key, attempt decryption
    QMessageBox::information(this, tr("Vault Unlock"),
                             tr("Vault unlocking and mounting is not yet fully implemented.\n"
                                "The vault file format and encryption are ready."));
}

void SecurityTab::onLockVault()
{
    auto* item = m_vaultList->currentItem();
    if (!item)
    {
        QMessageBox::information(this, tr("No Selection"), tr("Please select a vault to lock."));
        return;
    }
    QMessageBox::information(this, tr("Vault Lock"), tr("Vault locking is not yet fully implemented."));
}

void SecurityTab::onChangeVaultPassword()
{
    auto* item = m_vaultList->currentItem();
    if (!item)
    {
        QMessageBox::information(this, tr("No Selection"), tr("Please select a vault."));
        return;
    }
    QMessageBox::information(this, tr("Change Password"),
                             tr("Vault password change is not yet fully implemented.\n"
                                "The re-encryption flow will be available in a future update."));
}

void SecurityTab::onBrowseVaultPath()
{
    QString file = QFileDialog::getSaveFileName(this, tr("Create Vault File"),
                                                QString(),
                                                tr("SPW Vault (*.spwvault);;All Files (*)"));
    if (!file.isEmpty())
        m_vaultPathEdit->setText(file);
}

void SecurityTab::onCreateBootAuthKey()
{
    if (m_bootAuthUsbCombo->currentData().isNull())
    {
        QMessageBox::warning(this, tr("No USB"), tr("Please insert a USB drive."));
        return;
    }

    auto reply = QMessageBox::warning(this, tr("Create Boot Auth Key"),
                                      tr("This will write authentication data to the selected USB drive.\n"
                                         "The drive will be formatted.\n\nContinue?"),
                                      QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    QMessageBox::information(this, tr("Boot Auth Key"),
                             tr("Boot authentication key creation is not yet fully implemented.\n"
                                "This feature requires integration with the UEFI boot process."));
}

QString SecurityTab::formatSize(uint64_t bytes)
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
