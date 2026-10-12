#include "json_dom.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"          /* xrealloc */
#include "json_escape.h"

/* ---------------------------------------------------------------- values -- */

static jd_t *jd_new(jd_type t) {
    jd_t *v = (jd_t *)calloc(1, sizeof(jd_t));
    v->type = t;
    return v;
}

jd_t *json_null(void) { return jd_new(JD_NULL); }
jd_t *json_bool(int b) { jd_t *v = jd_new(JD_BOOL); v->u.b = b; return v; }
jd_t *json_int(long long i) { jd_t *v = jd_new(JD_INT); v->u.i = i; return v; }
jd_t *json_float(double f) { jd_t *v = jd_new(JD_FLOAT); v->u.f = f; return v; }

jd_t *json_str(const char *s) {
    jd_t *v = jd_new(JD_STR);
    v->u.s = s ? strdup(s) : NULL;
    return v;
}

jd_t *json_strf(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return json_str(buf);
}

jd_t *json_arr(void) { return jd_new(JD_ARR); }
jd_t *json_obj(void) { return jd_new(JD_OBJ); }

void json_arr_push(jd_t *arr, jd_t *v) {
    if (arr->u.arr.count >= arr->u.arr.cap) {
        arr->u.arr.cap = arr->u.arr.cap ? arr->u.arr.cap * 2 : 8;
        arr->u.arr.items = (jd_t **)xrealloc(arr->u.arr.items,
                                             (size_t)arr->u.arr.cap * sizeof(jd_t *));
    }
    arr->u.arr.items[arr->u.arr.count++] = v;
}

int json_obj_index(const jd_t *obj, const char *key) {
    for (int i = 0; i < obj->u.obj.count; i++)
        if (strcmp(obj->u.obj.items[i].key, key) == 0) return i;
    return -1;
}

void json_obj_set(jd_t *obj, const char *key, jd_t *v) {
    int idx = json_obj_index(obj, key);
    if (idx >= 0) {
        json_free(obj->u.obj.items[idx].val);
        obj->u.obj.items[idx].val = v;
        return;
    }
    if (obj->u.obj.count >= obj->u.obj.cap) {
        obj->u.obj.cap = obj->u.obj.cap ? obj->u.obj.cap * 2 : 8;
        obj->u.obj.items = (jd_member *)xrealloc(obj->u.obj.items,
                                                 (size_t)obj->u.obj.cap * sizeof(jd_member));
    }
    obj->u.obj.items[obj->u.obj.count].key = strdup(key);
    obj->u.obj.items[obj->u.obj.count].val = v;
    obj->u.obj.count++;
}

int json_obj_has(const jd_t *obj, const char *key) {
    return json_obj_index(obj, key) >= 0;
}

jd_t *json_obj_get(const jd_t *obj, const char *key) {
    int idx = json_obj_index(obj, key);
    return idx >= 0 ? obj->u.obj.items[idx].val : NULL;
}

void json_free(jd_t *v) {
    if (!v) return;
    switch (v->type) {
    case JD_STR:
        free(v->u.s);
        break;
    case JD_ARR:
        for (int i = 0; i < v->u.arr.count; i++) json_free(v->u.arr.items[i]);
        free(v->u.arr.items);
        break;
    case JD_OBJ:
        for (int i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.items[i].key);
            json_free(v->u.obj.items[i].val);
        }
        free(v->u.obj.items);
        break;
    default:
        break;
    }
    free(v);
}

/* -------------------------------------------------------------- serialize -- */

/* repr() of a float: the shortest decimal that strtod() reads back as the same
 * double, with a decimal point or an exponent always present (so an integral
 * value keeps its ".0"), which is what json.dumps writes for a float. */
static void float_text(double v, char *buf, size_t n) {
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, n, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    if (!strpbrk(buf, ".eE")) {
        size_t len = strlen(buf);
        if (len + 2 < n) {
            buf[len] = '.';
            buf[len + 1] = '0';
            buf[len + 2] = '\0';
        }
    }
}

void json_ser(const jd_t *v, sb_t *out, int compact) {
    const char *comma = compact ? "," : ", ";
    const char *colon = compact ? ":" : ": ";
    switch (v->type) {
    case JD_NULL:
        sb_puts(out, "null");
        break;
    case JD_BOOL:
        sb_puts(out, v->u.b ? "true" : "false");
        break;
    case JD_INT:
        sb_puts_int(out, v->u.i);
        break;
    case JD_FLOAT: {
        char fb[64];
        float_text(v->u.f, fb, sizeof fb);
        sb_puts(out, fb);
        break;
    }
    case JD_STR:
        json_escape_string(out, v->u.s);
        break;
    case JD_ARR:
        sb_putc(out, '[');
        for (int i = 0; i < v->u.arr.count; i++) {
            if (i) sb_puts(out, comma);
            json_ser(v->u.arr.items[i], out, compact);
        }
        sb_putc(out, ']');
        break;
    case JD_OBJ:
        sb_putc(out, '{');
        for (int i = 0; i < v->u.obj.count; i++) {
            if (i) sb_puts(out, comma);
            json_escape_string(out, v->u.obj.items[i].key);
            sb_puts(out, colon);
            json_ser(v->u.obj.items[i].val, out, compact);
        }
        sb_putc(out, '}');
        break;
    }
}

void json_ser_indent(const jd_t *v, sb_t *out, int depth) {
    switch (v->type) {
    case JD_NULL:
        sb_puts(out, "null");
        break;
    case JD_BOOL:
        sb_puts(out, v->u.b ? "true" : "false");
        break;
    case JD_INT:
        sb_puts_int(out, v->u.i);
        break;
    case JD_FLOAT: {
        char fb[64];
        float_text(v->u.f, fb, sizeof fb);
        sb_puts(out, fb);
        break;
    }
    case JD_STR:
        json_escape_string(out, v->u.s);
        break;
    case JD_ARR:
        if (v->u.arr.count == 0) {
            sb_puts(out, "[]");
            break;
        }
        sb_puts(out, "[\n");
        for (int i = 0; i < v->u.arr.count; i++) {
            for (int d = 0; d < depth + 1; d++) sb_puts(out, "  ");
            json_ser_indent(v->u.arr.items[i], out, depth + 1);
            if (i + 1 < v->u.arr.count) sb_putc(out, ',');
            sb_putc(out, '\n');
        }
        for (int d = 0; d < depth; d++) sb_puts(out, "  ");
        sb_putc(out, ']');
        break;
    case JD_OBJ:
        if (v->u.obj.count == 0) {
            sb_puts(out, "{}");
            break;
        }
        sb_puts(out, "{\n");
        for (int i = 0; i < v->u.obj.count; i++) {
            for (int d = 0; d < depth + 1; d++) sb_puts(out, "  ");
            json_escape_string(out, v->u.obj.items[i].key);
            sb_puts(out, ": ");
            json_ser_indent(v->u.obj.items[i].val, out, depth + 1);
            if (i + 1 < v->u.obj.count) sb_putc(out, ',');
            sb_putc(out, '\n');
        }
        for (int d = 0; d < depth; d++) sb_puts(out, "  ");
        sb_putc(out, '}');
        break;
    }
}

/* ----------------------------------------------------------------- parse -- */

typedef struct { const char *p; } jp_t;

static void jp_ws(jp_t *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_utf8(sb_t *b, unsigned cp) {
    if (cp < 0x80) {
        sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(b, (char)(0xC0 | (cp >> 6)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(b, (char)(0xE0 | (cp >> 12)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (char)(0xF0 | (cp >> 18)));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* The string body is decoded into a fresh buffer the caller owns. */
static char *jp_string(jp_t *j) {
    j->p++;   /* opening quote */
    sb_t sb = {0};
    for (;;) {
        char c = *j->p;
        if (!c) {
            sb_free(&sb);
            return NULL;
        }
        if (c == '"') {
            j->p++;
            return sb.s ? sb.s : strdup("");
        }
        if (c == '\\') {
            j->p++;
            char e = *j->p;
            j->p++;
            switch (e) {
            case '"':  sb_putc(&sb, '"'); break;
            case '\\': sb_putc(&sb, '\\'); break;
            case '/':  sb_putc(&sb, '/'); break;
            case 'b':  sb_putc(&sb, '\b'); break;
            case 'f':  sb_putc(&sb, '\f'); break;
            case 'n':  sb_putc(&sb, '\n'); break;
            case 'r':  sb_putc(&sb, '\r'); break;
            case 't':  sb_putc(&sb, '\t'); break;
            case 'u': {
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    int d = hexval(*j->p);
                    if (d < 0) {
                        sb_free(&sb);
                        return NULL;
                    }
                    cp = (cp << 4) | (unsigned)d;
                    j->p++;
                }
                /* merge a UTF-16 surrogate pair */
                if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    const char *q = j->p + 2;
                    for (int k = 0; k < 4; k++) {
                        int d = hexval(*q);
                        if (d < 0) {
                            ok = 0;
                            break;
                        }
                        lo = (lo << 4) | (unsigned)d;
                        q++;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p = q;
                    }
                }
                put_utf8(&sb, cp);
                break;
            }
            default:
                sb_free(&sb);
                return NULL;
            }
        } else {
            sb_putc(&sb, c);
            j->p++;
        }
    }
}

static jd_t *jp_value(jp_t *j);

static jd_t *jp_object(jp_t *j) {
    j->p++;   /* { */
    jd_t *o = json_obj();
    jp_ws(j);
    if (*j->p == '}') {
        j->p++;
        return o;
    }
    for (;;) {
        jp_ws(j);
        if (*j->p != '"') break;
        char *key = jp_string(j);
        if (!key) break;
        jp_ws(j);
        if (*j->p != ':') {
            free(key);
            break;
        }
        j->p++;
        jd_t *v = jp_value(j);
        if (!v) {
            free(key);
            break;
        }
        json_obj_set(o, key, v);
        free(key);
        jp_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == '}') {
            j->p++;
            return o;
        }
        break;
    }
    json_free(o);
    return NULL;
}

static jd_t *jp_array(jp_t *j) {
    j->p++;   /* [ */
    jd_t *a = json_arr();
    jp_ws(j);
    if (*j->p == ']') {
        j->p++;
        return a;
    }
    for (;;) {
        jp_ws(j);
        jd_t *v = jp_value(j);
        if (!v) break;
        json_arr_push(a, v);
        jp_ws(j);
        if (*j->p == ',') {
            j->p++;
            continue;
        }
        if (*j->p == ']') {
            j->p++;
            return a;
        }
        break;
    }
    json_free(a);
    return NULL;
}

/* An integer literal stays an integer; anything with a "." or an exponent is a
 * float, the way json.loads keeps them apart. */
static jd_t *jp_number(jp_t *j) {
    char *end = NULL;
    double d = strtod(j->p, &end);
    if (end == j->p) return NULL;
    int is_int = 1;
    for (const char *q = j->p; q < end; q++)
        if (*q == '.' || *q == 'e' || *q == 'E') {
            is_int = 0;
            break;
        }
    j->p = end;
    return is_int ? json_int((long long)d) : json_float(d);
}

static jd_t *jp_value(jp_t *j) {
    jp_ws(j);
    char c = *j->p;
    if (c == '{') return jp_object(j);
    if (c == '[') return jp_array(j);
    if (c == '"') {
        char *s = jp_string(j);
        if (!s) return NULL;
        jd_t *v = json_str(s);
        free(s);   /* jp_string owns its buffer; json_str re-copies it */
        return v;
    }
    if (strncmp(j->p, "true", 4) == 0) {
        j->p += 4;
        return json_bool(1);
    }
    if (strncmp(j->p, "false", 5) == 0) {
        j->p += 5;
        return json_bool(0);
    }
    if (strncmp(j->p, "null", 4) == 0) {
        j->p += 4;
        return json_null();
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jp_number(j);
    return NULL;
}

jd_t *json_parse(const char *s) {
    jp_t j = { s };
    jp_ws(&j);
    jd_t *v = jp_value(&j);
    if (!v) return NULL;
    jp_ws(&j);
    if (*j.p) {
        json_free(v);
        return NULL;
    }
    return v;
}
