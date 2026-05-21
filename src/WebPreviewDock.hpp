#pragma once
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <vector>

class WebPreviewPlugin;

struct StreamWidgets {
    QLabel*      statusDot    = nullptr;
    QLabel*      nameLabel    = nullptr;
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

private:
    void UpdateStreamRow(int idx);
    void OnStartStopImpl(int idx);

    WebPreviewPlugin*        plugin_;
    QVBoxLayout*             streamRowsLayout_ = nullptr;
    std::vector<StreamWidgets> rows_;
    QLabel*                  urlLabel_  = nullptr;
    QTimer*                  pollTimer_ = nullptr;
};
