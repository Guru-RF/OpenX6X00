/* Host test shim (tools/panel_stripes): the mode set's callback, kept by the test */
#pragma once

void drm_set_modeset_cb(void (*cb)(void));
