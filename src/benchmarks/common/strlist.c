#include "strlist.h"

#include <stdlib.h>
#include <string.h>

#include "core.h"   /* xrealloc */

void strlist_push(strlist_t *l, const char *s) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (char **)xrealloc(l->items, (size_t)l->cap * sizeof(char *));
    }
    l->items[l->n++] = strdup(s);
}

void strlist_free(strlist_t *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}
