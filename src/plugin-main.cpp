#include <obs-module.h>
#include <obs-frontend-api.h>

#include "WebPreviewPlugin.hpp"
#include "WebPreviewDock.hpp"
#include "WebPreviewOutput.hpp"
#include "TelestratorSource.hpp"
#include "Telestration.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-web-preview", "en-US")

static WebPreviewPlugin* g_plugin = nullptr;
static WebPreviewDock*   g_dock   = nullptr;

// Exposed to TelestratorSource so it can read the singleton plugin's
// telestrator-stream stroke list without dragging in WebPreviewPlugin's full
// header (the source TU stays graphics-only and doesn't touch libdatachannel).
extern "C" TelestrationState* webpreview_get_telestration_state()
{
    return g_plugin ? &g_plugin->TelestrationStateRef() : nullptr;
}

static void FrontendEventCallback(enum obs_frontend_event event, void*)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;

    g_plugin = new WebPreviewPlugin();
    g_dock   = new WebPreviewDock(g_plugin);

    obs_frontend_add_dock_by_id("obs-web-preview-dock",
                                "Telestrator++",
                                g_dock);
}

bool obs_module_load()
{
    WebPreviewOutput::RegisterOutputType();
    TelestratorSource::RegisterSourceType();
    obs_frontend_add_event_callback(FrontendEventCallback, nullptr);
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_dock("obs-web-preview-dock");
    g_dock = nullptr;

    if (g_plugin) {
        delete g_plugin;
        g_plugin = nullptr;
    }
}

const char* obs_module_name()        { return "Telestrator++"; }
const char* obs_module_description() { return "Native OBS telestration plus WebRTC scene streaming to any browser."; }
