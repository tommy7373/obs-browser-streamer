#include <obs-module.h>
#include <obs-frontend-api.h>

#include "WebPreviewPlugin.hpp"
#include "WebPreviewDock.hpp"
#include "WebPreviewOutput.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-web-preview", "en-US")

static WebPreviewPlugin* g_plugin = nullptr;
static WebPreviewDock*   g_dock   = nullptr;

static void FrontendEventCallback(enum obs_frontend_event event, void*)
{
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;

    g_plugin = new WebPreviewPlugin();
    g_dock   = new WebPreviewDock(g_plugin);

    obs_frontend_add_dock_by_id("obs-web-preview-dock",
                                "Browser Streamer",
                                g_dock);
}

bool obs_module_load()
{
    WebPreviewOutput::RegisterOutputType();
    obs_frontend_add_event_callback(FrontendEventCallback, nullptr);
    return true;
}

void obs_module_unload()
{
    obs_frontend_remove_dock("obs-web-preview-dock");
    // g_dock is owned by OBS after add_dock_by_id — do not delete
    g_dock = nullptr;

    if (g_plugin) {
        delete g_plugin;
        g_plugin = nullptr;
    }
}

const char* obs_module_name()        { return "OBS Web Preview"; }
const char* obs_module_description() { return "Streams a scene or source to browsers via WebRTC."; }
