#pragma once

#include <QWidget>

namespace spw
{

class RecoveryTab : public QWidget
{
    Q_OBJECT

public:
    explicit RecoveryTab(QWidget* parent = nullptr);
    ~RecoveryTab() override;

private:
    void setupUi();
};

} // namespace spw
