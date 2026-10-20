#pragma once

#include "dialog.h"
#include "wspr/wspr.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_wspr;


/* Shows spots as if this cycle had just decoded them. For tests. */
void dialog_wspr_show_spots(const wspr_spot_t *spots, int n);

#ifdef __cplusplus
}
#endif
