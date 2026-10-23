#include "src/adif.c"
int main(void) { FILE *f=tmpfile(); write_freq(f,145.5f); fclose(f); return 0; }
