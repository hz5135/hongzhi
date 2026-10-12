#ifndef BENCH_STRLIST_H
#define BENCH_STRLIST_H

/**
 * strlist.h — a growable list of strings.
 *
 * Two kernels collect generated text as a list of lines before it is joined,
 * counted, or scanned: diff_patch holds the unified-diff lines of one file, and
 * agentic_loop collects the project's file paths.  Every string is copied into
 * the list, so the list owns it and frees it.
 *
 *     strlist_t list = {0};
 *     strlist_push(&list, line);
 *     ... list.items[0..list.n-1] ...
 *     strlist_free(&list);
 */

typedef struct {
    char **items;   /* the strings, each owned by the list */
    int n;          /* how many are in use */
    int cap;        /* slots allocated */
} strlist_t;

/* Append a copy of `s`. */
void strlist_push(strlist_t *l, const char *s);

/* Free every string and the array itself, leaving the list empty. */
void strlist_free(strlist_t *l);

#endif /* BENCH_STRLIST_H */
