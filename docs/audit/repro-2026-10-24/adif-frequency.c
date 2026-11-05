#include "src/adif.c"
#include <string.h>
#include <unistd.h>
/* B03, since #174 through the formatter the day's log shares (field_adif_record): 145.5 MHz within its buffer */
int main(void) {
    char path[] = "/tmp/openx6x00-audit-adif-XXXXXX", buf[1024] = "";
    int fd = mkstemp(path);
    qso_log_record_t q = {.local_call = "ON6URE", .remote_call = "F4ABC", .time = 1793000000, .mode = MODE_FT8,
                          .rsts = -10, .rstr = -12, .freq_mhz = 145.5f, .local_grid = "JO20", .remote_grid = "JN18"};
    if (fd < 0) return 2;
    close(fd);
    unlink(path);
    adif_log l = adif_log_init(path);
    if (!l) return 3;
    adif_add_qso(l, q);
    adif_log_close(l);
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    unlink(path);
    return strstr(buf, "<FREQ:10>145.500000 ") && strstr(buf, "<BAND:2>2m ") ? 0 : 1;
}
