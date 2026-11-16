/* What src/dxcc.c says of each call on stdin, a line each: "CALL<tab>name<tab>primary prefix", "-" for both when
 * it does not know it. apps_web_test.py compares the JS8 and FT8 pages' own lookup (app_wall.js) with it */
#include "dxcc.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    char line[128];

    while (fgets(line, sizeof(line), stdin)) {
        const char *name, *prefix;

        line[strcspn(line, "\r\n")] = 0;
        if (dxcc_lookup(line, &name, &prefix))
            printf("%s\t%s\t%s\n", line, name, prefix);
        else
            printf("%s\t-\t-\n", line);
    }
    return 0;
}
