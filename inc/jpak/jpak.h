#pragma once


void err(char *fmt, ...);
#define ERR(fmt, ...) err("*** " fmt "\n", ##__VA_ARGS__)

void chext(char *dst, const char *path, const char *ext);

typedef union {
    char *str;
    int32_t i;
} TVal;

typedef struct {
    const char *key;
    unsigned hash;
    TVal val;
} TSlot;

typedef struct Table Table;

Table *newtab();
void freetab(Table *t);
int tabhas(Table *t, const char *key);
TVal tabget(Table *t, const char *key);
void tabput(Table *t, const char *key, TVal val);
int tabgeti(Table *t, int i, TSlot *dst);
