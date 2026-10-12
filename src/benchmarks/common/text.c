#include "text.h"

/* The three character classes are inline in text.h: the scanners call them once
   per character, so a call out of this file would sit on their hot path. */

int text_count_lines(const char *s, size_t n) {
    int c = 1;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n') c++;
    return c;
}

int text_split_lines(const char *s, size_t n, line_span_t *out, int max) {
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n') {
            if (count < max) {
                out[count].off = start;
                out[count].len = i - start;
            }
            count++;
            start = i + 1;
        }
    }
    return count;
}
