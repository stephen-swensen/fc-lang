#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ======================================================================== */
/* Arena-grown dynamic arrays                                               */
/* The arena cannot realloc in place, so growth allocates a fresh, larger    */
/* block and copies. Old blocks are reclaimed wholesale when the per-message */
/* arena is reset — fine for short-lived LSP messages.                       */
/* ======================================================================== */

static void *arena_grow(Arena *a, void *items, int count, int *cap, size_t elem) {
    if (count < *cap) return items;
    int newcap = *cap ? *cap * 2 : 8;
    void *n = arena_alloc(a, elem * (size_t)newcap);
    if (count > 0) memcpy(n, items, elem * (size_t)count);
    *cap = newcap;
    return n;
}

/* ======================================================================== */
/* Constructors                                                             */
/* ======================================================================== */

static JsonValue *jv(Arena *a, JsonKind k) {
    JsonValue *v = arena_alloc(a, sizeof *v);
    v->kind = k;
    return v;
}

JsonValue *json_null(Arena *a)            { return jv(a, JSON_NULL); }
JsonValue *json_bool(Arena *a, bool b)    { JsonValue *v = jv(a, JSON_BOOL);   v->b = b;   return v; }
JsonValue *json_num(Arena *a, double n)   { JsonValue *v = jv(a, JSON_NUMBER); v->num = n; return v; }

JsonValue *json_strn(Arena *a, const char *s, int len) {
    JsonValue *v = jv(a, JSON_STRING);
    v->str.s = arena_strdup(a, s, len);
    v->str.len = len;
    return v;
}
JsonValue *json_str(Arena *a, const char *s) {
    return json_strn(a, s, s ? (int)strlen(s) : 0);
}

JsonValue *json_array(Arena *a) {
    JsonValue *v = jv(a, JSON_ARRAY);
    v->arr.items = NULL; v->arr.count = 0; v->arr.cap = 0;
    return v;
}
JsonValue *json_object(Arena *a) {
    JsonValue *v = jv(a, JSON_OBJECT);
    v->obj.members = NULL; v->obj.count = 0; v->obj.cap = 0;
    return v;
}

void json_array_push(Arena *a, JsonValue *arr, JsonValue *v) {
    arr->arr.items = arena_grow(a, arr->arr.items, arr->arr.count, &arr->arr.cap,
                                sizeof(JsonValue *));
    arr->arr.items[arr->arr.count++] = v;
}

void json_object_set(Arena *a, JsonValue *obj, const char *key, JsonValue *v) {
    obj->obj.members = arena_grow(a, obj->obj.members, obj->obj.count, &obj->obj.cap,
                                  sizeof(JsonMember));
    JsonMember *m = &obj->obj.members[obj->obj.count++];
    m->key = arena_strdup(a, key, (int)strlen(key));
    m->value = v;
}

/* ======================================================================== */
/* Accessors                                                                */
/* ======================================================================== */

JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->kind != JSON_OBJECT) return NULL;
    for (int i = 0; i < obj->obj.count; i++)
        if (strcmp(obj->obj.members[i].key, key) == 0)
            return obj->obj.members[i].value;
    return NULL;
}

const char *json_get_str(const JsonValue *obj, const char *key) {
    JsonValue *v = json_get(obj, key);
    return (v && v->kind == JSON_STRING) ? v->str.s : NULL;
}

bool json_get_int(const JsonValue *obj, const char *key, long *out) {
    JsonValue *v = json_get(obj, key);
    if (!v || v->kind != JSON_NUMBER) return false;
    *out = (long)v->num;
    return true;
}

JsonValue *json_index(const JsonValue *arr, int i) {
    if (!arr || arr->kind != JSON_ARRAY || i < 0 || i >= arr->arr.count) return NULL;
    return arr->arr.items[i];
}

int json_array_len(const JsonValue *arr) {
    return (arr && arr->kind == JSON_ARRAY) ? arr->arr.count : 0;
}

/* ======================================================================== */
/* Parser                                                                   */
/* ======================================================================== */

typedef struct {
    const char *p;
    const char *end;
    Arena *a;
    bool error;
} Parse;

static void skip_ws(Parse *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static JsonValue *parse_value(Parse *ps);

/* Encode a Unicode code point as UTF-8 into out (>= 4 bytes). Returns count. */
static int utf8_encode(unsigned cp, char *out) {
    if (cp <= 0x7F)      { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF)     { out[0] = (char)(0xC0 | (cp >> 6));
                           out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp <= 0xFFFF)    { out[0] = (char)(0xE0 | (cp >> 12));
                           out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                           out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(Parse *ps) {
    if (ps->end - ps->p < 4) { ps->error = true; return -1; }
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else { ps->error = true; return -1; }
    }
    ps->p += 4;
    return v;
}

/* Parse a JSON string (ps->p points just past the opening quote). Decodes
 * escapes (incl. \uXXXX surrogate pairs) into a fresh, NUL-terminated arena
 * buffer. */
static JsonValue *parse_string(Parse *ps) {
    /* Worst case the decoded form is no longer than the source span. */
    size_t maxn = (size_t)(ps->end - ps->p) + 1;
    char *buf = arena_alloc(ps->a, maxn);
    int n = 0;
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            buf[n] = '\0';
            JsonValue *v = jv(ps->a, JSON_STRING);
            v->str.s = buf; v->str.len = n;
            return v;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) { ps->error = true; return NULL; }
            char e = *ps->p++;
            switch (e) {
                case '"':  buf[n++] = '"';  break;
                case '\\': buf[n++] = '\\'; break;
                case '/':  buf[n++] = '/';  break;
                case 'b':  buf[n++] = '\b'; break;
                case 'f':  buf[n++] = '\f'; break;
                case 'n':  buf[n++] = '\n'; break;
                case 'r':  buf[n++] = '\r'; break;
                case 't':  buf[n++] = '\t'; break;
                case 'u': {
                    int cp = hex4(ps);
                    if (ps->error) return NULL;
                    /* High surrogate: combine with a following \uXXXX low. */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        ps->end - ps->p >= 2 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        int lo = hex4(ps);
                        if (ps->error) return NULL;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    n += utf8_encode((unsigned)cp, buf + n);
                    break;
                }
                default: ps->error = true; return NULL;
            }
        } else {
            buf[n++] = c;
        }
    }
    ps->error = true;   /* unterminated */
    return NULL;
}

static JsonValue *parse_number(Parse *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
    while (ps->p < ps->end) {
        char c = *ps->p;
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
            c == '+' || c == '-') ps->p++;
        else break;
    }
    char tmp[64];
    int len = (int)(ps->p - start);
    if (len <= 0 || len >= (int)sizeof tmp) { ps->error = true; return NULL; }
    memcpy(tmp, start, (size_t)len);
    tmp[len] = '\0';
    return json_num(ps->a, strtod(tmp, NULL));
}

static bool lit(Parse *ps, const char *word) {
    size_t wl = strlen(word);
    if ((size_t)(ps->end - ps->p) < wl || memcmp(ps->p, word, wl) != 0) return false;
    ps->p += wl;
    return true;
}

static JsonValue *parse_array(Parse *ps) {
    JsonValue *arr = json_array(ps->a);
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        JsonValue *v = parse_value(ps);
        if (ps->error) return NULL;
        json_array_push(ps->a, arr, v);
        skip_ws(ps);
        if (ps->p >= ps->end) { ps->error = true; return NULL; }
        char c = *ps->p++;
        if (c == ']') return arr;
        if (c != ',') { ps->error = true; return NULL; }
        skip_ws(ps);
    }
}

static JsonValue *parse_object(Parse *ps) {
    JsonValue *obj = json_object(ps->a);
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') { ps->error = true; return NULL; }
        ps->p++;
        JsonValue *key = parse_string(ps);
        if (ps->error) return NULL;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p++ != ':') { ps->error = true; return NULL; }
        JsonValue *val = parse_value(ps);
        if (ps->error) return NULL;
        /* reuse the slot; key is already arena-owned + NUL-terminated */
        obj->obj.members = arena_grow(ps->a, obj->obj.members, obj->obj.count,
                                      &obj->obj.cap, sizeof(JsonMember));
        obj->obj.members[obj->obj.count].key = key->str.s;
        obj->obj.members[obj->obj.count].value = val;
        obj->obj.count++;
        skip_ws(ps);
        if (ps->p >= ps->end) { ps->error = true; return NULL; }
        char c = *ps->p++;
        if (c == '}') return obj;
        if (c != ',') { ps->error = true; return NULL; }
    }
}

static JsonValue *parse_value(Parse *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { ps->error = true; return NULL; }
    char c = *ps->p;
    switch (c) {
        case '{': ps->p++; return parse_object(ps);
        case '[': ps->p++; return parse_array(ps);
        case '"': ps->p++; return parse_string(ps);
        case 't': return lit(ps, "true")  ? json_bool(ps->a, true)
                                           : (ps->error = true, NULL);
        case 'f': return lit(ps, "false") ? json_bool(ps->a, false)
                                           : (ps->error = true, NULL);
        case 'n': return lit(ps, "null")  ? json_null(ps->a)
                                           : (ps->error = true, NULL);
        default:
            if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return parse_number(ps);
            ps->error = true;
            return NULL;
    }
}

JsonValue *json_parse(Arena *a, const char *text, int len) {
    Parse ps = { .p = text, .end = text + len, .a = a, .error = false };
    JsonValue *v = parse_value(&ps);
    if (ps.error) return NULL;
    return v;
}

/* ======================================================================== */
/* Serializer                                                               */
/* ======================================================================== */

static void sb_put(char **buf, int *len, int *cap, const char *s, int n) {
    if (*len + n > *cap) {
        while (*len + n > *cap) *cap = *cap ? *cap * 2 : 256;
        *buf = realloc(*buf, (size_t)*cap);
    }
    memcpy(*buf + *len, s, (size_t)n);
    *len += n;
}
static void sb_putc(char **buf, int *len, int *cap, char c) {
    sb_put(buf, len, cap, &c, 1);
}

static void emit_string(const char *s, int n, char **buf, int *len, int *cap) {
    sb_putc(buf, len, cap, '"');
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_put(buf, len, cap, "\\\"", 2); break;
            case '\\': sb_put(buf, len, cap, "\\\\", 2); break;
            case '\b': sb_put(buf, len, cap, "\\b", 2);  break;
            case '\f': sb_put(buf, len, cap, "\\f", 2);  break;
            case '\n': sb_put(buf, len, cap, "\\n", 2);  break;
            case '\r': sb_put(buf, len, cap, "\\r", 2);  break;
            case '\t': sb_put(buf, len, cap, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof esc, "\\u%04x", c);
                    sb_put(buf, len, cap, esc, 6);
                } else {
                    /* pass UTF-8 bytes through unescaped */
                    sb_putc(buf, len, cap, (char)c);
                }
        }
    }
    sb_putc(buf, len, cap, '"');
}

void json_serialize(const JsonValue *v, char **buf, int *len, int *cap) {
    if (!v) { sb_put(buf, len, cap, "null", 4); return; }
    switch (v->kind) {
        case JSON_NULL: sb_put(buf, len, cap, "null", 4); break;
        case JSON_BOOL:
            if (v->b) sb_put(buf, len, cap, "true", 4);
            else      sb_put(buf, len, cap, "false", 5);
            break;
        case JSON_NUMBER: {
            char tmp[32];
            double d = v->num;
            /* Integral values print as integers (LSP fields are integers). */
            if (d == (double)(long long)d && d >= -9.2e18 && d <= 9.2e18)
                snprintf(tmp, sizeof tmp, "%lld", (long long)d);
            else
                snprintf(tmp, sizeof tmp, "%.17g", d);
            sb_put(buf, len, cap, tmp, (int)strlen(tmp));
            break;
        }
        case JSON_STRING:
            emit_string(v->str.s, v->str.len, buf, len, cap);
            break;
        case JSON_ARRAY:
            sb_putc(buf, len, cap, '[');
            for (int i = 0; i < v->arr.count; i++) {
                if (i) sb_putc(buf, len, cap, ',');
                json_serialize(v->arr.items[i], buf, len, cap);
            }
            sb_putc(buf, len, cap, ']');
            break;
        case JSON_OBJECT:
            sb_putc(buf, len, cap, '{');
            for (int i = 0; i < v->obj.count; i++) {
                if (i) sb_putc(buf, len, cap, ',');
                emit_string(v->obj.members[i].key,
                            (int)strlen(v->obj.members[i].key), buf, len, cap);
                sb_putc(buf, len, cap, ':');
                json_serialize(v->obj.members[i].value, buf, len, cap);
            }
            sb_putc(buf, len, cap, '}');
            break;
    }
}
