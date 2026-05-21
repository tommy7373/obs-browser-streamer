#include "WebPreviewDock.hpp"
#include "WebPreviewPlugin.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

static const char* kDotStopped  = "background-color:#cc2222;border-radius:6px;";
static const char* kDotStreaming = "background-color:#22cc22;border-radius:6px;";

WebPreviewDock::WebPreviewDock(WebPreviewPlugin* plugin, QWidget* parent)
    : QWidget(parent)
    , plugin_(plugin)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    BuildStreamGroup(0, this, layout);
    BuildStreamGroup(1, this, layout);

    // Shared port control (single server serves both streams)
    auto* form = new QFormLayout();
    form->setSpacing(4);
    portSpin_ = new QSpinBox(this);
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(8080);
    form->addRow(obs_module_text("WebPreview.Port"), portSpin_);
    layout->addLayout(form);

    urlLabel_ = new QLabel("", this);
    urlLabel_->setWordWrap(true);
    urlLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(urlLabel_);

    layout->addStretch();

    // Save settings when port changes
    connect(portSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &WebPreviewDock::SaveSettings);

    // Poll timer updates viewer counts and streaming state
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(1000);
    connect(pollTimer_, &QTimer::timeout, this, &WebPreviewDock::OnPollTimer);
    pollTimer_->start();

    LoadSettings();
}

void WebPreviewDock::BuildStreamGroup(int idx, QWidget* parent, QVBoxLayout* layout)
{
    auto* group = new QGroupBox(idx == 0 ? "Stream 1" : "Stream 2", parent);
    sw_[idx].group = group;

    auto* grpLayout = new QVBoxLayout(group);
    grpLayout->setContentsMargins(6, 8, 6, 6);
    grpLayout->setSpacing(4);

    // Source + bitrate
    auto* form = new QFormLayout();
    form->setSpacing(4);

    sw_[idx].sourceCombo = new QComboBox(group);
    PopulateSources(sw_[idx].sourceCombo);
    form->addRow(obs_module_text("WebPreview.Source"), sw_[idx].sourceCombo);

    sw_[idx].bitrateSpin = new QSpinBox(group);
    sw_[idx].bitrateSpin->setRange(500, 20000);
    sw_[idx].bitrateSpin->setValue(2500);
    sw_[idx].bitrateSpin->setSuffix(" kbps");
    form->addRow(obs_module_text("WebPreview.Bitrate"), sw_[idx].bitrateSpin);

    grpLayout->addLayout(form);

    // Status row
    auto* statusRow = new QHBoxLayout();
    sw_[idx].statusDot = new QLabel(group);
    sw_[idx].statusDot->setFixedSize(12, 12);
    sw_[idx].statusDot->setStyleSheet(kDotStopped);
    statusRow->addWidget(sw_[idx].statusDot);

    sw_[idx].statusLabel = new QLabel(obs_module_text("WebPreview.StatusStopped"), group);
    statusRow->addWidget(sw_[idx].statusLabel, 1);

    sw_[idx].viewerLabel = new QLabel("", group);
    sw_[idx].viewerLabel->setAlignment(Qt::AlignRight);
    statusRow->addWidget(sw_[idx].viewerLabel);
    grpLayout->addLayout(statusRow);

    sw_[idx].startStopBtn = new QPushButton(obs_module_text("WebPreview.Start"), group);
    sw_[idx].startStopBtn->setMinimumHeight(28);
    grpLayout->addWidget(sw_[idx].startStopBtn);

    layout->addWidget(group);

    // Wire buttons
    connect(sw_[idx].startStopBtn, &QPushButton::clicked,
            this, [this, idx]() { OnStartStopImpl(idx); });

    connect(sw_[idx].sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WebPreviewDock::SaveSettings);
    connect(sw_[idx].bitrateSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &WebPreviewDock::SaveSettings);
}

void WebPreviewDock::PopulateSources(QComboBox* combo)
{
    combo->clear();

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; i++) {
        const char* name = obs_source_get_name(scenes.sources.array[i]);
        if (name)
            combo->addItem(QString("[Scene] ") + name, QString(name));
    }
    obs_frontend_source_list_free(&scenes);

    obs_enum_sources([](void* param, obs_source_t* src) -> bool {
        auto* combo = static_cast<QComboBox*>(param);
        uint32_t caps = obs_source_get_output_flags(src);
        if (!(caps & OBS_SOURCE_VIDEO))
            return true;
        uint32_t type = obs_source_get_type(src);
        if (type == OBS_SOURCE_TYPE_SCENE)
            return true;
        const char* name = obs_source_get_name(src);
        if (name)
            combo->addItem(QString("[Source] ") + name, QString(name));
        return true;
    }, combo);
}

void WebPreviewDock::OnStartStopImpl(int idx)
{
    if (plugin_->IsStreaming(idx)) {
        plugin_->Stop(idx);
    } else {
        QString sourceName = sw_[idx].sourceCombo->currentData().toString();
        if (sourceName.isEmpty()) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorNoSource"));
            return;
        }
        int port    = portSpin_->value();
        int bitrate = sw_[idx].bitrateSpin->value();

        if (!plugin_->Start(idx, sourceName.toStdString(), port, bitrate)) {
            QMessageBox::warning(this,
                obs_module_text("WebPreview.DockTitle"),
                obs_module_text("WebPreview.ErrorStartFailed"));
        }
    }
    UpdateStreamUi(0);
    UpdateStreamUi(1);
}


void WebPreviewDock::OnPollTimer()
{
    UpdateStreamUi(0);
    UpdateStreamUi(1);

    bool anyStreaming = plugin_->IsStreaming(0) || plugin_->IsStreaming(1);
    if (anyStreaming) {
        // Landing page URLs — same for both streams (stream 0 = root path)
        auto urls = plugin_->GetStreamUrls(0);
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

void WebPreviewDock::UpdateStreamUi(int idx)
{
    bool streaming     = plugin_->IsStreaming(idx);
    bool anyStreaming   = plugin_->IsStreaming(0) || plugin_->IsStreaming(1);

    sw_[idx].statusDot->setStyleSheet(streaming ? kDotStreaming : kDotStopped);
    sw_[idx].statusLabel->setText(obs_module_text(
        streaming ? "WebPreview.StatusStreaming" : "WebPreview.StatusStopped"));
    sw_[idx].startStopBtn->setText(obs_module_text(
        streaming ? "WebPreview.Stop" : "WebPreview.Start"));

    sw_[idx].sourceCombo->setEnabled(!streaming);
    sw_[idx].bitrateSpin->setEnabled(!streaming);

    // Disable port spinner whenever any stream is running (shared server)
    portSpin_->setEnabled(!anyStreaming);

    if (streaming) {
        int viewers = plugin_->GetViewerCount(idx);
        sw_[idx].viewerLabel->setText(QString("Viewers: %1").arg(viewers));
    } else {
        sw_[idx].viewerLabel->clear();
    }
}

void WebPreviewDock::LoadSettings()
{
    char* path = obs_module_get_config_path(obs_current_module(), "settings.json");
    if (!path)
        return;
    obs_data_t* data = obs_data_create_from_json_file(path);
    bfree(path);
    if (!data)
        return;

    // Block signals so loading doesn't immediately trigger SaveSettings
    for (int i = 0; i < 2; ++i) {
        sw_[i].sourceCombo->blockSignals(true);
        sw_[i].bitrateSpin->blockSignals(true);
    }
    portSpin_->blockSignals(true);

    int port = static_cast<int>(obs_data_get_int(data, "port"));
    if (port >= 1024 && port <= 65535)
        portSpin_->setValue(port);

    const char* keys_source[]  = { "stream1_source",  "stream2_source"  };
    const char* keys_bitrate[] = { "stream1_bitrate", "stream2_bitrate" };

    for (int i = 0; i < 2; ++i) {
        int bitrate = static_cast<int>(obs_data_get_int(data, keys_bitrate[i]));
        if (bitrate >= 500 && bitrate <= 20000)
            sw_[i].bitrateSpin->setValue(bitrate);

        const char* source = obs_data_get_string(data, keys_source[i]);
        if (source && source[0]) {
            int idx = sw_[i].sourceCombo->findData(QString(source));
            if (idx >= 0)
                sw_[i].sourceCombo->setCurrentIndex(idx);
        }
    }

    for (int i = 0; i < 2; ++i) {
        sw_[i].sourceCombo->blockSignals(false);
        sw_[i].bitrateSpin->blockSignals(false);
    }
    portSpin_->blockSignals(false);

    obs_data_release(data);
}

void WebPreviewDock::SaveSettings()
{
    char* dir = obs_module_get_config_path(obs_current_module(), "");
    if (dir) {
        os_mkdirs(dir);
        bfree(dir);
    }

    char* path = obs_module_get_config_path(obs_current_module(), "settings.json");
    if (!path)
        return;

    obs_data_t* data = obs_data_create();
    obs_data_set_int(data, "port", portSpin_->value());

    const char* keys_source[]  = { "stream1_source",  "stream2_source"  };
    const char* keys_bitrate[] = { "stream1_bitrate", "stream2_bitrate" };

    for (int i = 0; i < 2; ++i) {
        obs_data_set_string(data, keys_source[i],
            sw_[i].sourceCombo->currentData().toString().toUtf8().constData());
        obs_data_set_int(data, keys_bitrate[i], sw_[i].bitrateSpin->value());
    }

    obs_data_save_json_safe(data, path, "tmp", "bak");
    obs_data_release(data);
    bfree(path);
}
