#ifndef DIALOG_FREEDV_H
#define DIALOG_FREEDV_H
#include "app_window.h"     /* dialog_t */
#if defined(__cplusplus)
extern "C" {
#endif /* __cplusplus */
#include <stdbool.h>
extern dialog_t *dialog_freedv;     /* RADE (FreeDV): the Modes menu opens it */
bool dialog_freedv_is_recording(void);  /* the hand mic goes to the encoder (audio thread) */
#if defined(__cplusplus)
}   /* extern "C" */
#endif /* __cplusplus */
#endif /* DIALOG_FREEDV_H */
