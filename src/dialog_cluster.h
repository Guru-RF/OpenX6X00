#pragma once

#include "dialog.h"
#include "cluster/spots.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_cluster;

/* Starts the DX cluster client in the background (after the params are
 * loaded); it runs when a callsign is set. */
void dialog_cluster_start_client(void);

/* Tunes the radio to a spot: frequency and the mode its comment / the band plan suggests. False, and a message,
 * where the radio cannot tune (4 m, 2 m, 70 cm with no transverter for it, nor the X6200's WR12) */
bool dialog_cluster_tune_spot(const cluster_spot_t *s);
/* The last spot tuned on the radio, while freq_hz is within CLUSTER_TUNED_NEAR_HZ of it: its call and the activity
 * reference of its comment ("" none, cluster_spot_ref). False, both "", with no spot tuned or the radio away from it.
 * For the quick QSO log (#245). LVGL thread */
#define CLUSTER_TUNED_NEAR_HZ 3000
bool dialog_cluster_tuned_near(int32_t freq_hz, char *call, size_t size, char *ref, size_t ref_size);

/* MFK press while an alert popup is shown: tunes to its spot and closes it.
 * False when there is no popup. */
bool dialog_cluster_alert_press(void);

#ifdef __cplusplus
}
#endif
