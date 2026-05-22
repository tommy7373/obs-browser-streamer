#include "WebPreviewDock.hpp"
#include "WebPreviewPlugin.hpp"
#include "BrowserStreamerSettings.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QMessageBox>

static const char* kDotStopped   = "background-color:#cc2222;border-radius:6px;";
static const char* kDotStreaming = "background-color:#22cc22;border-radius:6px;";

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

    // ---- Telestrator section ----
    auto* telRow = new QWidget(this);
    auto* telRowLayout = new QHBoxLayout(telRow);
    telRowLayout->setContentsMargins(0, 0, 0, 0);
    telRowLayout->setSpacing(6);

    telStatusDot_ = new QLabel(telRow);
    telStatusDot_->setFixedSize(12, 12);
    telStatusDot_->setStyleSheet(kDotStopped);
    telRowLayout->addWidget(telStatusDot_);

    telEnableBox_ = new QCheckBox(obs_module_text("WebPreview.EnableTelestrator"), telRow);
    telEnableBox_->setChecked(plugin_->IsTelestratorEnabled());
    connect(telEnableBox_, &QCheckBox::toggled, this, &WebPreviewDock::OnTelestratorToggled);
    telRowLayout->addWidget(telEnableBox_);

    telSourceCombo_ = new QComboBox(telRow);
    telSourceCombo_->setMinimumWidth(140);
    PopulateTelestratorSources();
    connect(telSourceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WebPreviewDock::OnTelestratorSourceChanged);
    telRowLayout->addWidget(telSourceCombo_, 1);

    mainLayout->addWidget(telRow);

    telUrlLabel_ = new QLabel("", this);
    telUrlLabel_->setWordWrap(true);
    telUrlLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    telUrlLabel_->setStyleSheet("color:#aaa;font-size:11px;");
    mainLayout->addWidget(telUrlLabel_);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(sep);

    // Container for stream rows
    auto* rowsContainer = new QWidget(this);
    streamRowsLayout_ = new QVBoxLayout(rowsContainer);
    streamRowsLayout_->setContentsMargins(0, 0, 0, 0);
    streamRowsLayout_->setSpacing(4);
    mainLayout->addWidget(rowsContainer);

    // URL label at the bottom (regular streams)
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
    UpdateTelestratorRow();
}

void WebPreviewDock::PopulateTelestratorSources()
{
    QString current = QString::fromStdString(plugin_->GetTelestratorConfig().sourceName);

    QSignalBlocker blocker(telSourceCombo_);
    telSourceCombo_->clear();
    telSourceCombo_->addItem(obs_module_text("WebPreview.NoSource"), QString());

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++) {
        const char* name = obs_source_get_name(scenes.sources.array[i]);
        if (name)
            telSourceCombo_->addItem(QString("[Scene] ") + name, QString(name));
    }
    obs_frontend_source_list_free(&scenes);

    obs_enum_sources([](void* param, obs_source_t* src) -> bool {
        auto* combo = static_cast<QComboBox*>(param);
        uint32_t caps = obs_source_get_output_flags(src);
        if (!(caps & OBS_SOURCE_VIDEO))
            return true;
        if (obs_source_get_type(src) == OBS_SOURCE_TYPE_SCENE)
            return true;
        const char* name = obs_source_get_name(src);
        if (name)
            combo->addItem(QString("[Source] ") + name, QString(name));
        return true;
    }, telSourceCombo_);

    if (!current.isEmpty()) {
        int idx = telSourceCombo_->findData(current);
        if (idx >= 0)
            telSourceCombo_->setCurrentIndex(idx);
    }
}

void WebPreviewDock::OnTelestratorSourceChanged(int)
{
    QString src = telSourceCombo_->currentData().toString();
    StreamConfig cfg = plugin_->GetTelestratorConfig();
    cfg.sourceName = src.toStdString();
    plugin_->SetTelestratorConfig(cfg);
    plugin_->SaveSettings();
    UpdateTelestratorRow();
}

void WebPreviewDock::OnTelestratorToggled(bool checked)
{
    plugin_->SetTelestratorEnabled(checked);
    plugin_->SaveSettings();

    if (checked) {
        if (plugin_->GetTelestratorConfig().sourceName.empty()) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorNoSource"));
            QSignalBlocker b(telEnableBox_);
            telEnableBox_->setChecked(false);
            plugin_->SetTelestratorEnabled(false);
            plugin_->SaveSettings();
            UpdateTelestratorRow();
            return;
        }
        if (!plugin_->StartTelestrator()) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorStartFailed"));
            QSignalBlocker b(telEnableBox_);
            telEnableBox_->setChecked(false);
            plugin_->SetTelestratorEnabled(false);
            plugin_->SaveSettings();
        }
    } else {
        plugin_->StopTelestrator();
    }
    UpdateTelestratorRow();
}

void WebPreviewDock::UpdateTelestratorRow()
{
    bool running = plugin_->IsTelestratorStreaming();
    telStatusDot_->setStyleSheet(running ? kDotStreaming : kDotStopped);

    if (running) {
        auto urls = plugin_->GetTelestratorUrls();
        QString text;
        for (const auto& u : urls) {
            if (!text.isEmpty()) text += "\n";
            text += QString::fromStdString(u);
        }
        telUrlLabel_->setText(text);
    } else {
        telUrlLabel_->clear();
    }
}

void WebPreviewDock::RebuildStreamRows()
{
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

        rows_[i].statusDot = new QLabel(rowWidget);
        rows_[i].statusDot->setFixedSize(12, 12);
        rows_[i].statusDot->setStyleSheet(kDotStopped);
        rowLayout->addWidget(rows_[i].statusDot);

        rows_[i].nameLabel = new QLabel(rowWidget);
        rows_[i].nameLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rowLayout->addWidget(rows_[i].nameLabel);

        rows_[i].sourceLabel = new QLabel(rowWidget);
        rows_[i].sourceLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        rowLayout->addWidget(rows_[i].sourceLabel, 1);

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

    const auto& cfg = plugin_->GetConfig(idx);
    rows_[idx].statusDot->setStyleSheet(streaming ? kDotStreaming : kDotStopped);
    rows_[idx].nameLabel->setText(QString::number(idx + 1));
    rows_[idx].sourceLabel->setText(cfg.sourceName.empty()
        ? QString("(no source)")
        : QString::fromStdString(cfg.sourceName));
    rows_[idx].startStopBtn->setText(obs_module_text(
        streaming ? "WebPreview.Stop" : "WebPreview.Start"));
}

void WebPreviewDock::OnPollTimer()
{
    int n = plugin_->GetNumStreams();
    if (static_cast<int>(rows_.size()) != n)
        RebuildStreamRows();

    for (int i = 0; i < n; ++i)
        UpdateStreamRow(i);

    UpdateTelestratorRow();

    bool anyStreaming = plugin_->IsTelestratorStreaming();
    for (int i = 0; i < n && !anyStreaming; ++i)
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
        PopulateTelestratorSources();
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
