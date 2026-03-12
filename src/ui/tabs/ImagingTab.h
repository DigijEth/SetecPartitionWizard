#pragma once

#include "core/common/Types.h"
#include "core/disk/DiskEnumerator.h"

#include <QWidget>

class QComboBox;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace spw
{

class ImagingTab : public QWidget
{
    Q_OBJECT

public:
    explicit ImagingTab(QWidget* parent = nullptr);
    ~ImagingTab() override;

public slots:
    void refreshDisks(const SystemDiskSnapshot& snapshot);

signals:
    void statusMessage(const QString& msg);

private slots:
    void onCloneDisk();
    void onCreateImage();
    void onRestoreImage();
    void onFlashIso();
    void onBrowseImageOutput();
    void onBrowseRestoreInput();
    void onBrowseFlashInput();
    void onRestoreInputChanged();
    void onOpticalRipDisc();
    void onOpticalBurnImage();
    void onOpticalErase();
    void onOpticalBrowseBurnInput();
    void onOpticalBrowseRipOutput();
    void onOpticalRefreshDrives();

private:
    void setupUi();
    void populateDiskCombos();

    static QString formatSize(uint64_t bytes);

    // Clone section
    QComboBox* m_cloneSourceCombo = nullptr;
    QComboBox* m_cloneDestCombo = nullptr;
    QComboBox* m_cloneModeCombo = nullptr;
    QCheckBox* m_cloneVerifyCheck = nullptr;
    QPushButton* m_cloneBtn = nullptr;
    QProgressBar* m_cloneProgress = nullptr;
    QLabel* m_cloneSpeedLabel = nullptr;

    // Create Image section
    QComboBox* m_imageSourceCombo = nullptr;
    QLineEdit* m_imageOutputEdit = nullptr;
    QComboBox* m_imageFormatCombo = nullptr;
    QPushButton* m_imageCreateBtn = nullptr;
    QProgressBar* m_imageCreateProgress = nullptr;
    QLabel* m_imageCreateSpeedLabel = nullptr;

    // Restore Image section
    QLineEdit* m_restoreInputEdit = nullptr;
    QLabel* m_restoreImageInfo = nullptr;
    QComboBox* m_restoreDestCombo = nullptr;
    QCheckBox* m_restoreVerifyCheck = nullptr;
    QPushButton* m_restoreBtn = nullptr;
    QProgressBar* m_restoreProgress = nullptr;
    QLabel* m_restoreSpeedLabel = nullptr;

    // Flash ISO section
    QLineEdit* m_flashInputEdit = nullptr;
    QComboBox* m_flashTargetCombo = nullptr;
    QCheckBox* m_flashVerifyCheck = nullptr;
    QPushButton* m_flashBtn = nullptr;
    QProgressBar* m_flashProgress = nullptr;
    QLabel* m_flashSpeedLabel = nullptr;

    // Optical disc (CD/DVD/Blu-ray)
    QComboBox*    m_opticalDriveCombo   = nullptr;
    QPushButton*  m_opticalRefreshBtn   = nullptr;
    QLabel*       m_opticalDriveInfo    = nullptr;
    // Rip
    QLineEdit*    m_ripOutputEdit       = nullptr;
    QComboBox*    m_ripFormatCombo      = nullptr;
    QCheckBox*    m_ripVerifyCheck      = nullptr;
    QPushButton*  m_ripBtn              = nullptr;
    QProgressBar* m_ripProgress         = nullptr;
    QLabel*       m_ripStatusLabel      = nullptr;
    // Burn
    QLineEdit*    m_burnInputEdit       = nullptr;
    QComboBox*    m_burnSpeedCombo      = nullptr;
    QCheckBox*    m_burnVerifyCheck     = nullptr;
    QCheckBox*    m_burnFinalizeCheck   = nullptr;
    QPushButton*  m_burnBtn             = nullptr;
    QProgressBar* m_burnProgress        = nullptr;
    QLabel*       m_burnStatusLabel     = nullptr;
    // Erase
    QComboBox*    m_eraseTypeCombo      = nullptr;
    QPushButton*  m_opticalEraseBtn     = nullptr;
    QLabel*       m_eraseStatusLabel    = nullptr;

    // Data
    SystemDiskSnapshot m_snapshot;
};

} // namespace spw
