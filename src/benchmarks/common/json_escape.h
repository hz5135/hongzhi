#ifndef BENCH_JSON_ESCAPE_H
#define BENCH_JSON_ESCAPE_H

/**
 * json_escape.h — append a string as a JSON string literal.
 *
 * Three kernels build JSON with strings in it: tool results, SSE event bodies,
 * conversation payloads.  The escaping is what json.dumps writes — the
 * two-character escapes for what JSON must escape, and \u00xx for the rest of
 * the control range — so the bytes match the reference implementation's.
 */

#include "strbuf.h"

/* Append `s` as a quoted JSON string (the quotes are part of the output). */
void json_escape_string(sb_t *b, const char *s);

#endif /* BENCH_JSON_ESCAPE_H */
