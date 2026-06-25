#pragma once
#include "common.h"

/* Minimal JSON model for the LSP server (src/lsp.c). Everything is arena-
 * allocated from a per-message arena that the server resets between requests,
 * so neither parsing a request nor building a response leaks. Numbers are held
 * as double (every LSP integer — line, character, id — fits exactly under
 * 2^53); the serializer prints integral doubles without a trailing ".0".
 * Strings store an explicit length and are NUL-terminated; they hold the
 * *unescaped* bytes (UTF-8) and are (de)escaped only at the parse/emit edges. */

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    const char *key;        /* interned-into-arena, NUL-terminated */
    JsonValue  *value;
} JsonMember;

struct JsonValue {
    JsonKind kind;
    union {
        bool   b;
        double num;
        struct { const char *s; int len; } str;
        struct { JsonValue **items; int count, cap; } arr;
        struct { JsonMember *members; int count, cap; } obj;
    };
};

/* Parse `text` (length `len`). Returns NULL on malformed input. Tolerates
 * leading/trailing whitespace. */
JsonValue *json_parse(Arena *a, const char *text, int len);

/* Constructors (all arena-allocated). json_str/json_strn copy the bytes. */
JsonValue *json_null(Arena *a);
JsonValue *json_bool(Arena *a, bool b);
JsonValue *json_num(Arena *a, double n);
JsonValue *json_str(Arena *a, const char *s);
JsonValue *json_strn(Arena *a, const char *s, int len);
JsonValue *json_array(Arena *a);
JsonValue *json_object(Arena *a);
void json_array_push(Arena *a, JsonValue *arr, JsonValue *v);
void json_object_set(Arena *a, JsonValue *obj, const char *key, JsonValue *v);

/* Accessors. Object lookups return NULL / false when absent or wrong kind. */
JsonValue  *json_get(const JsonValue *obj, const char *key);
const char *json_get_str(const JsonValue *obj, const char *key);
bool        json_get_int(const JsonValue *obj, const char *key, long *out);
JsonValue  *json_index(const JsonValue *arr, int i);
int         json_array_len(const JsonValue *arr);

/* Serialize compactly, appending bytes to a malloc/realloc-grown buffer.
 * The buf/len/cap trio follows the DA_APPEND convention (pass a zeroed trio the
 * first time). The result is NOT NUL-terminated; use len. Caller frees buf. */
void json_serialize(const JsonValue *v, char **buf, int *len, int *cap);
