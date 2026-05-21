#include "BrowserStreamerSettings.hpp"
#include "WebPreviewPlugin.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

BrowserStreamerSettings::BrowserStreamerSettings(WebPreviewPlugin* plugin, QWidget* parent)
    : QDialog(parent)
    , plugin_(plugin)
{
    setWindowTitle(obs_module_text("WebPreview.Settings"));
    setMinimumWidth(400);

    auto* mainLayout = new QVBoxLayout(this);

    // Port + stream count form
    auto* form = new QFormLayout();
    form->setSpacing(6);

    portSpin_ = new QSpinBox(this);
    portSpin_->setRange(1024, 65535);
    portSpin_->setValue(plugin_->GetPort());
    form->addRow(obs_module_text("WebPreview.Port"), portSpin_);

    countSpin_ = new QSpinBox(this);
    countSpin_->setRange(1, 8);
    countSpin_->setValue(plugin_->GetNumStreams());
    form->addRow(obs_module_text("WebPreview.StreamCount"), countSpin_);

    mainLayout->addLayout(form);

    // Scroll area for stream rows
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumHeight(300);

    auto* scrollContent = new QWidget(scrollArea);
    streamsLayout_ = new QVBoxLayout(scrollContent);
    streamsLayout_->setContentsMargins(4, 4, 4, 4);
    streamsLayout_->setSpacing(6);
    streamsLayout_->addStretch();

    scrollArea->setWidget(scrollContent);
    mainLayout->addWidget(scrollArea, 1);

    // Button box
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &BrowserStreamerSettings::OnAccepted);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    // Connect count spinner
    connect(countSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &BrowserStreamerSettings::OnStreamCountChanged);

    // Populate initial rows
    int n = plugin_->GetNumStreams();
    for (int i = 0; i < n; ++i)
        AddStreamRow(i);
}

void BrowserStreamerSettings::AddStreamRow(int idx)
{
    StreamRow row;
    row.group = new QGroupBox(QString("Stream %1").arg(idx + 1));

    auto* grpLayout = new QFormLayout(row.group);
    grpLayout->setSpacing(4);

    row.sourceCombo = new QComboBox(row.group);
    QString currentSource = QString::fromStdString(plugin_->GetConfig(idx).sourceName);
    PopulateSources(row.sourceCombo, currentSource);
    grpLayout->addRow(obs_module_text("WebPreview.Source"), row.sourceCombo);

    row.bitrateSpin = new QSpinBox(row.group);
    row.bitrateSpin->setRange(500, 20000);
    row.bitrateSpin->setSuffix(" kbps");
    row.bitrateSpin->setValue(plugin_->GetConfig(idx).bitrateKbps);
    grpLayout->addRow(obs_module_text("WebPreview.Bitrate"), row.bitrateSpin);

    rows_.push_back(row);

    // Insert before the trailing stretch (last item)
    int insertPos = streamsLayout_->count() - 1;
    streamsLayout_->insertWidget(insertPos, row.group);
}

void BrowserStreamerSettings::RemoveLastStreamRow()
{
    if (rows_.empty())
        return;

    StreamRow& last = rows_.back();
    streamsLayout_->removeWidget(last.group);
    delete last.group;
    rows_.pop_back();
}

void BrowserStreamerSettings::OnStreamCountChanged(int n)
{
    while (static_cast<int>(rows_.size()) < n)
        AddStreamRow(static_cast<int>(rows_.size()));
    while (static_cast<int>(rows_.size()) > n)
        RemoveLastStreamRow();
}

void BrowserStreamerSettings::OnAccepted()
{
    plugin_->SetNumStreams(countSpin_->value());
    plugin_->SetPort(portSpin_->value());

    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        StreamConfig cfg = plugin_->GetConfig(i); // preserve name
        cfg.sourceName   = rows_[i].sourceCombo->currentData().toString().toStdString();
        cfg.bitrateKbps  = rows_[i].bitrateSpin->value();
        plugin_->SetConfig(i, cfg);
    }

    plugin_->SaveSettings();
    accept();
}

void BrowserStreamerSettings::PopulateSources(QComboBox* combo, const QString& currentSource)
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

    if (!currentSource.isEmpty()) {
        int idx = combo->findData(currentSource);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
    }
}
