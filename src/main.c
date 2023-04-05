#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/mman.h>
#include <nxlog/nxlog.h>

#define MAX_TOK 256

enum {
    T_NONE,
    T_EOF,
    T_L_PAREN, T_R_PAREN, T_COMMA, T_COLON,
    T_STR, T_NUM, T_TRUE, T_FALSE,
    T_RECORD, // not used in parsing, only when packing as a TLV type
};

// Not perfectly aligned but not important
typedef struct {
    char type;
    char *start;
    int len;
    int num;
} Tok;

typedef struct {
    FILE *fp;
    Tok prev;
    Tok cur;
    FILE *fpbj;
    Table *strtab;
    int nkeys;
    Table *keytab;
} Parser;

typedef struct {
    Table *dict;
} Unpacker;

// buf has to be zero terminated
static char *newstr(Parser *p, char *buf, int len) {
    if (tabhas(p->strtab, buf)) {
        printf("already has %s\n", buf);
        return tabget(p->strtab, buf).str;
    }
    char *cpy = malloc(len + 1);
    strncpy(cpy, buf, len);
    cpy[len] = 0;
    tabput(p->strtab, cpy, (TVal){.str=cpy});
    return cpy;
}

// Not using mmap() in case we want the input to be some kind of pipe.
// Not buffering input manually and relying on fgetc() buffering
// being good enough for the task.
// Using a maximum of 1 ungetc().
// Token size limited to MAX_TOK.
static Tok nexttok(Parser *p) {
    char buf[MAX_TOK];
    int len = 0;
    int c = fgetc(p->fp);
    int esc = 0; // for escaping double quotes in strings
    while (isspace(c))
        c = fgetc(p->fp);
    switch (c) {
    case EOF: return (Tok){T_EOF};
    case '{': return (Tok){T_L_PAREN};
    case '}': return (Tok){T_R_PAREN};
    case ',': return (Tok){T_COMMA};
    case ':': return (Tok){T_COLON};
    case '\"': buf[len++] = c; c = fgetc(p->fp); goto str;
    case '-': buf[len++] = c; c = fgetc(p->fp); goto num;
    case 't': case 'f': goto boolean;
    }
    if (isdigit(c)) goto num;
    ERR("unexpected char %c", c);
num:
    while (isdigit(c)) {
        buf[len++] = c;
        c = fgetc(p->fp);
    }
    buf[len] = 0;
    ungetc(c, p->fp);
    return (Tok){T_NUM, 0, 0, atoi(buf)};
str:
    while (c) {
        if (!esc && c == '\\') {
            esc = 1;
            buf[len++] = c;
            c = fgetc(p->fp);
            continue;
        }
        else if (!esc && c == '"') {
            buf[len++] = c;
            // strip quotes
            buf[len - 1] = 0;
            return (Tok){T_STR, newstr(p, buf + 1, len - 2), len - 2};
        }
        buf[len++] = c;
        c = fgetc(p->fp);
        esc = 0;
    }
    ERR("unterminated string");
boolean:
    while (isalpha(c)) {
        buf[len++] = c;
        c = fgetc(p->fp);
    }
    ungetc(c, p->fp);
    if (strncmp("true", buf, len) == 0)
        return (Tok){T_TRUE};
    else if (strncmp("false", buf, len) == 0)
        return (Tok){T_FALSE};
    ERR("unexpected value %.*s", len, buf);
    return (Tok){T_EOF};
}

static void advance(Parser *p) {
    p->prev = p->cur;
    p->cur = nexttok(p);
}

static int match(Parser *p, int type) {
    if (p->cur.type != type) return 0;
    advance(p);
    return 1;
}

static void expect(Parser *p, int type) {
    if (match(p, type)) return;
    ERR("unexpected token %i, expected %i", p->cur.type, type);
}

static int peek(Parser *p) {
    return p->cur.type;
}

static void expectval(Parser *p) {
    advance(p);
    switch (p->prev.type) {
    case T_STR:
    case T_NUM:
    case T_TRUE:
    case T_FALSE:
        return;
    }
    ERR("expected value");
}

static void parserecord(Parser *p) {
    expect(p, T_L_PAREN);
    char type = T_RECORD;
    int len = 0;
    fwrite(&type, 1, 1, p->fpbj);
    fpos_t reclen;
    fgetpos(p->fpbj, &reclen);
    fwrite(&len, sizeof(int), 1, p->fpbj);
    long recstart = ftell(p->fpbj);
    while (peek(p) != T_R_PAREN) {
        expect(p, T_STR);
        Tok key = p->prev;
        if (!tabhas(p->keytab, key.start)) {
            tabput(p->keytab, key.start, (TVal){.i=p->nkeys});
            p->nkeys++;
        }
        int keyi = tabget(p->keytab, key.start).i;
        expect(p, T_COLON);
        expectval(p);
        Tok val = p->prev;

        printf("%.*s --- ", key.len, key.start);
        switch (val.type) {
        case T_TRUE: printf("true\n"); break;
        case T_FALSE: printf("false\n"); break;
        case T_NUM: printf("%i\n", val.num); break;
        case T_STR: printf("%.*s\n", val.len, val.start); break;
        default: break;
        }

        fwrite(&val.type, 1, 1, p->fpbj);
        len = sizeof(int); // size of int key
        if (val.type == T_NUM) len += sizeof(int);
        else if (val.type == T_STR) len += val.len;
        fwrite(&len, sizeof(int), 1, p->fpbj);
        fwrite(&keyi, sizeof(int), 1, p->fpbj);
        if (val.type == T_NUM)
            fwrite(&val.num, sizeof(int), 1, p->fpbj);
        else if (val.type == T_STR)
            fwrite(val.start, val.len, 1, p->fpbj);

        if (!match(p, T_COMMA)) break;
    }
    expect(p, T_R_PAREN);
    long recend = ftell(p->fpbj);
    len = recend - recstart;
    fsetpos(p->fpbj, &reclen);
    fwrite(&len, sizeof(int), 1, p->fpbj);
    fseek(p->fpbj, len, SEEK_CUR);
}

static void parse(Parser *p) {
    advance(p);
    while (!match(p, T_EOF)) {
        parserecord(p);
    }
}

static void packdict(const char *path, Parser *p) {
    FILE *fp = fopen(path, "wb");
    TSlot slot;
    int i = 0;
    while ((i = tabgeti(p->keytab, i, &slot))) {
        char type = T_STR;
        int strl = strlen(slot.key);
        int len = sizeof(int) + strl; // index + strlen
        fwrite(&type, 1, 1, fp);
        fwrite(&len, sizeof(int), 1, fp);
        fwrite(&slot.val.i, sizeof(int), 1, fp);
        fwrite(slot.key, strl, 1, fp);
    }
    fclose(fp);
}

static void pack(const char *path) {
    char tmp[FILENAME_MAX];
    Parser p = {0};
    p.strtab = newtab();
    p.keytab = newtab();
    p.fp = fopen(path, "r");
    if (!p.fp) ERR("couldn't open %s", path);
    chext(tmp, path, "bj");
    p.fpbj = fopen(tmp, "wb");
    if (!p.fpbj) ERR("couldn't open %s", tmp);
    parse(&p);
    chext(tmp, path, "dict");
    packdict(tmp, &p);
    fclose(p.fp);
    fclose(p.fpbj);
    freetab(p.strtab);
    freetab(p.keytab);
}

static void loaddict(Unpacker *up, const char *path) {
    printf("---- %s ---\n", path);
    FILE *fp = fopen(path, "rb");
    if (!fp) ERR("couldn't load dictionary");
    char type;
    int len;
    int id;
    for (;;) {
        // could batch read, but i'll leave it more readable
        if (fread(&type, 1, 1, fp) != 1) break;
        if (fread(&len, sizeof(int), 1, fp) != 1) break;
        if (fread(&id, sizeof(int), 1, fp) != 1) break;
        int strl = len - sizeof(int);
        char *str = malloc(strl + 1);
        if (fread(str, strl, 1, fp) != 1) break;
        str[strl] = 0;
        printf("%i %s\n", id, str);
        char *key = malloc(16);
        sprintf(key, "%i", id);
        tabput(up->dict, key, (TVal){str});
    }
    fclose(fp);
}

static void unpack(const char *bin, const char *dict, const char *out) {
    Unpacker up = {0};
    up.dict = newtab();
    loaddict(&up, dict);
    FILE *fpbj = fopen(bin, "rb");
    if (!fpbj) ERR("couldn't open %s", bin);
    FILE *fpjson = fopen(out, "wb");
    if (!fpjson) ERR("couldn't open %s", out);
    char type = 0;
    int reclen;
    int len;
    int keyi;
    int num;
    char str[64];
    printf("--- bin json ---\n");
    for (;;) {
        if (fread(&type, 1, 1, fpbj) != 1) break;
        if (fread(&reclen, sizeof(int), 1, fpbj) != 1) break;
        printf("reclen %i\n", reclen);
        fprintf(fpjson, "{");
        long pos = ftell(fpbj);
        while (ftell(fpbj) < pos + reclen) {
            if (type != T_RECORD) fprintf(fpjson, ",");
            if (fread(&type, 1, 1, fpbj) != 1) break;
            if (fread(&len, sizeof(int), 1, fpbj) != 1) break;
            if (fread(&keyi, sizeof(int), 1, fpbj) != 1) break;
            char keyibuf[16];
            sprintf(keyibuf, "%i", keyi);
            char *keystr = tabget(up.dict, keyibuf).str;
            fprintf(fpjson, "\"%s\":", keystr);
            switch (type) {
            case T_TRUE: fprintf(fpjson, "true"); break;
            case T_FALSE: fprintf(fpjson, "false"); break;
            case T_NUM:
                fread(&num, sizeof(int), 1, fpbj);
                fprintf(fpjson, "%i", num);
                break;
            case T_STR:
                fread(&str, len - sizeof(int), 1, fpbj);
                str[len - sizeof(int)] = 0;
                fprintf(fpjson, "\"%s\"", str);
                break;
            }
        }
        fprintf(fpjson, "}\n");
    }
    fclose(fpbj);
    fclose(fpjson);
    freetab(up.dict);
}

static const char *OPTS[] = {
    "-h:print this message and exit",
    "-b file:packed binary to decode back to json",
    "-d file:string dictionary",
    "-o file:destination of decoded json",
    0,
};

static void help() {
    printf("Usage: jpak file.json\n");
    for (const char **opt = OPTS; *opt; opt++) {
        char *sep = strchr(*opt, ':');
        printf("%4s%-8.*s%s\n", "", (int)(sep - *opt), *opt, sep + 1);
    }
}

int main(int argc, char **argv) {
    char *path = 0;
    int ohelp = 0;
    char *obin = 0;
    char *odict = 0;
    char *oout = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp("-h", argv[i]) == 0) ohelp = 1;
        else if (strcmp("-b", argv[i]) == 0) obin = argv[++i];
        else if (strcmp("-d", argv[i]) == 0) odict = argv[++i];
        else if (strcmp("-o", argv[i]) == 0) oout = argv[++i];
        else if (!path) path = argv[i];
        else {
            help();
            ERR("stray operand %s", argv[i]);
        }
    }
    if (ohelp) {
        help();
        return 0;
    }
    else if (obin || odict || oout) {
        if (obin && odict && oout) {
            unpack(obin, odict, oout);
            return 0;
        }
        help();
        ERR("to decode provide a binary, a dictionary and an output file");
    }
    else if (!path) {
        help();
        ERR("missing input file");
    }
    else {
        pack(path);
    }
    return 0;
}