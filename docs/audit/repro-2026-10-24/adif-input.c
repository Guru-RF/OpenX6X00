#include "adif.h"
#include <stdio.h>
#include <stdlib.h>
qso_log_band_t qso_log_freq_to_band(uint64_t f) { (void)f; return BAND_OTHER; }
int main(int argc, char **argv) {
    qso_log_record_t *r = NULL;
    int skipped = 0;
    if (argc != 2) return 2;
    int n = adif_read(argv[1], &r, &skipped);
    printf("records=%d skipped=%d\n", n, skipped);
    free(r);
    return 0;
}
