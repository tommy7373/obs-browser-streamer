#pragma once

#include <obs.h>

// Registers the "Telestrator Overlay" OBS source. The source renders the
// stroke list held by WebPreviewPlugin's telestrator-stream telestration
// state. Reaches into the plugin via the C-style accessor
// webpreview_get_telestration_state() declared in plugin-main.cpp so this
// translation unit doesn't need to drag in the full plugin header.
class TelestratorSource {
public:
    static void RegisterSourceType();
};
