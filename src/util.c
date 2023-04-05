#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <jpak/jpak.h>

void err(char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vprintf(fmt, va);
    va_end(va);
    exit(1);
}

// http://www.cse.yorku.ca/~oz/hash.html
int strhash(const char *str) {
    int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}

// changes file extension, make sure the dst buffer is big enough
void chext(char *dst, const char *path, const char *ext) {
    strcpy(dst, path);
    char *period = strrchr(dst, '.');
    if (period) sprintf(period, ".%s", ext);
    else sprintf(&dst[strlen(path)], ".%s", ext);
}

// quick and dirty hashmap

struct Table {
    TSlot *slots;
    int nslots;
    int nused;
};

Table *newtab() {
    return calloc(1, sizeof(Table));
}

void freetab(Table *t) {
    if (t->slots) free(t->slots);
    free(t);
}

// we do't remove items from the map so tombstones aren't necessary
// % could be optimized away if only sticking to power of 2 table sizes
static TSlot *find(Table *t, const char *key) {
    if (!t->nslots) return 0;
    int hash = strhash(key);
    int idx = hash % t->nslots;
    TSlot *s = &t->slots[idx];
    while (s->key && strcmp(s->key, key) != 0) {
        idx = (idx + 1) % t->nslots;
        s = &t->slots[idx];
    }
    return s;
}

int tabhas(Table *t, const char *key) {
    TSlot *s = find(t, key);
    return s && s->key;
}

TVal tabget(Table *t, const char *key) {
    TSlot *s = find(t, key);
    return s && s->key ? s->val : (TVal){0};
}

static void grow(Table *t) {
    Table tmp = {0};
    tmp.nslots = t->nslots ? t->nslots * 2 : 2;
    // printf("Growing %i -> %i\n", t->nslots, tmp.nslots);
    tmp.nused = 0;
    tmp.slots = calloc(tmp.nslots, sizeof(TSlot));
    for (int i = 0; i < t->nslots; i++) {
        TSlot *s = &t->slots[i];
        if (!s->key) continue;
        tabput(&tmp, s->key, s->val);
    }
    if (t->slots) free(t->slots);
    *t = tmp;
}

// make sure the key isn't stack allocated in case it gets added to the table
void tabput(Table *t, const char *key, TVal val) {
    if (t->nused * 2 >= t->nslots)
        grow(t);
    TSlot *s = find(t, key);
    if (!s->key) t->nused++;
    s->key = key;
    s->val = val;
}

// absolutely no modification of table while iterating
int tabgeti(Table *t, int i, TSlot *dst) {
    for (int k = i; k < t->nslots; k++) {
        TSlot *s = &t->slots[k];
        if (!s->key) continue;
        *dst = *s;
        return k + 1;
    }
    return 0;
}
