#include <stdio.h>
#include "src/field/flog.h"
int main(int argc, char **argv) {
    field_qso_t out[1] = {{0}};
    printf("records=%d\n", field_log_read(argv[1], out, 1));
    return 0;
}
