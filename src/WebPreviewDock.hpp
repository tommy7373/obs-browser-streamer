#pragma once

#include <QWidget>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

class WebPreviewPlugin;

struct StreamWidgets {
    QGroupBox*   group       = nullptr;
    QComboBox*   sourceCombo = nullptr;
    QSpinBox*    bitrateSpin = nullptr;
    QPushButton* startStopBtn= nullptr;
    QLabel*      statusDot   = nullptr;
    QLabel*      statusLabel = nullptr;
    QLabel*      viewerLabel = nullptr;
};

class WebPreviewDock : public QWidget {
    Q_OBJECT

public:
    explicit WebPreviewDock(WebPreviewPlugin* plugin, QWidget* parent = nullptr);

private slots:
    void OnPollTimer();

private:
    void BuildStreamGroup(int idx, QWidget* parent, class QVBoxLayout* layout);
    void PopulateSources(QComboBox* combo);
    void UpdateStreamUi(int idx);
    void LoadSettings();
    void SaveSettings();
    void OnStartStopImpl(int idx);

    WebPreviewPlugin* plugin_;
    StreamWidgets     sw_[2];
    QSpinBox*         portSpin_  = nullptr;
    QLabel*           urlLabel_  = nullptr;
    QTimer*           pollTimer_ = nullptr;
};
