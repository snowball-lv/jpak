#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <jpak/jpak.h>

#define MAX_STR 512

enum {
    T_NONE,
    T_EOF,
    T_L_PAREN, T_R_PAREN, T_COMMA, T_COLON,
    T_STR, T_NUM, T_TRUE, T_FALSE,
    T_RECORD, // not used in parsing, only when packing as a TLV type
};

// Not perfectly aligned but not important
typedef struct {
    int8_t type;
    char *start;
    int32_t len;
    int32_t num;
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
    const char *pbin;
    const char *pdict;
    const char *pout;
    int debug;
} Unpacker;

// interns new strings and adds them to strtab
// buf has to be zero terminated
static char *newstr(Parser *p, char *buf, int len) {
    if (tabhas(p->strtab, buf))
        return tabget(p->strtab, buf).str;
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
// Token size limited to MAX_STR.
static Tok nexttok(Parser *p) {
    char buf[MAX_STR];
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

// parses records and simultaneously writes them to the binary
static void parserecord(Parser *p) {
    expect(p, T_L_PAREN);
    int8_t type = T_RECORD;
    int32_t len = 0;
    fwrite(&type, 1, 1, p->fpbj);
    fpos_t reclen;
    fgetpos(p->fpbj, &reclen);
    fwrite(&len, sizeof(int32_t), 1, p->fpbj);
    // record starting position of record and update the length
    // after writing out all the fields of the record
    long recstart = ftell(p->fpbj);
    while (peek(p) != T_R_PAREN) {
        expect(p, T_STR);
        Tok key = p->prev;
        if (!tabhas(p->keytab, key.start)) {
            tabput(p->keytab, key.start, (TVal){.i=p->nkeys});
            p->nkeys++;
        }
        int32_t keyi = tabget(p->keytab, key.start).i;
        expect(p, T_COLON);
        expectval(p);
        Tok val = p->prev;
        fwrite(&val.type, 1, 1, p->fpbj);
        len = sizeof(int32_t); // size of int key
        if (val.type == T_NUM) len += sizeof(int32_t);
        else if (val.type == T_STR) len += val.len;
        fwrite(&len, sizeof(int32_t), 1, p->fpbj);
        fwrite(&keyi, sizeof(int32_t), 1, p->fpbj);
        if (val.type == T_NUM)
            fwrite(&val.num, sizeof(int32_t), 1, p->fpbj);
        else if (val.type == T_STR)
            fwrite(val.start, val.len, 1, p->fpbj);
        if (!match(p, T_COMMA)) break;
    }
    expect(p, T_R_PAREN);
    long recend = ftell(p->fpbj);
    len = recend - recstart;
    fsetpos(p->fpbj, &reclen);
    fwrite(&len, sizeof(int32_t), 1, p->fpbj);
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
        int8_t type = T_STR;
        int strl = strlen(slot.key);
        int32_t len = sizeof(int32_t) + strl; // index + strlen
        fwrite(&type, 1, 1, fp);
        fwrite(&len, sizeof(int32_t), 1, fp);
        fwrite(&slot.val.i, sizeof(int32_t), 1, fp);
        fwrite(slot.key, strl, 1, fp);
    }
    fclose(fp);
}

static void pack(const char *path) {
    if (!path) printf("No input file specified, using stdin\n");
    char tmp[FILENAME_MAX];
    Parser p = {0};
    p.strtab = newtab();
    p.keytab = newtab();
    p.fp = path ? fopen(path, "r") : stdin;
    if (!path) path = "records.json";
    if (!p.fp) ERR("couldn't open %s", path);
    chext(tmp, path, "bj");
    p.fpbj = fopen(tmp, "wb");
    if (!p.fpbj) ERR("couldn't open %s", tmp);
    parse(&p);
    chext(tmp, path, "dict");
    packdict(tmp, &p);
    fclose(p.fp);
    fclose(p.fpbj);
    // delete interned strings
    int i = 0;
    TSlot slot;
    while ((i = tabgeti(p.strtab, i, &slot))) {
        free(slot.val.str);
    }
    freetab(p.strtab);
    freetab(p.keytab);
}

// loads the dictionary file to a table
// the mappings are reversed i.e. numbers get mapped to strings
// the hashmap doesn't support int keys so they're stringified
// a bit dirty but not a big speed penalty and saves us from adding special
// cases to the hashmap or making a new one
static void loaddict(Unpacker *up, const char *path) {
    if (up->debug)
        printf("--- %s ---\n", up->pdict);
    FILE *fp = fopen(path, "rb");
    if (!fp) ERR("couldn't load dictionary");
    int8_t type;
    int32_t len;
    int32_t id;
    for (;;) {
        // could batch read, but i'll leave it more readable
        // first read can fail, the rest are likely bugs
        if (fread(&type, 1, 1, fp) != 1) break;
        if (fread(&len, sizeof(int32_t), 1, fp) != 1) goto bug;
        if (fread(&id, sizeof(int32_t), 1, fp) != 1) goto bug;
        int strl = len - sizeof(int32_t);
        char *str = malloc(strl + 1);
        if (fread(str, strl, 1, fp) != 1) goto bug;
        str[strl] = 0;
        char *key = malloc(16);
        sprintf(key, "%i", id);
        tabput(up->dict, key, (TVal){str});
        if (up->debug)
            printf("\"%s\": %i\n", str, id);
    }
    goto end;
bug:
    ERR("malformed dictionary");
end:
    fclose(fp);
}

static void unpack(Unpacker *up) {
    up->dict = newtab();
    loaddict(up, up->pdict);
    FILE *fpbj = fopen(up->pbin, "rb");
    if (!fpbj) ERR("couldn't open %s", up->pbin);
    FILE *fpjson = fopen(up->pout, "wb");
    if (!fpjson) ERR("couldn't open %s", up->pout);
    int8_t type = 0;
    int32_t reclen;
    int32_t len;
    int32_t keyi;
    int32_t num;
    char str[MAX_STR];
    for (;;) {
        // if the first read of the record fails we're safe and can break
        // all other failures are likely bugs in the file
        if (fread(&type, 1, 1, fpbj) != 1) break;
        if (fread(&reclen, sizeof(int32_t), 1, fpbj) != 1) goto bug;
        fprintf(fpjson, "{");
        long pos = ftell(fpbj);
        while (ftell(fpbj) < pos + reclen) {
            if (type != T_RECORD) fprintf(fpjson, ",");
            if (fread(&type, 1, 1, fpbj) != 1) goto bug;
            if (fread(&len, sizeof(int32_t), 1, fpbj) != 1) goto bug;
            if (fread(&keyi, sizeof(int32_t), 1, fpbj) != 1) goto bug;
            char keyibuf[16]; // buffer big enough to store a 4 byte int
            sprintf(keyibuf, "%i", keyi);
            if (!tabhas(up->dict, keyibuf))
                ERR("Dictionary missing key %i", keyi);
            char *keystr = tabget(up->dict, keyibuf).str;
            fprintf(fpjson, "\"%s\":", keystr);
            switch (type) {
            case T_TRUE: fprintf(fpjson, "true"); break;
            case T_FALSE: fprintf(fpjson, "false"); break;
            case T_NUM:
                if (fread(&num, sizeof(int32_t), 1, fpbj) != 1)
                    continue;
                fprintf(fpjson, "%i", num);
                break;
            case T_STR:
                if (fread(&str, len - sizeof(int32_t), 1, fpbj) != 1)
                    continue;
                str[len - sizeof(int32_t)] = 0;
                fprintf(fpjson, "\"%s\"", str);
                break;
            }
        }
        fprintf(fpjson, "}\n");
    }
    goto end;
bug:
    ERR("malformed binary");
end:
    fclose(fpbj);
    fclose(fpjson);
    // delete dictionary keys and vlaues
    int i = 0;
    TSlot slot;
    while ((i = tabgeti(up->dict, i, &slot))) {
        free((void *)slot.key);
        free(slot.val.str);
    }
    freetab(up->dict);
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
        printf("%4s%-12.*s%s\n", "", (int)(sep - *opt), *opt, sep + 1);
    }
}

int main(int argc, char **argv) {
    char *path = 0;
    int ohelp = 0;
    char *obin = 0;
    char *odict = 0;
    char *oout = 0;
    int odebug = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp("-h", argv[i]) == 0) ohelp = 1;
        else if (strcmp("-b", argv[i]) == 0) obin = argv[++i];
        else if (strcmp("-d", argv[i]) == 0) odict = argv[++i];
        else if (strcmp("-o", argv[i]) == 0) oout = argv[++i];
        else if (strcmp("-g", argv[i]) == 0) odebug = 1;
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
            Unpacker up = {0};
            up.pbin = obin;
            up.pdict = odict;
            up.pout = oout;
            up.debug = odebug;
            unpack(&up);
            return 0;
        }
        help();
        ERR("to decode provide a binary, a dictionary and an output file");
    }
    else {
        pack(path);
    }
    return 0;
}
