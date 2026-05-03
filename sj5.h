// sj5.h JSON5 parser, based on sj.h by rxi 2025
// public domain - no warranty implied, use at your own risk

#ifndef SJ5_H
#define SJ5_H

#include <lf.h>

typedef struct {
    u8 *data, *cur, *end;
    int depth;
    char *error;
} sj5_Reader;

enum sj5_Type { SJ5_ERROR, SJ5_END, SJ5_ARRAY, SJ5_OBJECT, SJ5_NUMBER, SJ5_STRING, SJ5_BOOL, SJ5_NULL };
typedef struct {
    enum sj5_Type type;
    str content;
    int depth;
} sj5_Value;


sj5_Reader sj5_reader(u8 *data, size_t len);
sj5_Value sj5_read(sj5_Reader *r);
bool sj5_iter_array(sj5_Reader *r, sj5_Value arr, sj5_Value *val);
bool sj5_iter_object(sj5_Reader *r, sj5_Value obj, sj5_Value *key, sj5_Value *val);
void sj5_location(sj5_Reader *r, int *line, int *col);

#endif//SJ5_H
#ifdef SJ5_IMPL

sj5_Reader sj5_reader(u8 *data, size_t len) {
    return (sj5_Reader){ .data = data, .cur = data, .end = data + len };
}


static bool sj5__is_ident_start(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

static bool sj5__is_ident_cont(u8 c) {
    return sj5__is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool sj5__is_number_cont(u8 c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
        ||  c == 'e' || c == 'E' || c == '.' || c == '-' || c == '+' || c == 'x' || c == 'X';
}

static bool sj5__is_keyword(u8 *cur, u8 *end, u8 *kw) {
    while (*kw) {
        if (cur == end || *cur != *kw) return false;
        kw++, cur++;
    }
    return cur == end || !sj5__is_ident_cont(*cur);
}

sj5_Value sj5_read(sj5_Reader *r) {
    sj5_Value res;
top:
    if (r->error) { return (sj5_Value){ .type = SJ5_ERROR, .content = (str){ (u8*)r->cur, 0 } }; }
    if (r->cur == r->end) { r->error = "unexpected eof"; goto top; }
    res.content.str = (u8*)r->cur;

    switch (*r->cur) {
    case ' ': case '\n': case '\r': case '\t':
    case ':': case ',':
        r->cur++;
        goto top;

    case '/':
        if (r->cur + 1 != r->end && r->cur[1] == '/') {
            while (r->cur != r->end && *r->cur != '\n') { r->cur++; }
        } else if (r->cur + 1 != r->end && r->cur[1] == '*') {
            r->cur += 2;
            while (r->cur + 1 < r->end && !(r->cur[0] == '*' && r->cur[1] == '/')) { r->cur++; }
            if (r->cur + 1 < r->end) { r->cur += 2; }
            else { r->error = "unclosed block comment"; goto top; }
        } else {
            r->error = "unknown token";
            goto top;
        }
        goto top;

    case '+': case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        res.type = SJ5_NUMBER;
        while (r->cur != r->end && sj5__is_number_cont(*r->cur)) { r->cur++; }
        break;

    case '"': case '\'': {
        u8 stop = *r->cur++;
        res.type = SJ5_STRING;
        res.content.str = (u8*)r->cur;
        for (;;) {
            if ( r->cur == r->end) { r->error = "unclosed string"; goto top; }
            if (*r->cur ==   stop) { break; }
            if (*r->cur ==   '\\') { r->cur++; }
            if ( r->cur != r->end) { r->cur++; }
        }
        res.content.len = r->cur - (u8*)res.content.str;
        r->cur++;
        return res;
    }

    case '{': case '[':
        res.type = (*r->cur == '{') ? SJ5_OBJECT : SJ5_ARRAY;
        res.depth = ++r->depth;
        r->cur++;
        break;

    case '}': case ']':
        res.type = SJ5_END;
        if (--r->depth < 0) {
            r->error = (*r->cur == '}') ? "stray '}'" : "stray ']'";
            goto top;
        }
        r->cur++;
        break;

    case 'n': case 't': case 'f':
        res.type = (*r->cur == 'n') ? SJ5_NULL : SJ5_BOOL;
        if (sj5__is_keyword(r->cur, r->end,  "null")) { r->cur += 4; break; }
        if (sj5__is_keyword(r->cur, r->end,  "true")) { r->cur += 4; break; }
        if (sj5__is_keyword(r->cur, r->end, "false")) { r->cur += 5; break; }
        // fallthrough

    default:
        if (sj5__is_ident_start(*r->cur)) {
            res.type = SJ5_STRING;
            while (r->cur != r->end && sj5__is_ident_cont(*r->cur)) { r->cur++; }
            break;
        }
        r->error = "unknown token";
        goto top;
    }
    res.content.len = r->cur - (u8*)res.content.str;
    return res;
}

static void sj5__discard_until(sj5_Reader *r, int depth) {
    sj5_Value val;
    val.type = SJ5_NULL;
    while (r->depth != depth && val.type != SJ5_ERROR) {
        val = sj5_read(r);
    }
}

bool sj5_iter_array(sj5_Reader *r, sj5_Value arr, sj5_Value *val) {
    sj5__discard_until(r, arr.depth);
    *val = sj5_read(r);
    if (val->type == SJ5_ERROR || val->type == SJ5_END) { return false; }
    return true;
}

bool sj5_iter_object(sj5_Reader *r, sj5_Value obj, sj5_Value *key, sj5_Value *val) {
    sj5__discard_until(r, obj.depth);
    *key = sj5_read(r);
    if (key->type == SJ5_ERROR || key->type == SJ5_END) { return false; }
    *val = sj5_read(r);
    if (val->type == SJ5_END)   { r->error = "unexpected object end"; return false; }
    if (val->type == SJ5_ERROR) { return false; }
    return true;
}

void sj5_location(sj5_Reader *r, int *line, int *col) {
    int ln = 1, cl = 1;
    for (u8 *p = r->data; p != r->cur; p++) {
        if (*p == '\n') { ln++; cl = 0; }
        cl++;
    }
    *line = ln;
    *col = cl;
}

#endif//SJ5_IMPL
