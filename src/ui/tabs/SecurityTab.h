#pragma once

#include <QWidget>

namespace spw
{

class SecurityTab : public QWidget
{
    Q_OBJECT

public:
    explicit SecurityTab(QWidget* parent = nullptr);
    ~SecurityTab() override;

private:
    void setupUi();
};

} // namespace spw
