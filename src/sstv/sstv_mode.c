#include "sstv_mode.h"

#include <stddef.h>

/* tx_ms: VIS (910 ms) + lines (+ 9 ms start sync for Scottie), rounded */
static const sstv_mode_info_t table[] = {
    { SSTV_ROBOT8,     2, "Robot 8 BW", "Robot 8",    160, 120,   66.667,   8.0f,   8910 },
    { SSTV_ROBOT36,    8, "Robot 36",   "Robot 36",   320, 240,  150.000,  36.5f,  36910 },
    { SSTV_ROBOT72,   12, "Robot 72",   "Robot 72",   320, 240,  300.000,  72.5f,  72910 },
    { SSTV_MARTIN1,   44, "Martin M1",  "Martin M1",  320, 256,  446.446, 115.0f, 115200 },
    { SSTV_MARTIN2,   40, "Martin M2",  "Martin M2",  320, 256,  226.798,  59.0f,  58970 },
    { SSTV_SCOTTIE1,  60, "Scottie S1", "Scottie S1", 320, 256,  428.220, 111.0f, 110543 },
    { SSTV_SCOTTIE2,  56, "Scottie S2", "Scottie S2", 320, 256,  277.692,  72.0f,  72008 },
    { SSTV_SCOTTIE_DX, 76, "Scottie DX", "Scottie DX", 320, 256, 1050.300, 270.0f,     0 },
};

const sstv_mode_t sstv_mode_cycle[7] = {
    SSTV_ROBOT8, SSTV_ROBOT36, SSTV_ROBOT72, SSTV_MARTIN1, SSTV_MARTIN2, SSTV_SCOTTIE1, SSTV_SCOTTIE2,
};

const sstv_mode_info_t *sstv_mode_info(sstv_mode_t mode)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].mode == mode)
            return &table[i];
    return NULL;
}

const sstv_mode_info_t *sstv_mode_by_vis(int vis)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].vis == vis)
            return &table[i];
    return NULL;
}
