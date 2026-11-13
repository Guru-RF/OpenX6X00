/* Host test shim (tools/smartcw): a CW message on the air or not, as the test says */
#pragma once

typedef enum { CW_ENCODER_IDLE = 0, CW_ENCODER_BEEP } cw_encoder_state_t;

cw_encoder_state_t cw_encoder_state(void);
