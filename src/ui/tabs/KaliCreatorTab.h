#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace spw
{

class DownloadManager;

class KaliCreatorTab : public QWidget
{
    Q_OBJECT

public:
    explicit KaliCreatorTab(QWidget* parent = nullptr);
    ~KaliCreatorTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    // USB / SD Card
    void onFlashToUsb();
    void onUsbImageChanged(int index);

    // Virtual Machine
    void onCreateVmDisk();
    void onBrowseVmOutput();
    void onDownloadPrebuiltVm();

    // Containers
    void onPullContainerImage();
    void onContainerRuntimeChanged(int index);
    void onContainerTagChanged(int index);

    // Cloud Image
    void onDownloadCloudImage();
    void onBrowseCloudOutput();

private:
    void setupUi();
    void setupUsbTab(QTabWidget* tabs);
    void setupVmTab(QTabWidget* tabs);
    void setupContainerTab(QTabWidget* tabs);
    void setupCloudTab(QTabWidget* tabs);
    void populateRemovableDrives();
    void updateContainerPullPreview();

    static QString formatSize(uint64_t bytes);

    // USB / SD Card sub-tab
    QComboBox*    m_usbImageCombo      = nullptr;
    QComboBox*    m_usbTargetCombo     = nullptr;
    QCheckBox*    m_usbPersistCheck    = nullptr;
    QSpinBox*     m_usbPersistSizeSpin = nullptr;
    QLabel*       m_usbPersistLabel    = nullptr;
    QProgressBar* m_usbProgress        = nullptr;
    QLabel*       m_usbStatusLabel     = nullptr;
    QPushButton*  m_usbFlashBtn        = nullptr;

    // Virtual Machine sub-tab
    QComboBox*    m_vmFormatCombo       = nullptr;
    QSpinBox*     m_vmSizeSpin         = nullptr;
    QLineEdit*    m_vmOutputEdit       = nullptr;
    QComboBox*    m_vmVersionCombo     = nullptr;
    QPushButton*  m_vmCreateBtn        = nullptr;
    QPushButton*  m_vmDownloadBtn      = nullptr;
    QProgressBar* m_vmProgress         = nullptr;
    QLabel*       m_vmStatusLabel      = nullptr;

    // Containers sub-tab
    QComboBox*      m_containerRuntimeCombo = nullptr;
    QComboBox*      m_containerTagCombo     = nullptr;
    QLineEdit*      m_containerCmdPreview   = nullptr;
    QPushButton*    m_containerPullBtn      = nullptr;
    QPlainTextEdit* m_containerLog          = nullptr;
    QProcess*       m_containerProcess      = nullptr;

    // Cloud Image sub-tab
    QComboBox*    m_cloudFormatCombo    = nullptr;
    QLabel*       m_cloudInfoLabel     = nullptr;
    QLineEdit*    m_cloudOutputEdit    = nullptr;
    QPushButton*  m_cloudDownloadBtn   = nullptr;
    QProgressBar* m_cloudProgress      = nullptr;
    QLabel*       m_cloudStatusLabel   = nullptr;

    // Shared
    SystemDiskSnapshot m_snapshot;
    DownloadManager* m_downloader = nullptr;
};

} // namespace spw
