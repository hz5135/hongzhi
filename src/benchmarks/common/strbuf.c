#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"   /* xrealloc */

void sb_reserve(sb_t *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 64;
        while (ncap < b->len + extra + 1) ncap *= 2;
        b->s = (char *)xrealloc(b->s, ncap);
        b->cap = ncap;
    }
}

void sb_init(sb_t *b) {
    b->s = NULL;
    b->len = 0;
    b->cap = 0;
    sb_reserve(b, 0);
    b->s[0] = '\0';
}

/* sb_put/sb_puts/sb_putc are inline in strbuf.h - see the note there. */

void sb_printf(sb_t *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need > 0) {
        sb_reserve(b, (size_t)need);
        vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
        b->len += (size_t)need;
    }
    va_end(ap);
}

void sb_puts_int(sb_t *b, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    sb_puts(b, tmp);
}

void sb_reset(sb_t *b) {
    b->len = 0;
    if (b->s) b->s[0] = '\0';
}

char *sb_take(sb_t *b) {
    if (!b->s) {
        b->s = strdup("");
        b->len = 0;
        b->cap = 1;
    }
    return b->s;
}

void sb_free(sb_t *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}
