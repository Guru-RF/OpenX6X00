/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  CW trainer: how it scores (see cw_trainer_score.h)
 *
 *  Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
 */

#include "cw_trainer_score.h"

#include <ctype.h>
#include <string.h>

#define LEN_MAX 32      /* places checked: a group is 5 at most, an answer as long */

char cwt_copy_char(uint32_t key, const char *set) {
    char c;

    if (key <= ' ' || key >= 127) {
        return 0;
    }
    c = (char)toupper((int)key);
    return strchr(set, c) ? c : 0;
}

bool cwt_copy_add(char *answer, int max, char c) {
    size_t n = strlen(answer);

    if ((int)n >= max) {
        return false;
    }
    answer[n]     = c;
    answer[n + 1] = 0;
    return true;
}

bool cwt_copy_del(char *answer) {
    size_t n = strlen(answer);

    if (!n) {
        return false;
    }
    answer[n - 1] = 0;
    return true;
}

/* The fewest changes that make the answer of the group (a place wrong, missed, or typed one too many, each one; a
 * place right none), from the front: place against place where that costs no more, else the group's place missed,
 * else the answer's one too many. Place by place first: IRK typed KSS had all three wrong, not the K typed first
 * taken for the group's last one with I and R missed and two S too many */
int cwt_copy_check(const char *sent, const char *typed, bool *sent_ok, bool *typed_ok) {
    size_t  sn = strlen(sent), tn = strlen(typed);
    uint8_t cost[LEN_MAX + 1][LEN_MAX + 1];     /* of sent from i and typed from j on */
    int     copied = 0;

    for (size_t i = 0; sent_ok && i < sn; i++) {
        sent_ok[i] = false;
    }
    for (size_t j = 0; typed_ok && j < tn; j++) {
        typed_ok[j] = false;
    }
    if (sn > LEN_MAX) {
        sn = LEN_MAX;
    }
    if (tn > LEN_MAX) {
        tn = LEN_MAX;
    }
    for (size_t i = sn + 1; i-- > 0;) {
        for (size_t j = tn + 1; j-- > 0;) {
            if (i == sn) {
                cost[i][j] = tn - j;
            } else if (j == tn) {
                cost[i][j] = sn - i;
            } else {
                int place  = cost[i + 1][j + 1] + (sent[i] != typed[j]);
                int missed = cost[i + 1][j] + 1;
                int extra  = cost[i][j + 1] + 1;
                int c      = place < missed ? place : missed;

                cost[i][j] = c < extra ? c : extra;
            }
        }
    }
    for (size_t i = 0, j = 0; i < sn || j < tn;) {
        if (i < sn && j < tn && cost[i][j] == cost[i + 1][j + 1] + (sent[i] != typed[j])) {
            if (sent[i] == typed[j]) {
                if (sent_ok) {
                    sent_ok[i] = true;
                }
                if (typed_ok) {
                    typed_ok[j] = true;
                }
                copied++;
            }
            i++;
            j++;
        } else if (i < sn && cost[i][j] == cost[i + 1][j] + 1) {
            i++;
        } else {
            j++;
        }
    }
    return copied;
}

int cwt_copy_sel(int sel, int n, int d) {
    if (n <= 0) {
        return 0;
    }
    return ((sel + d) % n + n) % n;
}

void cwt_stats_add(cwt_stats_t *s, int i, bool ok) {
    s->mask[i] = ((s->mask[i] << 1) | (ok ? 1 : 0)) & ((1u << CWT_HIST) - 1);
    if (s->n[i] < CWT_HIST) {
        s->n[i]++;
    }
}

int cwt_stats_pct(const cwt_stats_t *s, int i) {
    int n = s->n[i];

    if (!n) {
        return -1;
    }
    return __builtin_popcount(s->mask[i] & ((1u << n) - 1)) * 100 / n;
}

void cwt_stats_group(cwt_stats_t *s, const char *set, const char *sent, const bool *sent_ok, bool right) {
    s->tries++;
    s->right += right;
    for (size_t i = 0; sent[i]; i++) {
        const char *k = strchr(set, sent[i]);

        if (k && (k - set) < CWT_CHARS) {
            cwt_stats_add(s, (int)(k - set), sent_ok[i]);
        }
    }
}

void cwt_session_add(cwt_session_t *s, bool ok) {
    s->tried++;
    s->right += ok;
    s->hist[s->pos] = ok;
    s->pos          = (s->pos + 1) % CWT_WINDOW;
    if (s->n < CWT_WINDOW) {
        s->n++;
    }
}

int cwt_session_good(const cwt_session_t *s) {
    int good = 0;

    for (int i = 0; i < s->n; i++) {
        good += s->hist[i];
    }
    return good;
}

bool cwt_session_pass(const cwt_session_t *s) {
    return s->n == CWT_WINDOW && cwt_session_good(s) * 100 >= CWT_PASS * CWT_WINDOW;
}

void cwt_session_level(cwt_session_t *s) {
    s->n = s->pos = 0;
}
