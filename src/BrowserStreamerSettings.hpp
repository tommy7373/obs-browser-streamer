#pragma once
#include <QDialog>
#include <QComboBox>
#include <QGroupBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <vector>

class WebPreviewPlugin;

class BrowserStreamerSettings : public QDialog {
    Q_OBJECT
public:
    explicit BrowserStreamerSettings(WebPreviewPlugin* plugin, QWidget* parent = nullptr);

private slots:
    void OnStreamCountChanged(int n);
    void OnAccepted();

private:
    void AddStreamRow(int idx);
    void RemoveLastStreamRow();
    void PopulateSources(QComboBox* combo, const QString& currentSource);

    struct StreamRow {
        QGroupBox* group      = nullptr;
        QComboBox* sourceCombo= nullptr;
        QSpinBox*  bitrateSpin= nullptr;
    };

    WebPreviewPlugin* plugin_;
    QSpinBox*         countSpin_    = nullptr;
    QSpinBox*         portSpin_     = nullptr;
    QVBoxLayout*      streamsLayout_= nullptr;
    std::vector<StreamRow> rows_;
};
