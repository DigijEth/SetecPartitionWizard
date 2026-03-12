#include "SecurityTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

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

    // Sub-tabs for the three security key types
    auto* subTabs = new QTabWidget();

    // --- FIDO2/WebAuthn Tab ---
    auto* fido2Widget = new QWidget();
    auto* fido2Layout = new QVBoxLayout(fido2Widget);

    auto* fido2DevGroup = new QGroupBox(tr("FIDO2 Device"));
    auto* fido2DevLayout = new QGridLayout(fido2DevGroup);
    fido2DevLayout->addWidget(new QLabel(tr("Device:")), 0, 0);
    auto* fido2DeviceCombo = new QComboBox();
    fido2DeviceCombo->addItem(tr("No FIDO2 devices detected"));
    fido2DevLayout->addWidget(fido2DeviceCombo, 0, 1);
    auto* fido2RefreshBtn = new QPushButton(tr("Refresh"));
    fido2DevLayout->addWidget(fido2RefreshBtn, 0, 2);
    fido2DevLayout->addWidget(new QLabel(tr("Device Info:")), 1, 0);
    fido2DevLayout->addWidget(new QLabel(tr("—")), 1, 1);
    fido2Layout->addWidget(fido2DevGroup);

    auto* fido2OpsGroup = new QGroupBox(tr("Operations"));
    auto* fido2OpsLayout = new QVBoxLayout(fido2OpsGroup);
    auto* setPinBtn = new QPushButton(tr("Set/Change PIN"));
    auto* genCredBtn = new QPushButton(tr("Generate Credential"));
    auto* listCredsBtn = new QPushButton(tr("List Resident Keys"));
    auto* resetBtn = new QPushButton(tr("Factory Reset Device"));
    resetBtn->setObjectName("cancelButton");
    fido2OpsLayout->addWidget(setPinBtn);
    fido2OpsLayout->addWidget(genCredBtn);
    fido2OpsLayout->addWidget(listCredsBtn);
    fido2OpsLayout->addWidget(resetBtn);
    fido2Layout->addWidget(fido2OpsGroup);

    auto* fido2KeyList = new QListWidget();
    fido2Layout->addWidget(new QLabel(tr("Resident Keys:")));
    fido2Layout->addWidget(fido2KeyList);

    subTabs->addTab(fido2Widget, tr("FIDO2 / WebAuthn"));

    // --- Encrypted Vault Tab ---
    auto* vaultWidget = new QWidget();
    auto* vaultLayout = new QVBoxLayout(vaultWidget);

    auto* vaultCreateGroup = new QGroupBox(tr("Create Encrypted Vault"));
    auto* vaultCreateLayout = new QGridLayout(vaultCreateGroup);
    vaultCreateLayout->addWidget(new QLabel(tr("USB Drive:")), 0, 0);
    vaultCreateLayout->addWidget(new QComboBox(), 0, 1);
    vaultCreateLayout->addWidget(new QLabel(tr("Vault Size:")), 1, 0);
    auto* vaultSize = new QSpinBox();
    vaultSize->setRange(1, 999999);
    vaultSize->setSuffix(" MB");
    vaultSize->setValue(256);
    vaultCreateLayout->addWidget(vaultSize, 1, 1);
    vaultCreateLayout->addWidget(new QLabel(tr("Encryption:")), 2, 0);
    auto* encCombo = new QComboBox();
    encCombo->addItems({tr("AES-256-XTS"), tr("AES-256-CBC"), tr("ChaCha20-Poly1305")});
    vaultCreateLayout->addWidget(encCombo, 2, 1);
    vaultCreateLayout->addWidget(new QLabel(tr("Password:")), 3, 0);
    auto* passEdit = new QLineEdit();
    passEdit->setEchoMode(QLineEdit::Password);
    vaultCreateLayout->addWidget(passEdit, 3, 1);
    vaultCreateLayout->addWidget(new QLabel(tr("Confirm:")), 4, 0);
    auto* confirmEdit = new QLineEdit();
    confirmEdit->setEchoMode(QLineEdit::Password);
    vaultCreateLayout->addWidget(confirmEdit, 4, 1);
    auto* keyFileCheck = new QCheckBox(tr("Also require key file"));
    vaultCreateLayout->addWidget(keyFileCheck, 5, 1);
    vaultLayout->addWidget(vaultCreateGroup);

    auto* createVaultBtn = new QPushButton(tr("Create Vault"));
    createVaultBtn->setObjectName("applyButton");
    vaultLayout->addWidget(createVaultBtn);

    auto* vaultManageGroup = new QGroupBox(tr("Manage Existing Vaults"));
    auto* vaultManageLayout = new QVBoxLayout(vaultManageGroup);
    auto* unlockBtn = new QPushButton(tr("Unlock Vault"));
    auto* lockBtn = new QPushButton(tr("Lock Vault"));
    auto* changePassBtn = new QPushButton(tr("Change Password"));
    vaultManageLayout->addWidget(unlockBtn);
    vaultManageLayout->addWidget(lockBtn);
    vaultManageLayout->addWidget(changePassBtn);
    vaultLayout->addWidget(vaultManageGroup);

    vaultLayout->addStretch();
    subTabs->addTab(vaultWidget, tr("Encrypted Vaults"));

    // --- Boot Auth Key Tab ---
    auto* bootAuthWidget = new QWidget();
    auto* bootAuthLayout = new QVBoxLayout(bootAuthWidget);

    auto* bootAuthGroup = new QGroupBox(tr("Boot Authentication Key"));
    auto* bootAuthGridLayout = new QGridLayout(bootAuthGroup);
    bootAuthGridLayout->addWidget(new QLabel(tr("USB Drive:")), 0, 0);
    bootAuthGridLayout->addWidget(new QComboBox(), 0, 1);
    bootAuthGridLayout->addWidget(new QLabel(tr("Target PC:")), 1, 0);
    auto* pcIdLabel = new QLabel(tr("Current machine"));
    bootAuthGridLayout->addWidget(pcIdLabel, 1, 1);
    bootAuthGridLayout->addWidget(new QLabel(tr("Auth Method:")), 2, 0);
    auto* authMethodCombo = new QComboBox();
    authMethodCombo->addItems({tr("USB presence only"), tr("USB + PIN"), tr("USB + Password")});
    bootAuthGridLayout->addWidget(authMethodCombo, 2, 1);
    bootAuthLayout->addWidget(bootAuthGroup);

    auto* createBootKeyBtn = new QPushButton(tr("Create Boot Auth Key"));
    createBootKeyBtn->setObjectName("applyButton");
    bootAuthLayout->addWidget(createBootKeyBtn);

    auto* bootKeyInfoGroup = new QGroupBox(tr("Information"));
    auto* bootKeyInfoLayout = new QVBoxLayout(bootKeyInfoGroup);
    bootKeyInfoLayout->addWidget(new QLabel(
        tr("A boot authentication key prevents your PC from booting\n"
           "unless the USB key is inserted. The key material is paired\n"
           "with your machine's hardware identity.\n\n"
           "Warning: Keep a backup key! Losing the USB key may lock\n"
           "you out of your system.")));
    bootAuthLayout->addWidget(bootKeyInfoGroup);

    bootAuthLayout->addStretch();
    subTabs->addTab(bootAuthWidget, tr("Boot Authentication"));

    layout->addWidget(subTabs);
}

} // namespace spw
