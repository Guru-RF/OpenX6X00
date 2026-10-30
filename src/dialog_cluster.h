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
/* The call of the last spot tuned on the radio; the result changes at each one */
uint32_t dialog_cluster_tuned(char *call, size_t size);

/* MFK press while an alert popup is shown: tunes to its spot and closes it.
 * False when there is no popup. */
bool dialog_cluster_alert_press(void);

#ifdef __cplusplus
}
#endif
