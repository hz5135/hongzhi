/**
 * text_processing.c — Agent text-processing benchmark.
 *
 * Mirrors the reference text_processing benchmark: agents process large
 * volumes of text (splitting documents into chunks, regex extraction,
 * searching and replacing, tokenizer-like byte-pair operations, log parsing).
 * Five phases run over a generated pseudo-realistic corpus:
 *
 *   Phase 1: overlapping chunk splitting (RAG-style) + content hashing
 *   Phase 2: 20 regex-like pattern extractions across the corpus
 *   Phase 3: byte-pair frequency counting
 *   Phase 4: top-k frequent pair merging (simplified BPE step)
 *   Phase 5: line-by-line processing (log parsing pattern)
 *
 * Workload constants (total sets + corpus size) are defined in the Lua script
 * and passed in as parameters. A fixed total workload runs per event over a
 * single shared read-only corpus, split across `threads`: more threads finish
 * the same work faster.
 */

#include "core.h"
#include "workers.h"
#include "text.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Workload constants (total sets + corpus size) are defined in
 * lua/benchmarks/text_processing.lua and passed in as parameters (edit the
 * Lua script to tune the workload, no recompile needed). */

/* ------------------------------------------------------------------ */
/*  Corpus generation (byte-identical to the reference)                */
/* ------------------------------------------------------------------ */

static const char *WORDS[] = {
    "the", "agent", "function", "tool", "result", "query", "search",
    "context", "window", "token", "embedding", "model", "inference",
    "parameter", "schema", "response", "request", "error", "success",
    "data", "pipeline", "batch", "stream", "async", "await", "process",
    "memory", "cache", "index", "vector", "dimension", "layer", "weight",
    "gradient", "optimize", "transform", "encode", "decode", "parse",
    "serialize", "validate", "dispatch", "execute", "monitor", "trace",
};
#define NUM_WORDS (int)(sizeof(WORDS) / sizeof(WORDS[0]))

/* Build `size` chars of pseudo-realistic text, mirroring the reference
 * `_build_corpus`: words cycled with ".\n" every 15th and "," every 7th,
 * joined by single spaces and truncated to `size` characters. */
static char *build_corpus(int size, size_t *out_len) {
    char *buf = (char *)malloc((size_t)size + 8);
    if (!buf) return NULL;
    size_t pos = 0;
    size_t total = 0;
    int i = 0;
    int first = 1;
    while (total < (size_t)size) {
        const char *word = WORDS[i % NUM_WORDS];
        int wlen = (int)strlen(word);
        int suffix = 0;
        if (i % 15 == 0) suffix = 2;       /* ".\n" */
        else if (i % 7 == 0) suffix = 1;   /* "," */
        int wfull = wlen + suffix;
        if (!first && pos < (size_t)size) buf[pos++] = ' ';
        for (int k = 0; k < wfull && pos < (size_t)size; k++) {
            char ch;
            if (k < wlen) ch = word[k];
            else if (suffix == 2) ch = (k == wlen) ? '.' : '\n';
            else ch = ',';
            buf[pos++] = ch;
        }
        total += (size_t)wfull + 1;
        i++;
        first = 0;
    }
    buf[pos] = '\0';
    if (out_len) *out_len = pos;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Character-class helpers                                            */
/* ------------------------------------------------------------------ */

/* Hex digit for the `[a-f0-9]` pattern: not the shared text_is_digit. */
static int is_hexl(char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); }

/* s[i] is the first character of a word (\b on the left). */
static int word_start(const char *s, size_t n, size_t i) {
    return i < n && text_is_word_char(s[i]) && (i == 0 || !text_is_word_char(s[i - 1]));
}
/* s[i] is the last character of a word (\b on the right). */
static int word_end(const char *s, size_t n, size_t i) {
    return i < n && text_is_word_char(s[i]) && (i + 1 >= n || !text_is_word_char(s[i + 1]));
}

/* ------------------------------------------------------------------ */
/*  Phase 1: overlapping chunk splitting + content hashing             */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static long long run_phase1(const char *corpus, size_t clen) {
    static const int CHUNK_SIZES[] = { 256, 512, 1024, 2048, 4096 };
    long long ops = 0;
    for (int ci = 0; ci < 5; ci++) {
        int chunk_size = CHUNK_SIZES[ci];
        int overlap = chunk_size / 4;
        size_t pos = 0;
        while (pos < clen) {
            size_t end = pos + (size_t)chunk_size;
            if (end > clen) end = clen;
            (void)fnv1a(corpus + pos, end - pos);
            pos += (size_t)(chunk_size - overlap);
            ops++;
        }
    }
    return ops;
}

/* ------------------------------------------------------------------ */
/*  Phase 2: regex-like pattern extraction                             */
/*  Each pattern mirrors one regex from the reference, counted by a     */
/*  manual scanner (no regex engine, as is the project convention).     */
/* ------------------------------------------------------------------ */

/* \b\w+_\w+\b — snake_case identifiers */
static long long pat_snake(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            int hit = 0;
            for (size_t k = i; k < j && !hit; k++)
                if (s[k] == '_' && k > i && k + 1 < j) hit = 1;
            if (hit) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* \b[A-Z][a-z]+[A-Z]\w*\b — camelCase identifiers */
static long long pat_camel(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i) && s[i] >= 'A' && s[i] <= 'Z') {
            size_t j = i + 1;
            while (j < n && text_is_word_char(s[j])) j++;
            int hit = 0;
            for (size_t k = i; k + 2 < j && !hit; k++) {
                if (s[k] >= 'A' && s[k] <= 'Z') {
                    size_t m = k + 1;
                    while (m < j && s[m] >= 'a' && s[m] <= 'z') m++;
                    if (m > k + 1 && m < j && s[m] >= 'A' && s[m] <= 'Z') hit = 1;
                }
            }
            if (hit) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* "[^"]{1,100}" — quoted strings */
static long long pat_quoted(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '"') {
            size_t j = i + 1;
            size_t cnt = 0;
            while (j < n && s[j] != '"' && cnt < 100) { j++; cnt++; }
            if (j < n && s[j] == '"' && cnt >= 1) c++;
            i = j + 1;
        } else i++;
    }
    return c;
}

/* \b\d{1,5}\b — numbers */
static long long pat_numbers(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (text_is_digit(s[i])) {
            size_t j = i;
            while (j < n && text_is_digit(s[j])) j++;
            if (j - i >= 1 && j - i <= 5 && word_start(s, n, i) && word_end(s, n, j - 1)) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* \b(?:error|Error|ERROR)\b — error mentions */
static long long pat_error(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if ((s[i] == 'e' || s[i] == 'E') && word_start(s, n, i)) {
            size_t wl = 0;
            if (strncmp(s + i, "error", 5) == 0) wl = 5;
            else if (strncmp(s + i, "Error", 5) == 0) wl = 5;
            else if (strncmp(s + i, "ERROR", 5) == 0) wl = 5;
            if (wl && word_end(s, n, i + wl - 1)) { c++; i += wl; continue; }
        }
        i++;
    }
    return c;
}

/* \b(?:function|tool|agent)\s+\w+ — function/tool declarations */
static long long pat_decl(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t wl = 0;
            if (strncmp(s + i, "function", 8) == 0) wl = 8;
            else if (strncmp(s + i, "tool", 4) == 0) wl = 4;
            else if (strncmp(s + i, "agent", 5) == 0) wl = 5;
            if (wl && word_end(s, n, i + wl - 1)) {
                size_t j = i + wl;
                while (j < n && text_is_space(s[j])) j++;
                if (j < n && text_is_word_char(s[j])) {
                    c++;
                    while (j < n && text_is_word_char(s[j])) j++;
                    i = j;
                    continue;
                }
            }
        }
        i++;
    }
    return c;
}

/* ```[\s\S]{1,200}?``` — code blocks */
static long long pat_codeblock(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i + 5 < n) {
        if (strncmp(s + i, "```", 3) == 0) {
            size_t j = i + 3;
            size_t cnt = 0;
            int found = 0;
            while (j + 2 < n && cnt <= 200) {
                if (strncmp(s + j, "```", 3) == 0 && cnt >= 1) { found = 1; break; }
                j++;
                cnt++;
            }
            if (found) { c++; i = j + 3; continue; }
        }
        i++;
    }
    return c;
}

/* \b\w{10,}\b — long words */
static long long pat_longwords(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            if (j - i >= 10) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* (?<=\s)\w+(?=,) — words before commas */
static long long pat_precomma(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == ',' && i > 0 && text_is_word_char(s[i - 1])) {
            size_t j = i - 1;
            while (j > 0 && text_is_word_char(s[j - 1])) j--;
            if (j > 0 && text_is_space(s[j - 1])) c++;
        }
        i++;
    }
    return c;
}

/* \b(?:async|await|process)\b — async keywords */
static long long pat_async(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t wl = 0;
            if (strncmp(s + i, "process", 7) == 0) wl = 7;
            else if (strncmp(s + i, "async", 5) == 0) wl = 5;
            else if (strncmp(s + i, "await", 5) == 0) wl = 5;
            if (wl && word_end(s, n, i + wl - 1)) { c++; i += wl; continue; }
        }
        i++;
    }
    return c;
}

/* \.\w+\( — method calls */
static long long pat_method(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '.' && i + 1 < n && text_is_word_char(s[i + 1])) {
            size_t j = i + 1;
            while (j < n && text_is_word_char(s[j])) j++;
            if (j < n && s[j] == '(') c++;
            i = j + 1;
        } else i++;
    }
    return c;
}

/* \b[a-f0-9]{8,}\b — hex strings */
static long long pat_hex(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i) && is_hexl(s[i])) {
            size_t j = i;
            while (j < n && is_hexl(s[j])) j++;
            if (j - i >= 8 && word_end(s, n, j - 1)) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* (?:https?://)\S+ — URLs */
static long long pat_url(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i + 7 < n) {
        size_t scheme = 0;
        if (strncmp(s + i, "https://", 8) == 0) scheme = 8;
        else if (strncmp(s + i, "http://", 7) == 0) scheme = 7;
        if (scheme) {
            size_t j = i + scheme;
            while (j < n && !text_is_space(s[j])) j++;
            if (j > i + scheme) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* \b\w+ing\b — gerunds */
static long long pat_gerund(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            if (j - i >= 4 && strncmp(s + j - 3, "ing", 3) == 0) c++;
            i = j;
        } else i++;
    }
    return c;
}

/* \b(?:the|a|an)\s+\w+ — articles + noun */
static long long pat_article(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        size_t wl = 0;
        if (word_start(s, n, i) && strncmp(s + i, "the", 3) == 0 && word_end(s, n, i + 2)) wl = 3;
        else if (word_start(s, n, i) && strncmp(s + i, "an", 2) == 0 && word_end(s, n, i + 1)) wl = 2;
        else if (word_start(s, n, i) && strncmp(s + i, "a", 1) == 0 && word_end(s, n, i)) wl = 1;
        if (wl) {
            size_t j = i + wl;
            while (j < n && text_is_space(s[j])) j++;
            if (j < n && text_is_word_char(s[j])) {
                c++;
                while (j < n && text_is_word_char(s[j])) j++;
                i = j;
                continue;
            }
        }
        i++;
    }
    return c;
}

/* \w+\.\w+\.\w+ — dotted paths */
static long long pat_dotted(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (text_is_word_char(s[i])) {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            if (j < n && s[j] == '.') {
                size_t k = j + 1;
                if (k < n && text_is_word_char(s[k])) {
                    while (k < n && text_is_word_char(s[k])) k++;
                    if (k < n && s[k] == '.') {
                        size_t m = k + 1;
                        if (m < n && text_is_word_char(s[m])) c++;
                    }
                }
            }
            i = j;
        } else i++;
    }
    return c;
}

/* \[\d+\] — array indices */
static long long pat_arrayidx(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '[') {
            size_t j = i + 1;
            if (j < n && text_is_digit(s[j])) {
                while (j < n && text_is_digit(s[j])) j++;
                if (j < n && s[j] == ']') c++;
            }
            i = j + 1;
        } else i++;
    }
    return c;
}

/* \b\d+\.\d+\b — floating point */
static long long pat_float(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (text_is_digit(s[i]) && word_start(s, n, i)) {
            size_t j = i;
            while (j < n && text_is_digit(s[j])) j++;
            if (j < n && s[j] == '.') {
                size_t k = j + 1;
                if (k < n && text_is_digit(s[k])) {
                    while (k < n && text_is_digit(s[k])) k++;
                    if (k >= n || !text_is_word_char(s[k])) c++;
                }
            }
            i = j;
        } else i++;
    }
    return c;
}

/* (?<=\n)\w+ — line starters */
static long long pat_lineseed(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (text_is_word_char(s[i]) && i > 0 && s[i - 1] == '\n') {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            c++;
            i = j;
        } else i++;
    }
    return c;
}

/* \b\w+(?:ize|ise)\b — -ize/-ise verbs */
static long long pat_izeise(const char *s, size_t n) {
    long long c = 0;
    size_t i = 0;
    while (i < n) {
        if (word_start(s, n, i)) {
            size_t j = i;
            while (j < n && text_is_word_char(s[j])) j++;
            if (j - i >= 4 &&
                (strncmp(s + j - 3, "ize", 3) == 0 || strncmp(s + j - 3, "ise", 3) == 0))
                c++;
            i = j;
        } else i++;
    }
    return c;
}

static long long run_phase2(const char *corpus, size_t clen) {
    long long ops = 0;
    ops += pat_snake(corpus, clen);
    ops += pat_camel(corpus, clen);
    ops += pat_quoted(corpus, clen);
    ops += pat_numbers(corpus, clen);
    ops += pat_error(corpus, clen);
    ops += pat_decl(corpus, clen);
    ops += pat_codeblock(corpus, clen);
    ops += pat_longwords(corpus, clen);
    ops += pat_precomma(corpus, clen);
    ops += pat_async(corpus, clen);
    ops += pat_method(corpus, clen);
    ops += pat_hex(corpus, clen);
    ops += pat_url(corpus, clen);
    ops += pat_gerund(corpus, clen);
    ops += pat_article(corpus, clen);
    ops += pat_dotted(corpus, clen);
    ops += pat_arrayidx(corpus, clen);
    ops += pat_float(corpus, clen);
    ops += pat_lineseed(corpus, clen);
    ops += pat_izeise(corpus, clen);
    return ops;
}

/* ------------------------------------------------------------------ */
/*  Phase 3: byte-pair frequency counting                              */
/*  Counts 2-char pairs over the first 100k positions (like the         */
/*  reference); the pair histogram feeds Phase 4.                       */
/* ------------------------------------------------------------------ */

static long long run_phase3(const char *corpus, size_t clen, uint32_t *cnt) {
    memset(cnt, 0, 65536 * sizeof(uint32_t));
    size_t lim = clen - 1;
    if (lim > 100000) lim = 100000;
    for (size_t i = 0; i < lim; i++) {
        unsigned idx = ((unsigned char)corpus[i] << 8) | (unsigned char)corpus[i + 1];
        cnt[idx]++;
    }
    return (long long)lim;
}

/* ------------------------------------------------------------------ */
/*  Phase 4: top-k frequent pair merging (simplified BPE step)         */
/*  Merges the 20 most frequent pairs in the first 50k chars.          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t pair;
    uint32_t count;
} pair_entry;

static void top_pairs(const uint32_t *cnt, pair_entry *out, int *nout) {
    int m = 0;
    for (unsigned idx = 0; idx < 65536; idx++) {
        if (cnt[idx] == 0) continue;
        if (m < 20) {
            out[m].pair = (uint16_t)idx;
            out[m].count = cnt[idx];
            m++;
            for (int k = m - 1; k > 0 && out[k].count > out[k - 1].count; k--) {
                pair_entry t = out[k]; out[k] = out[k - 1]; out[k - 1] = t;
            }
        } else if (cnt[idx] > out[m - 1].count) {
            out[m - 1].pair = (uint16_t)idx;
            out[m - 1].count = cnt[idx];
            for (int k = m - 1; k > 0 && out[k].count > out[k - 1].count; k--) {
                pair_entry t = out[k]; out[k] = out[k - 1]; out[k - 1] = t;
            }
        }
    }
    *nout = m;
}

static long long run_phase4(const char *corpus, size_t clen, const uint32_t *cnt) {
    pair_entry top[20];
    int ntop = 0;
    top_pairs(cnt, top, &ntop);
    if (ntop > 20) ntop = 20;

    size_t seg = clen < 50000 ? clen : 50000;
    char *work = (char *)malloc(seg + 1);
    char *tmp = (char *)malloc(seg + 1);
    if (!work || !tmp) { free(work); free(tmp); return 0; }
    memcpy(work, corpus, seg);
    work[seg] = '\0';

    long long ops = 0;
    for (int p = 0; p < ntop; p++) {
        char c0 = (char)(top[p].pair >> 8);
        char c1 = (char)(top[p].pair & 0xFF);
        char r0 = (char)toupper((unsigned char)c0);
        char r1 = (char)toupper((unsigned char)c1);
        size_t w = 0;
        size_t i = 0;
        while (i < seg) {
            if (i + 1 < seg && work[i] == c0 && work[i + 1] == c1) {
                tmp[w++] = r0;
                tmp[w++] = r1;
                i += 2;
            } else {
                tmp[w++] = work[i];
                i++;
            }
        }
        memcpy(work, tmp, w);
        work[w] = '\0';
        ops++;
    }
    free(work);
    free(tmp);
    return ops;
}

/* ------------------------------------------------------------------ */
/*  Phase 5: line-by-line processing (log parsing pattern)             */
/* ------------------------------------------------------------------ */

static long long run_phase5(const char *corpus, size_t clen) {
    long long ops = 0;
    size_t i = 0;
    while (i < clen) {
        size_t j = i;
        while (j < clen && corpus[j] != '\n') j++;
        size_t a = i;
        while (a < j && text_is_space(corpus[a])) a++;
        size_t b = j;
        while (b > a && text_is_space(corpus[b - 1])) b--;
        int tokens = 0;
        size_t k = a;
        while (k < b) {
            while (k < b && text_is_space(corpus[k])) k++;
            if (k >= b) break;
            tokens++;
            while (k < b && !text_is_space(corpus[k])) k++;
        }
        (void)tokens;
        ops++;
        i = (j < clen) ? j + 1 : clen;
    }
    return ops;
}

/* ------------------------------------------------------------------ */
/*  Parallel worker                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int sets;
    const char *corpus;
    size_t clen;
} text_work_t;

static void *text_worker(void *arg) {
    text_work_t *w = (text_work_t *)arg;
    const char *corpus = w->corpus;
    size_t clen = w->clen;
    uint32_t *cnt = (uint32_t *)calloc(65536, sizeof(uint32_t));
    if (!cnt) return work_ops(0);
    long long ops = 0;
    for (int r = 0; r < w->sets; r++) {
        ops += run_phase1(corpus, clen);
        ops += run_phase2(corpus, clen);
        ops += run_phase3(corpus, clen, cnt);
        ops += run_phase4(corpus, clen, cnt);
        ops += run_phase5(corpus, clen);
    }
    free(cnt);
    return work_ops(ops);
}

long long run_text_processing(int total_sets, int corpus_size, int threads) {
    if (total_sets <= 0) total_sets = 256;
    if (corpus_size <= 0) corpus_size = 500000;

    size_t clen = 0;
    char *corpus = build_corpus(corpus_size, &clen);
    if (!corpus) return 0;

    threads = work_threads(total_sets, threads);
    text_work_t *work = (text_work_t *)calloc((size_t)threads, sizeof *work);
    if (!work) {
        fprintf(stderr, "error: text_processing: out of memory for worker arrays\n");
        free(corpus);
        return 0;
    }
    for (int i = 0; i < threads; i++) {
        work[i].sets = work_slice(total_sets, threads, i);
        work[i].corpus = corpus;
        work[i].clen = clen;
    }

    long long total = work_run(work, sizeof *work, threads, text_worker,
                               "text_processing");
    free(work);
    free(corpus);
    return total;
}
