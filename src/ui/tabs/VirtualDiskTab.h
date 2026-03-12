#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"
#include "core/imaging/VirtualDisk.h"

#include <QWidget>
#include <atomic>

class QComboBox;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTabWidget;
class QTableWidget;

namespace spw
{

class VirtualDiskTab : public QWidget
{
    Q_OBJECT

public:
    explicit VirtualDiskTab(QWidget* parent = nullptr);
    ~VirtualDiskTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onMount();
    void onUnmount();
    void onRefreshMounted();
    void onBrowseMount();
    void onCreate();
    void onBrowseCreate();
    void onCapture();
    void onFlash();
    void onConvert();
    void onBrowseConvertIn();
    void onBrowseConvertOut();
    void onBrowseFlashImage();
    void onWslMount();
    void onWslUnmount();
    void onWslCheckAvailable();

private:
    void setupUi();
    void populateDiskCombo();
    static QString formatSize(uint64_t bytes);

    // Mount / Unmount
    QLineEdit*    m_mountPathEdit   = nullptr;
    QPushButton*  m_mountBrowseBtn  = nullptr;
    QCheckBox*    m_mountReadOnly   = nullptr;
    QPushButton*  m_mountBtn        = nullptr;
    QTableWidget* m_mountedTable    = nullptr;
    QPushButton*  m_unmountBtn      = nullptr;
    QPushButton*  m_refreshBtn      = nullptr;
    QLabel*       m_mountStatus     = nullptr;

    // Create
    QLineEdit*      m_createPathEdit  = nullptr;
    QPushButton*    m_createBrowse    = nullptr;
    QComboBox*      m_createFmtCombo  = nullptr;
    QDoubleSpinBox* m_createSizeSpin  = nullptr;
    QComboBox*      m_createSizeUnit  = nullptr;
    QCheckBox*      m_createDynamic   = nullptr;
    QPushButton*    m_createBtn       = nullptr;
    QProgressBar*   m_createProgress  = nullptr;
    QLabel*         m_createStatus    = nullptr;

    // Capture (disk → image)
    QComboBox*    m_captureSourceCombo = nullptr;
    QLineEdit*    m_captureOutEdit     = nullptr;
    QPushButton*  m_captureBrowse      = nullptr;
    QComboBox*    m_captureFmtCombo    = nullptr;
    QPushButton*  m_captureBtn         = nullptr;
    QProgressBar* m_captureProgress    = nullptr;
    QLabel*       m_captureStatus      = nullptr;

    // Flash (image → disk/SD)
    QLineEdit*    m_flashImageEdit    = nullptr;
    QPushButton*  m_flashBrowse       = nullptr;
    QComboBox*    m_flashTargetCombo  = nullptr;
    QPushButton*  m_flashBtn          = nullptr;
    QProgressBar* m_flashProgress     = nullptr;
    QLabel*       m_flashStatus       = nullptr;

    // Convert
    QLineEdit*    m_convInEdit        = nullptr;
    QPushButton*  m_convInBrowse      = nullptr;
    QLineEdit*    m_convOutEdit       = nullptr;
    QPushButton*  m_convOutBrowse     = nullptr;
    QComboBox*    m_convFmtCombo      = nullptr;
    QLabel*       m_qemuStatus        = nullptr;
    QPushButton*  m_convertBtn        = nullptr;
    QProgressBar* m_convProgress      = nullptr;
    QLabel*       m_convStatus        = nullptr;

    // WSL2 Linux filesystem mount
    QComboBox*   m_wslDiskCombo    = nullptr;
    QComboBox*   m_wslFsCombo      = nullptr;
    QCheckBox*   m_wslReadOnly     = nullptr;
    QPushButton* m_wslMountBtn     = nullptr;
    QPushButton* m_wslUnmountBtn   = nullptr;
    QLabel*      m_wslStatus       = nullptr;
    QLabel*      m_wslAvailLabel   = nullptr;

    SystemDiskSnapshot m_snapshot;
};

} // namespace spw
