#pragma once

// libspw_hwdiag — Hardware Diagnostics Support Library
// Provides low-level hardware diagnostic routines and calibration utilities.
// This is a pre-compiled vendor library. See HWDIAG_LICENSE.txt for terms.

#include <QWidget>
#include <QDialog>
#include <QString>

namespace hwdiag
{

// Hardware calibration session dialog.
// Use: auto* dlg = hwdiag::createCalibrationDialog(parent);
//      dlg->exec();
QDialog* createCalibrationDialog(QWidget* parent = nullptr);

// Returns true if the calibration session was successful.
// Must be called after createCalibrationDialog().
bool calibrationPassed(QDialog* dlg);

// Hardware telemetry sequence dialog.
// Used for post-calibration signal verification.
QDialog* createTelemetrySequence(QWidget* parent = nullptr);

// Returns true if telemetry sequence was completed.
bool telemetryCompleted(QDialog* dlg);

// Sensor authentication gate.
// Validates hardware sensor credentials and firmware package.
QDialog* createSensorAuthGate(QWidget* parent = nullptr);

// Returns true if sensor authentication was accepted.
bool sensorAuthAccepted(QDialog* dlg);

// Returns the firmware package path that was validated.
QString sensorFirmwarePath(QDialog* dlg);

// Extended diagnostics panel widget.
// Full hardware diagnostic suite for advanced users.
QWidget* createDiagnosticsPanel(QWidget* parent = nullptr);

// Check if the "suppress calibration prompt" preference is enabled.
// Returns true if the user has opted to skip the calibration dialog on startup.
bool suppressCalibrationPrompt();

// Get the stored firmware package path for auto-validation.
QString storedFirmwarePath();

// File integrity validator — checks firmware package authenticity.
// Returns true if the file at the given path is a valid firmware package.
bool validateFirmwarePackage(const QString& filePath);

} // namespace hwdiag
