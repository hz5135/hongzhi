#ifndef BENCH_JSON_DOM_H
#define BENCH_JSON_DOM_H

/**
 * json_dom.h — a JSON value tree, its serializer, and its parser.
 *
 * Two kernels build JSON, walk it, and parse it back: json_processing
 * round-trips tool schemas, conversations and function calls; schema_validation
 * compiles a schema, generates instances from it, and repairs malformed JSON.
 * Both need the same value model, so it lives here.
 *
 * The model keeps what json.loads keeps: an integer literal stays an integer and
 * a real literal becomes a float, so a schema can be validated on that
 * distinction (an "integer" property must not accept 1.5).  Objects remember
 * insertion order, and setting an existing key keeps its first position, like a
 * Python dict.
 *
 * Serialization writes what json.dumps writes: an integer as itself, a float the
 * way repr() prints it (the shortest decimal that reads back as the same double,
 * with ".0" kept on an integral value), and strings escaped to the same bytes
 * (json_escape.h).  Indented output is json.dumps(indent=2).
 */

#include "strbuf.h"

typedef enum { JD_NULL, JD_BOOL, JD_INT, JD_FLOAT, JD_STR, JD_ARR, JD_OBJ } jd_type;

typedef struct jd jd_t;
typedef struct { char *key; jd_t *val; } jd_member;

struct jd {
    jd_type type;
    union {
        int b;
        long long i;
        double f;
        char *s;
        struct { jd_t **items; int count, cap; } arr;
        struct { jd_member *items; int count, cap; } obj;
    } u;
};

/* Constructors; each returns a value the caller owns. */
jd_t *json_null(void);
jd_t *json_bool(int b);
jd_t *json_int(long long i);
jd_t *json_float(double f);
jd_t *json_str(const char *s);
jd_t *json_strf(const char *fmt, ...);
jd_t *json_arr(void);
jd_t *json_obj(void);

/* Containers.  json_obj_set replaces an existing key in place, keeping its
 * position. */
void json_arr_push(jd_t *arr, jd_t *v);
void json_obj_set(jd_t *obj, const char *key, jd_t *v);
int json_obj_index(const jd_t *obj, const char *key);   /* -1 when absent */
int json_obj_has(const jd_t *obj, const char *key);
jd_t *json_obj_get(const jd_t *obj, const char *key);   /* NULL when absent */

/* Release a value and everything under it. */
void json_free(jd_t *v);

/* json.dumps(v): `compact` selects separators=(",", ":") over the default
 * ", " / ": ". */
void json_ser(const jd_t *v, sb_t *out, int compact);

/* json.dumps(v, indent=2), with the value nested `depth` levels deep. */
void json_ser_indent(const jd_t *v, sb_t *out, int depth);

/* json.loads(s): NULL when the text is not exactly one complete JSON value. */
jd_t *json_parse(const char *s);

#endif /* BENCH_JSON_DOM_H */
