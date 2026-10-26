#include "utils.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/timex.h>

#define SYNC_MAX_ERR_US 1000000     /* ntpd's maximum error, under which the clock is right for FT8 */

/**
 * Check that text is CQ modifier (3 digits or 1 to 4 letters)
 */
bool is_cq_modifier(const char *text) {
    size_t len = strlen(text);
    char   c;

    // Check for 3 digits
    bool correct = true;
    if (len == 3) {
        // Check for 3 digits
        for (size_t i = 0; i < len; i++) {
            if ((text[i] < '0') || (text[i] > '9')) {
                correct = false;
                break;
            }
        }
        if (correct) {
            return true;
        }
    }
    // Check for 1 to 4 letters
    correct = true;
    if ((len >= 1) && (len <= 4)) {
        for (size_t i = 0; i < len; i++) {
            c = toupper(text[i]);
            if ((c < 'A') || (c > 'Z')) {
                correct = false;
                break;
            }
        }
        if (correct) {
            return true;
        }
    }
    return false;
}

int64_t ftx_slot_offset_ns(int64_t sec, long nsec, float slot) {
    int64_t slot_ns = llround(slot * 1e9);
    int64_t t       = (sec % 60 + 60) % 60 * 1000000000LL + nsec;

    return (t + slot_ns / 2) % slot_ns - slot_ns / 2;
}

bool ftx_clock_synced(int *err_ms) {
    struct timex tx    = {0};   /* modes 0: only read */
    int          state = ntp_adjtime(&tx);

    if (state < 0 || state == TIME_ERROR || (tx.status & STA_UNSYNC) || tx.maxerror > SYNC_MAX_ERR_US) {
        return false;
    }
    if (err_ms) *err_ms = (int)(tx.maxerror / 1000);
    return true;
}
