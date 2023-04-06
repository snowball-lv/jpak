#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <jpak/jpak.h>

#define MAX_STR 4096

// token types for the lexer
// also used as types in TLV
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
    char *start; // only holds values for string types
    int32_t len; // length of string (a bit redundant after rework)
    int32_t num; // number type value
} Tok;

// the context structure getting passed around when packing
// holds all necessary data, like input/output files and dictionaries
typedef struct {
    FILE *fp;
    Tok prev;
    Tok cur;
    FILE *fpbj;
    Table *strtab;
    int nkeys;
    Table *keytab;
    char *tbuf; // buffer for lexing, malloc to MAX_STR + space for terminal 0
} Parser;

// context structure when unpacked
typedef struct {
    Table *dict;
    const char *pbin;   // binary file path to unpack
    const char *pdict;  // dictionary file path
    const char *pout;   // unpacked json path
    int debug;          // set to print dict. to stdout when unpacking
} Unpacker;

static int32_t swap32(int32_t i) {
    return ((i >> 24) & 0x000000ff)
         | ((i >> 8) & 0x0000ff00)
         | ((i << 8) & 0x00ff0000)
         | ((i << 24) & 0xff000000);
}

static void fwrite32(FILE *fp, int32_t i) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    i = swap32(i);
#endif
    fwrite(&i, sizeof(int32_t), 1, fp);
}

static int32_t fread32(FILE *fp) {
    int32_t dst;
    if (fread(&dst, sizeof(int32_t), 1, fp) != 1)
        ERR("Read failed");
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    dst = swap32(dst);
#endif
    return dst;
}

// interns new strings and adds them to strtab.
// buf has to be zero terminated.
// we could prepend each string with a header holding it's length and
// pre-calculated hash and require the user to only use interned strings with
// the hashmap but i'd like to keep the requirements looser and the speed
// penalty isn't that bad.
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
    int len = 0;
    int c = fgetc(p->fp);
    int esc = 0; // for escaping double quotes in strings
    // skip over whitespace
    while (isspace(c))
        c = fgetc(p->fp);
    switch (c) {
    case EOF: return (Tok){T_EOF};
    case '{': return (Tok){T_L_PAREN};
    case '}': return (Tok){T_R_PAREN};
    case ',': return (Tok){T_COMMA};
    case ':': return (Tok){T_COLON};
    case '\"': p->tbuf[len++] = c; c = fgetc(p->fp); goto str;
    case '-': p->tbuf[len++] = c; c = fgetc(p->fp); goto num;
    case 't': case 'f': goto boolean;
    }
    if (isdigit(c)) goto num;
    ERR("unexpected char %c", c);
num:
    while (isdigit(c)) {
        if (len >= MAX_STR) ERR("Max token size exceeded");
        p->tbuf[len++] = c;
        c = fgetc(p->fp);
    }
    p->tbuf[len] = 0;
    ungetc(c, p->fp);
    return (Tok){T_NUM, 0, 0, atoi(p->tbuf)};
str:
    while (c) {
        if (len >= MAX_STR) ERR("Max token size exceeded");
        if (!esc && c == '\\') {
            esc = 1;
            p->tbuf[len++] = c;
            c = fgetc(p->fp);
            continue;
        }
        else if (!esc && c == '"') {
            p->tbuf[len++] = c;
            // strip start and end quotes from strings
            p->tbuf[len - 1] = 0;
            return (Tok){T_STR, newstr(p, p->tbuf + 1, len - 2), len - 2};
        }
        p->tbuf[len++] = c;
        c = fgetc(p->fp);
        esc = 0;
    }
    ERR("unterminated string");
boolean:
    while (isalpha(c)) {
        if (len >= MAX_STR) ERR("Max token size exceeded");
        p->tbuf[len++] = c;
        c = fgetc(p->fp);
    }
    ungetc(c, p->fp);
    if (strncmp("true", p->tbuf, len) == 0)
        return (Tok){T_TRUE};
    else if (strncmp("false", p->tbuf, len) == 0)
        return (Tok){T_FALSE};
    ERR("unexpected value %.*s", len, p->tbuf);
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
    fwrite32(p->fpbj, len);
    // record starting position of record and update the length
    // after writing out all the fields of the record
    long recstart = ftell(p->fpbj);
    while (peek(p) != T_R_PAREN) {
        expect(p, T_STR);
        
        // assign each new key a sequential integer id
        Tok key = p->prev;
        if (!tabhas(p->keytab, key.start)) {
            tabput(p->keytab, key.start, (TVal){.i=p->nkeys});
            p->nkeys++;
        }
        int32_t keyi = tabget(p->keytab, key.start).i;

        expect(p, T_COLON);
        expectval(p);
        Tok val = p->prev;

        // write out in TLV format
        fwrite(&val.type, 1, 1, p->fpbj);
        len = sizeof(int32_t); // size of int key
        if (val.type == T_NUM) len += sizeof(int32_t);
        else if (val.type == T_STR) len += val.len;
        fwrite32(p->fpbj, len);
        fwrite32(p->fpbj, keyi);
        if (val.type == T_NUM)
            fwrite32(p->fpbj, val.num);
        else if (val.type == T_STR)
            fwrite(val.start, val.len, 1, p->fpbj);

        // if next tok isn't a comma we assume we're done with the record
        if (!match(p, T_COMMA)) break;
    }
    expect(p, T_R_PAREN);
    // jump back, update record length and jump back to current pos again
    long recend = ftell(p->fpbj);
    len = recend - recstart;
    fsetpos(p->fpbj, &reclen);
    fwrite32(p->fpbj, len);
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
        fwrite32(fp, len);
        fwrite32(fp, slot.val.i);
        fwrite(slot.key, strl, 1, fp);
    }
    fclose(fp);
}

static void pack(const char *path) {
    if (!path) printf("No input file specified, using stdin\n");
    char tmp[FILENAME_MAX];
    Parser p = {0};
    p.tbuf = malloc(MAX_STR + 1); // + space for terminal 0
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
    free(p.tbuf);
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
        len = fread32(fp);
        id = fread32(fp);
        int strl = len - sizeof(int32_t);
        // remember to delete allocated key and string !!!
        char *str = malloc(strl + 1);
        // bug only if read failed AND strl > 0 (empty strings also exist)
        if (strl && fread(str, strl, 1, fp) != 1) goto bug;
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
    int strl;
    char str[MAX_STR];
    for (;;) {

        // if the first read of the record fails we're safe and can break
        // all other failures are likely bugs in the file
        if (fread(&type, 1, 1, fpbj) != 1) break;
        reclen = fread32(fpbj);
        fprintf(fpjson, "{");
        
        // loop over fields until we exceed the size of the current record
        long pos = ftell(fpbj);
        while (ftell(fpbj) < pos + reclen) {
            if (type != T_RECORD) fprintf(fpjson, ",");
            if (fread(&type, 1, 1, fpbj) != 1) goto bug;
            len = fread32(fpbj);
            keyi = fread32(fpbj);
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
                num = fread32(fpbj);
                fprintf(fpjson, "%i", num);
                break;
            case T_STR:
                strl = len - sizeof(int32_t);
                // fail only of strl > 0 (empty strings exist)
                if (strl && fread(&str, strl, 1, fpbj) != 1)
                    continue;
                str[strl] = 0;
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
    // delete dictionary keys and values
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
