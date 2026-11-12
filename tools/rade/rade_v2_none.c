/*
 *  OpenX6X00 - RADE V1 only: the V2 functions rade_c's rade_api.c calls, in place of rade_c's V2 sources.
 *
 *  rade_api.c picks V1 or V2 at run time (rade_open()'s RADE_MODE_V2), so it refers to both, and linking it with
 *  V2's sources brings V2's encoder, decoder and frame-sync networks with their weights into the GUI, which opens
 *  V1 only (rade_open("", ...)). tools/rade/build.sh compiles this file into librade.a instead of them: a
 *  rade_open() with RADE_MODE_V2 fails at the first call here (and returns NULL); with V1 none of these is ever
 *  called. Compiled against rade_c's own V2 headers, so a prototype changed upstream stops the build here, and a
 *  V2 function rade_api.c calls that is missing here stops tools/rade/build.sh's link check.
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include <stdio.h>

#include "rade_api.h"
#include "rade_tx_v2.h"
#include "rade_rx_v2.h"

static int no_v2(void)
{
    fprintf(stderr, "RADE V2 is not built in (V1 only, tools/rade/build.sh)\n");
    return -1;
}

int rade_tx_v2_init(rade_tx_v2_state *tx, int bpf_en)
{
    (void)tx; (void)bpf_en;
    return no_v2();
}

int rade_rx_v2_init(rade_rx_v2_state *rx, int bpf_en)
{
    (void)rx; (void)bpf_en;
    return no_v2();
}

int rade_tx_v2_n_features_in(void) { return 0; }
int rade_tx_v2_n_samples_out(void) { return 0; }
int rade_tx_v2_n_eoo_out(void) { return 0; }

int rade_tx_v2_process(rade_tx_v2_state *tx, RADE_COMP *tx_out, const float *features_in)
{
    (void)tx; (void)tx_out; (void)features_in;
    return 0;
}

int rade_tx_v2_eoo(rade_tx_v2_state *tx, RADE_COMP *tx_out)
{
    (void)tx; (void)tx_out;
    return 0;
}

void rade_tx_v2_set_data_symbol(rade_tx_v2_state *tx, float symbol)
{
    (void)tx; (void)symbol;
}

int rade_rx_v2_nin(const rade_rx_v2_state *rx)
{
    (void)rx;
    return 0;
}

int rade_rx_v2_nin_max(void) { return 0; }
int rade_rx_v2_n_features_out(void) { return 0; }

int rade_rx_v2_process(rade_rx_v2_state *rx, float *features_out, const RADE_COMP *rx_in)
{
    (void)rx; (void)features_out; (void)rx_in;
    return 0;
}

float rade_rx_v2_get_data_symbol(const rade_rx_v2_state *rx)
{
    (void)rx;
    return 0.0f;
}
