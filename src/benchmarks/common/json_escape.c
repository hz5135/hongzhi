#include "json_escape.h"

#include <stdio.h>

void json_escape_string(sb_t *b, const char *s) {
    sb_putc(b, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n");  break;
        case '\r': sb_puts(b, "\\r");  break;
        case '\t': sb_puts(b, "\\t");  break;
        case '\b': sb_puts(b, "\\b");  break;
        case '\f': sb_puts(b, "\\f");  break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                sb_puts(b, esc);
            } else {
                sb_putc(b, (char)c);
            }
            break;
        }
    }
    sb_putc(b, '"');
}
