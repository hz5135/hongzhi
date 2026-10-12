/**
 * json_processing.c — JSON serialization / deserialization benchmark.
 *
 * Mirrors the reference json_processing benchmark: AI agents communicate
 * almost entirely via JSON (tool schemas, function-call arguments, structured
 * responses, conversation histories). Four phases exercise a full
 * recursive-descent JSON parser/serializer in C on structures that mirror
 * real agent traffic:
 *
 *   Phase 1: generate + serialize 200 realistic tool schemas
 *   Phase 2: deserialize all schemas back
 *   Phase 3: generate, serialize, and deserialize 100 multi-turn agent
 *            conversations, then re-serialize every tool_use block's arguments
 *   Phase 4: 500 function call -> result round trips
 *
 * Workload constants (total sets + per-set sizes) are defined in the Lua
 * script and passed in as parameters. A fixed total workload runs per event,
 * split across `threads`: more threads finish the same work faster.
 */

#include "core.h"
#include "workers.h"
#include "strbuf.h"
#include "json_escape.h"
#include "json_dom.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The kernel's numbers are ids, counts and indices, so they are all integral;
 * a value parsed back from JSON keeps the integer/float split json.loads gives
 * it. */
static jd_t *json_num(double d) {
    return d == (double)(long long)d ? json_int((long long)d) : json_float(d);
}

/* Workload constants are defined in lua/benchmarks/json_processing.lua and
 * passed in as parameters (edit the Lua script to tune the workload, no
 * recompile needed). */


/* ------------------------------------------------------------------ */
/*  Deterministic workload generators (mirror the reference)           */
/* ------------------------------------------------------------------ */

static uint32_t prng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static jd_t *gen_tool_schema(uint32_t seed) {
    uint32_t rng = seed * 2654435761u + 12345u;
    int param_count = 2 + (int)(prng_next(&rng) % 11);  /* 2..12 */
    static const char *types[] = { "string", "integer", "boolean", "array", "object" };
    jd_t *params = json_obj();
    for (int j = 0; j < param_count; j++) {
        const char *ptype = types[prng_next(&rng) % 5];
        jd_t *param = json_obj();
        json_obj_set(param, "type", json_str(ptype));
        char desc[64];
        snprintf(desc, sizeof desc, "Parameter %d for tool %u", j, seed);
        json_obj_set(param, "description", json_str(desc));
        if (strcmp(ptype, "array") == 0) {
            jd_t *items = json_obj();
            json_obj_set(items, "type", json_str("string"));
            json_obj_set(param, "items", items);
        } else if (strcmp(ptype, "object") == 0) {
            jd_t *props = json_obj();
            int nsub = 1 + (int)(prng_next(&rng) % 4);  /* 1..4 */
            for (int k = 0; k < nsub; k++) {
                char skey[16];
                snprintf(skey, sizeof skey, "sub_%d", k);
                jd_t *sv = json_obj();
                json_obj_set(sv, "type", json_str("string"));
                json_obj_set(props, skey, sv);
            }
            json_obj_set(param, "properties", props);
        }
        char pkey[16];
        snprintf(pkey, sizeof pkey, "param_%d", j);
        json_obj_set(params, pkey, param);
    }
    jd_t *schema = json_obj();
    char name[32];
    snprintf(name, sizeof name, "tool_%u", seed);
    json_obj_set(schema, "name", json_str(name));
    char desc2[80];
    snprintf(desc2, sizeof desc2, "A tool that performs operation %u with various parameters", seed);
    json_obj_set(schema, "description", json_str(desc2));
    jd_t *parameters = json_obj();
    json_obj_set(parameters, "type", json_str("object"));
    json_obj_set(parameters, "properties", params);
    jd_t *required = json_arr();
    int nreq = param_count < 3 ? param_count : 3;
    for (int k = 0; k < nreq; k++) {
        char rk[16];
        snprintf(rk, sizeof rk, "param_%d", k);
        json_arr_push(required, json_str(rk));
    }
    json_obj_set(parameters, "required", required);
    json_obj_set(schema, "parameters", parameters);
    return schema;
}

static jd_t *gen_conversation(uint32_t seed, int messages_per_conversation) {
    uint32_t rng = seed * 2654435761u + 987654321u;
    jd_t *conv = json_arr();
    for (int i = 0; i < messages_per_conversation; i++) {
        int role = i % 3;
        jd_t *msg = json_obj();
        json_obj_set(msg, "role",
                     json_str(role == 0 ? "user" : role == 1 ? "assistant" : "tool"));
        if (role == 2) {
            /* tool: content is a JSON-encoded result string */
            sb_t jb;
            sb_init(&jb);
            sb_puts(&jb, "{\"result\":{\"data\":[");
            for (int d = 0; d < 20; d++) {
                if (d) sb_puts(&jb, ",");
                sb_printf(&jb, "%.6f", (double)(prng_next(&rng) % 1000000) / 1000000.0);
            }
            sb_puts(&jb, "]},\"metadata\":{\"latency_ms\":");
            sb_printf(&jb, "%u}}", 10u + (prng_next(&rng) % 491));
            json_obj_set(msg, "content", json_str(jb.s));
            sb_free(&jb);
        } else if (role == 1) {
            /* assistant: content is a list of text + tool_use blocks */
            jd_t *content = json_arr();
            jd_t *text_block = json_obj();
            json_obj_set(text_block, "type", json_str("text"));
            char txt[64];
            snprintf(txt, sizeof txt, "Step %d: analyzing results...", i);
            json_obj_set(text_block, "text", json_str(txt));
            json_arr_push(content, text_block);
            jd_t *tool_use = json_obj();
            json_obj_set(tool_use, "type", json_str("tool_use"));
            char tname[16];
            snprintf(tname, sizeof tname, "tool_%u", prng_next(&rng) % 51);
            json_obj_set(tool_use, "name", json_str(tname));
            jd_t *args = json_obj();
            int nargs = 1 + (int)(prng_next(&rng) % 5);  /* 1..5 */
            for (int k = 0; k < nargs; k++) {
                char akey[16];
                snprintf(akey, sizeof akey, "param_%d", k);
                char aval[16];
                snprintf(aval, sizeof aval, "value_%u", prng_next(&rng) % 1001);
                json_obj_set(args, akey, json_str(aval));
            }
            json_obj_set(tool_use, "arguments", args);
            json_arr_push(content, tool_use);
            json_obj_set(msg, "content", content);
        } else {
            char content[80];
            snprintf(content, sizeof content, "User message %d with context about task %u",
                     i, seed);
            json_obj_set(msg, "content", json_str(content));
        }
        json_arr_push(conv, msg);
    }
    return conv;
}

/* ------------------------------------------------------------------ */
/*  Parallel worker                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int sets;                    /* this thread's share of total_sets */
    int num_schemas;
    int num_conversations;
    int messages_per_conversation;
    int num_round_trips;
} json_work_t;

static void *json_worker(void *arg) {
    json_work_t *w = (json_work_t *)arg;
    long long total_ops = 0;
    sb_t jb;
    sb_init(&jb);
    for (int it = 0; it < w->sets; it++) {
        /* Phase 1 + 2: generate + serialize, then deserialize tool schemas */
        for (int i = 0; i < w->num_schemas; i++) {
            jd_t *schema = gen_tool_schema((uint32_t)i);
            sb_reset(&jb);
            json_ser(schema, &jb, 1);  /* compact separators, as the reference */
            total_ops++;
            json_free(schema);
            jd_t *back = json_parse(jb.s);
            if (back) {
                total_ops++;
                json_free(back);
            }
        }
        /* Phase 3: conversations (serialize + deserialize + tool-call extraction) */
        for (int i = 0; i < w->num_conversations; i++) {
            jd_t *conv = gen_conversation((uint32_t)i, w->messages_per_conversation);
            sb_reset(&jb);
            json_ser(conv, &jb, 0);
            total_ops += 2;  /* serialize + deserialize */
            json_free(conv);
            jd_t *decoded = json_parse(jb.s);
            if (decoded) {
                for (int m = 0; m < decoded->u.arr.count; m++) {
                    jd_t *msg = decoded->u.arr.items[m];
                    jd_t *content = json_obj_get(msg, "content");
                    if (content && content->type == JD_ARR) {
                        for (int b = 0; b < content->u.arr.count; b++) {
                            jd_t *block = content->u.arr.items[b];
                            jd_t *bt = json_obj_get(block, "type");
                            if (bt && bt->type == JD_STR && strcmp(bt->u.s, "tool_use") == 0) {
                                jd_t *args = json_obj_get(block, "arguments");
                                if (args) {
                                    sb_reset(&jb);
                                    json_ser(args, &jb, 0);
                                    total_ops++;
                                }
                            }
                        }
                    }
                }
                json_free(decoded);
            }
        }
        /* Phase 4: function call -> result round trips */
        for (int i = 0; i < w->num_round_trips; i++) {
            jd_t *call = json_obj();
            char id[32];
            snprintf(id, sizeof id, "call_%d", i);
            json_obj_set(call, "id", json_str(id));
            char fn[16];
            snprintf(fn, sizeof fn, "tool_%d", i % 50);
            json_obj_set(call, "function", json_str(fn));
            jd_t *arguments = json_obj();
            char q[64];
            snprintf(q, sizeof q, "search term %d", i);
            json_obj_set(arguments, "query", json_str(q));
            json_obj_set(arguments, "limit", json_num((double)(i % 20 + 1)));
            json_obj_set(call, "arguments", arguments);
            sb_reset(&jb);
            json_ser(call, &jb, 0);
            total_ops++;  /* serialize call */
            json_free(call);
            jd_t *back = json_parse(jb.s);
            if (back) {
                total_ops++;  /* deserialize call */
                jd_t *result = json_obj();
                jd_t *bid = json_obj_get(back, "id");
                if (bid && bid->type == JD_STR) json_obj_set(result, "id", json_str(bid->u.s));
                jd_t *output = json_obj();
                jd_t *matches = json_arr();
                jd_t *back_args = json_obj_get(back, "arguments");
                jd_t *lim = back_args ? json_obj_get(back_args, "limit") : NULL;
                int n = (lim && lim->type == JD_INT) ? (int)lim->u.i : 0;
                for (int m = 0; m < n; m++) json_arr_push(matches, json_num((double)m));
                json_obj_set(output, "matches", matches);
                json_obj_set(result, "output", output);
                sb_reset(&jb);
                json_ser(result, &jb, 0);
                total_ops++;  /* serialize result */
                json_free(result);
                jd_t *d2 = json_parse(jb.s);
                if (d2) {
                    total_ops++;  /* deserialize result */
                    json_free(d2);
                }
                json_free(back);
            }
        }
    }
    sb_free(&jb);
    return work_ops(total_ops);
}

long long run_json_processing(int total_sets, int num_schemas, int num_conversations,
                        int messages_per_conversation, int num_round_trips,
                        int threads) {
    if (total_sets <= 0) total_sets = 256;
    if (num_schemas <= 0) num_schemas = 200;
    if (num_conversations <= 0) num_conversations = 100;
    if (messages_per_conversation <= 0) messages_per_conversation = 50;
    if (num_round_trips <= 0) num_round_trips = 500;
    threads = work_threads(total_sets, threads);

    json_work_t *work = (json_work_t *)calloc((size_t)threads, sizeof *work);
    if (!work) {
        fprintf(stderr, "error: json_processing: out of memory for worker arrays\n");
        return 0;
    }
    for (int i = 0; i < threads; i++) {
        work[i].sets = work_slice(total_sets, threads, i);
        work[i].num_schemas = num_schemas;
        work[i].num_conversations = num_conversations;
        work[i].messages_per_conversation = messages_per_conversation;
        work[i].num_round_trips = num_round_trips;
    }

    long long total = work_run(work, sizeof *work, threads, json_worker, "json_processing");
    free(work);
    return total;
}
