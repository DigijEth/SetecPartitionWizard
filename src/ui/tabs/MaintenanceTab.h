#pragma once

#include <QWidget>

namespace spw
{

class MaintenanceTab : public QWidget
{
    Q_OBJECT

public:
    explicit MaintenanceTab(QWidget* parent = nullptr);
    ~MaintenanceTab() override;

private:
    void setupUi();
};

} // namespace spw
