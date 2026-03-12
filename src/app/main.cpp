#include "SingleInstance.h"
#include "core/common/Logging.h"
#include "core/common/Version.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QString>

#ifdef _WIN32
#include <Windows.h>
#endif

static bool isRunningAsAdmin()
{
#ifdef _WIN32
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
#else
    return geteuid() == 0;
#endif
}

static bool relaunchAsAdmin()
{
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) != FALSE;
#else
    return false;
#endif
}

static void applyStylesheet(QApplication& app)
{
    QFile styleFile(":/styles/default.qss");
    if (styleFile.open(QFile::ReadOnly | QFile::Text))
    {
        app.setStyleSheet(styleFile.readAll());
        styleFile.close();
    }
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(spw::AppName);
    app.setApplicationVersion(spw::VersionString);
    app.setOrganizationName("Setec");

    // Single instance check
    spw::SingleInstance instance(QStringLiteral("SetecPartitionWizard_SingleInstance_Mutex"));
    if (!instance.isUnique())
    {
        QMessageBox::warning(nullptr, spw::AppName,
                             "Setec Partition Wizard is already running.");
        return 1;
    }

    // Initialize logging
    auto logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(logDir);
    auto logPath = logDir + "/setec_partition_wizard.log";
    spw::log::init(logPath.toStdString());

    spw::log::info(QString("Starting %1 v%2").arg(spw::AppName, spw::VersionString));

    // Admin privilege check (defense-in-depth; manifest should handle UAC)
    if (!isRunningAsAdmin())
    {
        spw::log::warn("Not running as administrator — attempting elevation");
        auto answer = QMessageBox::question(
            nullptr, spw::AppName,
            "Setec Partition Wizard requires administrator privileges for disk operations.\n\n"
            "Restart as administrator?",
            QMessageBox::Yes | QMessageBox::No);

        if (answer == QMessageBox::Yes)
        {
            if (relaunchAsAdmin())
            {
                return 0; // Exit this instance; elevated one will start
            }
            QMessageBox::critical(nullptr, spw::AppName, "Failed to elevate privileges.");
        }
        // Continue anyway — some read-only features may still work
        spw::log::warn("Continuing without admin privileges — write operations will fail");
    }

    // Apply stylesheet
    applyStylesheet(app);

    // Show main window
    spw::MainWindow mainWindow;
    mainWindow.show();

    spw::log::info("Application ready");

    int result = app.exec();

    spw::log::info("Application shutting down");
    spw::log::shutdown();

    return result;
}
