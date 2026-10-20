#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_cw_trainer;

/* Learn mode: the group being played or keyed back ("" when none). For tests. */
const char *dialog_cw_trainer_target(void);

#ifdef __cplusplus
}
#endif
