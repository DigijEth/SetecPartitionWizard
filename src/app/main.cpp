#include "SingleInstance.h"
#include "core/common/Logging.h"
#include "core/common/Version.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPalette>
#include <QStandardPaths>
#include <QString>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
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

static void applyDarkPalette(QApplication& app)
{
    // Set the application palette so every widget — including QProgressDialog,
    // QMessageBox, QInputDialog, and other system-rendered popups — gets the
    // correct dark background and white text regardless of stylesheets.
    QPalette p;

    // Background roles
    p.setColor(QPalette::Window,          QColor(0x1e, 0x1e, 0x2e));
    p.setColor(QPalette::WindowText,      QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::Base,            QColor(0x18, 0x18, 0x25));
    p.setColor(QPalette::AlternateBase,   QColor(0x1e, 0x1e, 0x2e));
    p.setColor(QPalette::ToolTipBase,     QColor(0x31, 0x32, 0x44));
    p.setColor(QPalette::ToolTipText,     QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::PlaceholderText, QColor(0x6e, 0x70, 0x80));

    // Text roles
    p.setColor(QPalette::Text,            QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::BrightText,      QColor(0xff, 0xff, 0xff));

    // Button roles
    p.setColor(QPalette::Button,          QColor(0x45, 0x47, 0x5a));
    p.setColor(QPalette::ButtonText,      QColor(0xff, 0xff, 0xff));

    // Selection
    p.setColor(QPalette::Highlight,       QColor(0x45, 0x47, 0x5a));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));

    // Links
    p.setColor(QPalette::Link,            QColor(0xd4, 0xa5, 0x74));
    p.setColor(QPalette::LinkVisited,     QColor(0xb0, 0x80, 0x50));

    // Disabled state — dimmed text, same dark backgrounds
    p.setColor(QPalette::Disabled, QPalette::WindowText,  QColor(0x6e, 0x70, 0x80));
    p.setColor(QPalette::Disabled, QPalette::Text,        QColor(0x6e, 0x70, 0x80));
    p.setColor(QPalette::Disabled, QPalette::ButtonText,  QColor(0x6e, 0x70, 0x80));
    p.setColor(QPalette::Disabled, QPalette::Base,        QColor(0x2a, 0x2a, 0x3a));
    p.setColor(QPalette::Disabled, QPalette::Button,      QColor(0x2a, 0x2a, 0x3a));
    p.setColor(QPalette::Disabled, QPalette::Highlight,   QColor(0x31, 0x32, 0x44));

    app.setPalette(p);
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

    // Apply dark palette first (catches system-rendered widgets like QProgressDialog,
    // QMessageBox internals that ignore stylesheets on Windows), then apply
    // the stylesheet on top for fine-grained visual control.
    applyDarkPalette(app);
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
