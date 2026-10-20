#include "utils.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>

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
