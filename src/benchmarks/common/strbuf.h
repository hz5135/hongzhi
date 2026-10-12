#ifndef BENCH_STRBUF_H
#define BENCH_STRBUF_H

/**
 * strbuf.h — a growable, NUL-terminated byte buffer.
 *
 * The kernels build JSON payloads, HTML, and generated source text piece by
 * piece, so they all keep the same small string builder: append, format, reset,
 * free.  The buffer owns its bytes, grows by doubling, and always ends in a NUL,
 * so it can be handed to strlen()/strcmp()/printf() directly.
 *
 * A zeroed buffer needs no init call:
 *
 *     sb_t b = {0};
 *     sb_puts(&b, "{\"id\": ");
 *     sb_puts_int(&b, id);
 *     sb_putc(&b, '}');
 *     ... use b.s, b.len ...
 *     sb_free(&b);
 *
 * A buffer allocates on its first write.  sb_init is only for the callers that
 * want the allocation up front and the buffer usable as an empty string before
 * anything is written to it.
 */

#include <stddef.h>
#include <string.h>   /* memcpy/strlen, used by the inline appenders below */

typedef struct {
    char *s;         /* the bytes, NUL-terminated once anything was written */
    size_t len;      /* bytes in use, not counting the NUL */
    size_t cap;      /* bytes allocated */
} sb_t;

/* Make room for `extra` more bytes plus the trailing NUL. */
void sb_reserve(sb_t *b, size_t extra);

/* An empty buffer that is already usable: allocated and NUL-terminated. */
void sb_init(sb_t *b);

/* The three appenders are inline here rather than in strbuf.c: the kernels
 * append once per fragment, and routing that through a call costs them 12-40 %
 * of their runtime (text_processing +65 % on aarch64, +115 % on x86_64).  What
 * stays in strbuf.c is the growth policy - sb_reserve - and everything that
 * walks the whole buffer. */

/* Append `n` bytes. */
static inline void sb_put(sb_t *b, const char *data, size_t n) {
    sb_reserve(b, n);
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = '\0';
}

/* Append a NUL-terminated string. */
static inline void sb_puts(sb_t *b, const char *str) {
    sb_put(b, str, strlen(str));
}

/* Append one byte. */
static inline void sb_putc(sb_t *b, char c) {
    sb_put(b, &c, 1);
}

/* Append a printf-formatted string. */
void sb_printf(sb_t *b, const char *fmt, ...);

/* Append an integer as decimal text. */
void sb_puts_int(sb_t *b, long long v);

/* Append one line, newline included (a list.append plus join in one call). */
static inline void sb_puts_line(sb_t *b, const char *line) {
    sb_puts(b, line);
    sb_putc(b, '\n');
}

/* Empty the buffer, keeping the allocation. */
void sb_reset(sb_t *b);

/* Hand the bytes over to the caller, who then owns them and must free them.
 * A buffer nothing was written to yields an empty string, never NULL. */
char *sb_take(sb_t *b);

/* Release the buffer. */
void sb_free(sb_t *b);

#endif /* BENCH_STRBUF_H */
