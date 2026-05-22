#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>

class WebPreviewPlugin;

struct StreamWidgets {
    QLabel*      statusDot    = nullptr;
    QLabel*      nameLabel    = nullptr;
    QLabel*      sourceLabel  = nullptr;
    QPushButton* startStopBtn = nullptr;
};

class WebPreviewDock : public QWidget {
    Q_OBJECT
public:
    explicit WebPreviewDock(WebPreviewPlugin* plugin, QWidget* parent = nullptr);
    void RebuildStreamRows();

private slots:
    void OnPollTimer();
    void OnSettingsClicked();
    void OnTelestratorToggled(bool checked);
    void OnTelestratorSourceChanged(int index);

private:
    void UpdateStreamRow(int idx);
    void OnStartStopImpl(int idx);
    void UpdateTelestratorRow();
    void PopulateTelestratorSources();

    WebPreviewPlugin*        plugin_;
    QVBoxLayout*             streamRowsLayout_ = nullptr;
    std::vector<StreamWidgets> rows_;

    // Telestrator controls (top of dock)
    QCheckBox*               telEnableBox_   = nullptr;
    QComboBox*               telSourceCombo_ = nullptr;
    QLabel*                  telStatusDot_   = nullptr;
    QLabel*                  telUrlLabel_    = nullptr;

    QLabel*                  urlLabel_  = nullptr;
    QTimer*                  pollTimer_ = nullptr;
};
