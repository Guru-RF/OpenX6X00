/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2024 Georgy Dyuldin aka R2RFE
 */
#pragma once

#include "qso_log.h"
#include "field/flog.h"

#include <time.h>

typedef struct adif_log_s *adif_log;

adif_log  adif_log_init(const char * path);

void adif_log_close(adif_log l);

/* The QSO into the FT8 app's log (ft_log.adi), as adif_field_qso() and field_adif_record() write it */
void adif_add_qso(adif_log l, qso_log_record_t qso);

/* An app's QSO (FT8, FT4, FT2, JS8) as the radio's ADIF logs write it: ft_log.adi and the day's log both take it
 * from here, so the same QSO is the same record in both (#174) */
field_qso_t adif_field_qso(const qso_log_record_t *r);

int adif_read(const char * path, qso_log_record_t ** records, int *skipped);
