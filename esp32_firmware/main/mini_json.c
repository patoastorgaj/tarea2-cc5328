#include "mini_json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 6

typedef struct {
    const char *s;
    int len;
    int pos;
    mj_tok_t *toks;
    int n;
    int max;
} mj_ctx_t;

static void skip_ws(mj_ctx_t *c)
{
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        c->pos++;
    }
}

static int new_tok(mj_ctx_t *c, mj_type_t type, int start, int parent)
{
    if (c->n >= c->max) {
        return MJ_ERR_NOMEM;
    }
    mj_tok_t *t = &c->toks[c->n];
    t->type = type;
    t->start = start;
    t->end = -1;
    t->size = 0;
    t->parent = parent;
    return c->n++;
}

static int parse_value(mj_ctx_t *c, int parent, int depth);

static int parse_string(mj_ctx_t *c, int parent)
{
    // c->pos apunta a la comilla inicial
    c->pos++;
    int idx = new_tok(c, MJ_STRING, c->pos, parent);
    if (idx < 0) {
        return idx;
    }
    while (c->pos < c->len && c->s[c->pos] != '"') {
        unsigned char ch = (unsigned char)c->s[c->pos];
        if (ch < 0x20) {
            return MJ_ERR_INVAL;
        }
        c->pos += (ch == '\\') ? 2 : 1;
    }
    if (c->pos >= c->len) {
        return MJ_ERR_INVAL;
    }
    c->toks[idx].end = c->pos;
    c->pos++;
    return idx;
}

static int parse_primitive(mj_ctx_t *c, int parent)
{
    int start = c->pos;
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            break;
        }
        c->pos++;
    }
    if (c->pos == start) {
        return MJ_ERR_INVAL;
    }
    char first = c->s[start];
    if (!(first == '-' || (first >= '0' && first <= '9') || first == 't' || first == 'f' || first == 'n')) {
        return MJ_ERR_INVAL;
    }
    int idx = new_tok(c, MJ_PRIMITIVE, start, parent);
    if (idx < 0) {
        return idx;
    }
    c->toks[idx].end = c->pos;
    return idx;
}

static int parse_object(mj_ctx_t *c, int parent, int depth)
{
    int idx = new_tok(c, MJ_OBJECT, c->pos, parent);
    if (idx < 0) {
        return idx;
    }
    c->pos++;
    skip_ws(c);
    if (c->pos < c->len && c->s[c->pos] == '}') {
        c->pos++;
        c->toks[idx].end = c->pos;
        return idx;
    }
    while (1) {
        skip_ws(c);
        if (c->pos >= c->len || c->s[c->pos] != '"') {
            return MJ_ERR_INVAL;
        }
        int key = parse_string(c, idx);
        if (key < 0) {
            return key;
        }
        c->toks[idx].size++;
        skip_ws(c);
        if (c->pos >= c->len || c->s[c->pos] != ':') {
            return MJ_ERR_INVAL;
        }
        c->pos++;
        int r = parse_value(c, key, depth + 1);
        if (r < 0) {
            return r;
        }
        c->toks[key].size = 1;
        skip_ws(c);
        if (c->pos >= c->len) {
            return MJ_ERR_INVAL;
        }
        if (c->s[c->pos] == ',') {
            c->pos++;
            continue;
        }
        if (c->s[c->pos] == '}') {
            c->pos++;
            break;
        }
        return MJ_ERR_INVAL;
    }
    c->toks[idx].end = c->pos;
    return idx;
}

static int parse_array(mj_ctx_t *c, int parent, int depth)
{
    int idx = new_tok(c, MJ_ARRAY, c->pos, parent);
    if (idx < 0) {
        return idx;
    }
    c->pos++;
    skip_ws(c);
    if (c->pos < c->len && c->s[c->pos] == ']') {
        c->pos++;
        c->toks[idx].end = c->pos;
        return idx;
    }
    while (1) {
        int r = parse_value(c, idx, depth + 1);
        if (r < 0) {
            return r;
        }
        c->toks[idx].size++;
        skip_ws(c);
        if (c->pos >= c->len) {
            return MJ_ERR_INVAL;
        }
        if (c->s[c->pos] == ',') {
            c->pos++;
            continue;
        }
        if (c->s[c->pos] == ']') {
            c->pos++;
            break;
        }
        return MJ_ERR_INVAL;
    }
    c->toks[idx].end = c->pos;
    return idx;
}

static int parse_value(mj_ctx_t *c, int parent, int depth)
{
    if (depth > MJ_MAX_DEPTH) {
        return MJ_ERR_INVAL;
    }
    skip_ws(c);
    if (c->pos >= c->len) {
        return MJ_ERR_INVAL;
    }
    switch (c->s[c->pos]) {
    case '{':
        return parse_object(c, parent, depth);
    case '[':
        return parse_array(c, parent, depth);
    case '"':
        return parse_string(c, parent);
    default:
        return parse_primitive(c, parent);
    }
}

int mj_parse(const char *js, size_t len, mj_tok_t *toks, int max_toks)
{
    mj_ctx_t c = {.s = js, .len = (int)len, .pos = 0, .toks = toks, .n = 0, .max = max_toks};
    int r = parse_value(&c, -1, 0);
    if (r < 0) {
        return r;
    }
    skip_ws(&c);
    if (c.pos != c.len) {
        return MJ_ERR_INVAL;
    }
    return c.n;
}

bool mj_eq(const char *js, const mj_tok_t *t, const char *s)
{
    if (t->type != MJ_STRING) {
        return false;
    }
    size_t n = (size_t)(t->end - t->start);
    return strlen(s) == n && strncmp(js + t->start, s, n) == 0;
}

int mj_next_child(const mj_tok_t *t, int n, int parent, int from)
{
    for (int i = from + 1; i < n; i++) {
        if (t[i].parent == parent) {
            return i;
        }
    }
    return -1;
}

int mj_find(const char *js, const mj_tok_t *t, int n, int obj, const char *key)
{
    if (obj < 0 || obj >= n || t[obj].type != MJ_OBJECT) {
        return -1;
    }
    for (int k = mj_next_child(t, n, obj, obj); k >= 0; k = mj_next_child(t, n, obj, k)) {
        if (mj_eq(js, &t[k], key) && k + 1 < n) {
            return k + 1;
        }
    }
    return -1;
}

bool mj_number(const char *js, const mj_tok_t *t, double *out)
{
    if (t->type != MJ_PRIMITIVE) {
        return false;
    }
    char buf[32];
    int n = t->end - t->start;
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }
    memcpy(buf, js + t->start, (size_t)n);
    buf[n] = '\0';
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp != buf + n || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool mj_bool(const char *js, const mj_tok_t *t, bool *out)
{
    if (t->type != MJ_PRIMITIVE) {
        return false;
    }
    int n = t->end - t->start;
    if (n == 4 && strncmp(js + t->start, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (n == 5 && strncmp(js + t->start, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}
