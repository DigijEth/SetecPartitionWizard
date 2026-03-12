#pragma once

#include <QWidget>

namespace spw
{

class ImagingTab : public QWidget
{
    Q_OBJECT

public:
    explicit ImagingTab(QWidget* parent = nullptr);
    ~ImagingTab() override;

private:
    void setupUi();
};

} // namespace spw
