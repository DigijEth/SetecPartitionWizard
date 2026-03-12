#pragma once

#include <QWidget>

namespace spw
{

class DiagnosticsTab : public QWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsTab(QWidget* parent = nullptr);
    ~DiagnosticsTab() override;

private:
    void setupUi();
};

} // namespace spw
