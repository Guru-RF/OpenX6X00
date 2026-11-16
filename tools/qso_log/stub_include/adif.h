/* tools/qso_log: the ADIF reader of the import, not run here */
#pragma once
#include "qso_log.h"
int adif_read(const char *path, qso_log_record_t **records, int *skipped);
