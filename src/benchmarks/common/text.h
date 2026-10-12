#ifndef BENCH_TEXT_H
#define BENCH_TEXT_H

/**
 * text.h — the character classes and line helpers the text kernels share.
 *
 * The predicates are written out rather than taken from <ctype.h>, so a kernel
 * never depends on the locale.
 *
 * `text_is_space` is the whitespace JSON recognises (space, tab, newline,
 * carriage return).  HTML additionally treats the form feed as whitespace -
 * that predicate is not the same thing and stays with the HTML kernel rather
 * than being folded in here.
 *
 * The line helpers describe the same buffer the same way the kernels did: a
 * line is the bytes between newlines, and a buffer whose last character is not a
 * newline still ends in a line.
 */

#include <stddef.h>

/* The character classes are three-comparison predicates and the scanners call
 * them once per character (text_processing makes ~10^8 calls per event), so
 * their definitions live here and inline into the scanner loops: routing them
 * through a call per character costs that kernel +65 % on aarch64 and +115 % on
 * x86_64, which is more than the separation is worth on the hot path.  The
 * functions that walk a whole buffer (below) stay in text.c. */
static inline int text_is_digit(int c) {
    return c >= '0' && c <= '9';
}

static inline int text_is_word_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           text_is_digit(c) || c == '_';
}

/* JSON whitespace: space, tab, newline, carriage return. */
static inline int text_is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* One line's byte range inside the buffer it was split from. */
typedef struct {
    size_t off;
    size_t len;
} line_span_t;

/* How many lines `n` bytes hold: a final line without a newline counts, so an
 * empty buffer is one empty line and "a\n" is two. */
int text_count_lines(const char *s, size_t n);

/* Fill up to `max` line spans, in order, and return the number of lines the
 * buffer holds (which can be larger than `max`). */
int text_split_lines(const char *s, size_t n, line_span_t *out, int max);

#endif /* BENCH_TEXT_H */
