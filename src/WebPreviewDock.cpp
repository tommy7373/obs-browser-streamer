#include "WebPreviewDock.hpp"
#include "WebPreviewPlugin.hpp"
#include "BrowserStreamerSettings.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QHBoxLayout>
#include <QMessageBox>

static const char* kDotStopped   = "background-color:#cc2222;border-radius:6px;";
static const char* kDotStreaming  = "background-color:#22cc22;border-radius:6px;";

WebPreviewDock::WebPreviewDock(WebPreviewPlugin* plugin, QWidget* parent)
    : QWidget(parent)
    , plugin_(plugin)
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    mainLayout->setSpacing(6);

    // Settings button at the top
    auto* settingsBtn = new QPushButton(obs_module_text("WebPreview.Settings"), this);
    connect(settingsBtn, &QPushButton::clicked, this, &WebPreviewDock::OnSettingsClicked);
    mainLayout->addWidget(settingsBtn);

    // Container for stream rows
    auto* rowsContainer = new QWidget(this);
    streamRowsLayout_ = new QVBoxLayout(rowsContainer);
    streamRowsLayout_->setContentsMargins(0, 0, 0, 0);
    streamRowsLayout_->setSpacing(4);
    mainLayout->addWidget(rowsContainer);

    // URL label at the bottom
    urlLabel_ = new QLabel("", this);
    urlLabel_->setWordWrap(true);
    urlLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(urlLabel_);

    mainLayout->addStretch();

    // Poll timer
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(1000);
    connect(pollTimer_, &QTimer::timeout, this, &WebPreviewDock::OnPollTimer);
    pollTimer_->start();

    RebuildStreamRows();
}

void WebPreviewDock::RebuildStreamRows()
{
    // Clear existing rows
    QLayoutItem* item;
    while ((item = streamRowsLayout_->takeAt(0))) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    rows_.clear();

    int n = plugin_->GetNumStreams();
    rows_.resize(n);

    for (int i = 0; i < n; ++i) {
        auto* rowWidget = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);

        // Status dot
        rows_[i].statusDot = new QLabel(rowWidget);
        rows_[i].statusDot->setFixedSize(12, 12);
        rows_[i].statusDot->setStyleSheet(kDotStopped);
        rowLayout->addWidget(rows_[i].statusDot);

        // Name label (flex)
        rows_[i].nameLabel = new QLabel(rowWidget);
        rowLayout->addWidget(rows_[i].nameLabel, 1);

        // Start/Stop button
        rows_[i].startStopBtn = new QPushButton(rowWidget);
        connect(rows_[i].startStopBtn, &QPushButton::clicked,
                this, [this, i]() { OnStartStopImpl(i); });
        rowLayout->addWidget(rows_[i].startStopBtn);

        streamRowsLayout_->addWidget(rowWidget);

        UpdateStreamRow(i);
    }
}

void WebPreviewDock::UpdateStreamRow(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(rows_.size()))
        return;

    bool streaming = plugin_->IsStreaming(idx);

    rows_[idx].statusDot->setStyleSheet(streaming ? kDotStreaming : kDotStopped);
    rows_[idx].nameLabel->setText(QString::fromStdString(plugin_->GetConfig(idx).name));
    rows_[idx].startStopBtn->setText(obs_module_text(
        streaming ? "WebPreview.Stop" : "WebPreview.Start"));
}

void WebPreviewDock::OnPollTimer()
{
    int n = plugin_->GetNumStreams();
    // Rebuild rows if count changed (e.g. settings were changed externally)
    if (static_cast<int>(rows_.size()) != n)
        RebuildStreamRows();

    for (int i = 0; i < n; ++i)
        UpdateStreamRow(i);

    bool anyStreaming = false;
    for (int i = 0; i < n; ++i)
        if (plugin_->IsStreaming(i)) { anyStreaming = true; break; }

    if (anyStreaming) {
        auto urls = plugin_->GetLandingUrls();
        QString text;
        for (const auto& u : urls) {
            if (!text.isEmpty()) text += "\n";
            text += QString::fromStdString(u);
        }
        urlLabel_->setText(text);
    } else {
        urlLabel_->clear();
    }
}

void WebPreviewDock::OnSettingsClicked()
{
    BrowserStreamerSettings dlg(plugin_, this);
    if (dlg.exec() == QDialog::Accepted) {
        RebuildStreamRows();
    }
}

void WebPreviewDock::OnStartStopImpl(int idx)
{
    if (plugin_->IsStreaming(idx)) {
        plugin_->Stop(idx);
    } else {
        if (plugin_->GetConfig(idx).sourceName.empty()) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorNoSource"));
            return;
        }
        if (!plugin_->Start(idx)) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorStartFailed"));
        }
    }
    UpdateStreamRow(idx);
}
