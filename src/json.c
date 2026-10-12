#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------ */
/* Reader: recursive descent with a depth limit.                                              */
/* ------------------------------------------------------------------------------------------ */
typedef struct { const char *p; char *error; size_t error_size; int depth; } Parser;

static void parse_error(Parser *ps, const char *message) {
    if (ps->error != NULL && ps->error_size > 0U && ps->error[0] == '\0') (void)snprintf(ps->error, ps->error_size, "%s", message);
}

static void skip_ws(Parser *ps) { while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ++ps->p; }

static bool parse_value(Parser *ps, NrJson *out);

static bool append_char(char **buffer, size_t *length, size_t *capacity, char c) {
    if (*length + 1U >= *capacity) {
        const size_t grown = *capacity * 2U + 16U;
        char *next = realloc(*buffer, grown);
        if (next == NULL) return false;
        *buffer = next; *capacity = grown;
    }
    (*buffer)[(*length)++] = c; (*buffer)[*length] = '\0';
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *parse_string_raw(Parser *ps) {
    if (*ps->p != '"') { parse_error(ps, "expected string"); return NULL; }
    ++ps->p;
    size_t length = 0U, capacity = 16U; char *out = malloc(capacity);
    if (out == NULL) return NULL;
    out[0] = '\0';
    while (*ps->p != '"') {
        char c = *ps->p;
        if (c == '\0' || (unsigned char)c < 0x20U) { free(out); parse_error(ps, "unterminated string"); return NULL; }
        if (c == '\\') {
            ++ps->p; c = *ps->p;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break; case 'n': c = '\n'; break;
            case 'r': c = '\r'; break; case 't': c = '\t'; break;
            case 'u': {
                unsigned code = 0U;
                for (int i = 1; i <= 4; ++i) { const int h = hex_value(ps->p[i]); if (h < 0) { free(out); parse_error(ps, "bad unicode escape"); return NULL; } code = code * 16U + (unsigned)h; }
                ps->p += 4;
                /* Encode the BMP code point as UTF-8 (surrogate pairs are passed through individually). */
                bool ok;
                if (code < 0x80U) ok = append_char(&out, &length, &capacity, (char)code);
                else if (code < 0x800U) ok = append_char(&out, &length, &capacity, (char)(0xC0U | (code >> 6U))) && append_char(&out, &length, &capacity, (char)(0x80U | (code & 0x3FU)));
                else ok = append_char(&out, &length, &capacity, (char)(0xE0U | (code >> 12U))) && append_char(&out, &length, &capacity, (char)(0x80U | ((code >> 6U) & 0x3FU))) && append_char(&out, &length, &capacity, (char)(0x80U | (code & 0x3FU)));
                if (!ok) { free(out); return NULL; }
                ++ps->p;
                continue;
            }
            default: free(out); parse_error(ps, "bad escape"); return NULL;
            }
        }
        if (!append_char(&out, &length, &capacity, c)) { free(out); return NULL; }
        ++ps->p;
    }
    ++ps->p;
    return out;
}

static bool parse_container(Parser *ps, NrJson *out, bool object) {
    const char close = object ? '}' : ']';
    ++ps->p;
    out->type = object ? NR_JSON_OBJECT : NR_JSON_ARRAY;
    size_t capacity = 0U;
    skip_ws(ps);
    if (*ps->p == close) { ++ps->p; return true; }
    for (;;) {
        if (out->count == capacity) {
            capacity = capacity == 0U ? 4U : capacity * 2U;
            NrJson *items = realloc(out->items, capacity * sizeof(*items));
            if (items == NULL) return false;
            out->items = items;
            if (object) { char **keys = realloc(out->keys, capacity * sizeof(*keys)); if (keys == NULL) return false; out->keys = keys; }
        }
        skip_ws(ps);
        if (object) {
            char *key = parse_string_raw(ps);
            if (key == NULL) return false;
            out->keys[out->count] = key;
            skip_ws(ps);
            if (*ps->p != ':') { memset(&out->items[out->count], 0, sizeof(NrJson)); ++out->count; parse_error(ps, "expected ':'"); return false; }
            ++ps->p;
        }
        NrJson *item = &out->items[out->count];
        memset(item, 0, sizeof(*item));
        ++out->count;
        if (!parse_value(ps, item)) return false;
        skip_ws(ps);
        if (*ps->p == ',') { ++ps->p; continue; }
        if (*ps->p == close) { ++ps->p; return true; }
        parse_error(ps, object ? "expected ',' or '}'" : "expected ',' or ']'");
        return false;
    }
}

static bool parse_value(Parser *ps, NrJson *out) {
    if (++ps->depth > 48) { parse_error(ps, "nesting too deep"); return false; }
    skip_ws(ps);
    bool ok = true;
    const char c = *ps->p;
    if (c == '{' || c == '[') ok = parse_container(ps, out, c == '{');
    else if (c == '"') { out->type = NR_JSON_STRING; out->string = parse_string_raw(ps); ok = out->string != NULL; }
    else if (strncmp(ps->p, "true", 4) == 0) { out->type = NR_JSON_BOOL; out->boolean = true; ps->p += 4; }
    else if (strncmp(ps->p, "false", 5) == 0) { out->type = NR_JSON_BOOL; out->boolean = false; ps->p += 5; }
    else if (strncmp(ps->p, "null", 4) == 0) { out->type = NR_JSON_NULL; ps->p += 4; }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end = NULL;
        out->type = NR_JSON_NUMBER; out->number = strtod(ps->p, &end);
        if (end == ps->p || !isfinite(out->number)) { parse_error(ps, "bad number"); ok = false; } else ps->p = end;
    } else { parse_error(ps, "unexpected character"); ok = false; }
    --ps->depth;
    return ok;
}

static void free_contents(NrJson *v) {
    if (v == NULL) return;
    free(v->string);
    for (size_t i = 0; i < v->count; ++i) { free_contents(&v->items[i]); if (v->keys != NULL) free(v->keys[i]); }
    free(v->items); free(v->keys);
}

void nr_json_free(NrJson *v) { if (v != NULL) { free_contents(v); free(v); } }

NrJson *nr_json_parse(const char *text, char *error, size_t error_size) {
    if (error != NULL && error_size > 0U) error[0] = '\0';
    if (text == NULL) return NULL;
    Parser ps = {text, error, error_size, 0};
    NrJson *root = calloc(1U, sizeof(*root));
    if (root == NULL) return NULL;
    if (!parse_value(&ps, root)) { nr_json_free(root); parse_error(&ps, "invalid JSON"); return NULL; }
    skip_ws(&ps);
    if (*ps.p != '\0') { nr_json_free(root); parse_error(&ps, "trailing characters after JSON value"); return NULL; }
    return root;
}

const NrJson *nr_json_get(const NrJson *o, const char *key) {
    if (o == NULL || o->type != NR_JSON_OBJECT || key == NULL) return NULL;
    for (size_t i = 0; i < o->count; ++i) if (strcmp(o->keys[i], key) == 0) return &o->items[i];
    return NULL;
}
const NrJson *nr_json_at(const NrJson *a, size_t i) { return a == NULL || a->type != NR_JSON_ARRAY || i >= a->count ? NULL : &a->items[i]; }
bool nr_json_has(const NrJson *o, const char *key) { const NrJson *v = nr_json_get(o, key); return v != NULL && v->type != NR_JSON_NULL; }
double nr_json_number(const NrJson *v, double fallback) {
    if (v == NULL) return fallback;
    if (v->type == NR_JSON_NUMBER) return v->number;
    if (v->type == NR_JSON_BOOL) return v->boolean ? 1.0 : 0.0;
    return fallback;
}
bool nr_json_bool(const NrJson *v, bool fallback) {
    if (v == NULL) return fallback;
    if (v->type == NR_JSON_BOOL) return v->boolean;
    if (v->type == NR_JSON_NUMBER) return v->number != 0.0;
    return fallback;
}
const char *nr_json_string(const NrJson *v, const char *fallback) { return v != NULL && v->type == NR_JSON_STRING ? v->string : fallback; }

/* ------------------------------------------------------------------------------------------ */
/* Writer                                                                                     */
/* ------------------------------------------------------------------------------------------ */
void jw_init(NrJsonWriter *w) {
    memset(w, 0, sizeof(*w));
    w->capacity = 1024U; w->data = malloc(w->capacity); w->ok = w->data != NULL;
    if (w->ok) w->data[0] = '\0';
}
void jw_free(NrJsonWriter *w) { free(w->data); memset(w, 0, sizeof(*w)); }
char *jw_take(NrJsonWriter *w) {
    if (!w->ok) { jw_free(w); return NULL; }
    char *text = w->data; w->data = NULL; jw_free(w);
    return text;
}

static void put(NrJsonWriter *w, const char *text, size_t length) {
    if (!w->ok) return;
    if (w->length + length + 1U > w->capacity) {
        size_t capacity = w->capacity;
        while (w->length + length + 1U > capacity) capacity *= 2U;
        char *grown = realloc(w->data, capacity);
        if (grown == NULL) { w->ok = false; return; }
        w->data = grown; w->capacity = capacity;
    }
    memcpy(w->data + w->length, text, length);
    w->length += length; w->data[w->length] = '\0';
}
static void puts_raw(NrJsonWriter *w, const char *text) { put(w, text, strlen(text)); }

static void before_value(NrJsonWriter *w) {
    if (w->after_key) { w->after_key = false; return; }
    if (w->depth > 0 && w->depth < 64) {
        if (w->need_comma[w->depth]) put(w, ",", 1U);
        w->need_comma[w->depth] = true;
    }
}

void jw_begin_object(NrJsonWriter *w) { before_value(w); put(w, "{", 1U); if (++w->depth < 64) w->need_comma[w->depth] = false; }
void jw_end_object(NrJsonWriter *w) { --w->depth; put(w, "}", 1U); }
void jw_begin_array(NrJsonWriter *w) { before_value(w); put(w, "[", 1U); if (++w->depth < 64) w->need_comma[w->depth] = false; }
void jw_end_array(NrJsonWriter *w) { --w->depth; put(w, "]", 1U); }

void jw_string(NrJsonWriter *w, const char *value) {
    before_value(w);
    if (value == NULL) { puts_raw(w, "null"); return; }
    put(w, "\"", 1U);
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        char escaped[8];
        switch (*p) {
        case '"': puts_raw(w, "\\\""); break;
        case '\\': puts_raw(w, "\\\\"); break;
        case '\n': puts_raw(w, "\\n"); break;
        case '\r': puts_raw(w, "\\r"); break;
        case '\t': puts_raw(w, "\\t"); break;
        default:
            if (*p < 0x20U) { (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p); puts_raw(w, escaped); }
            else put(w, (const char *)p, 1U);
        }
    }
    put(w, "\"", 1U);
}

void jw_key(NrJsonWriter *w, const char *key) { jw_string(w, key); put(w, ":", 1U); w->after_key = true; }
void jw_number(NrJsonWriter *w, double value) {
    before_value(w);
    if (!isfinite(value)) { puts_raw(w, "null"); return; }
    char text[40]; (void)snprintf(text, sizeof(text), "%.10g", value); puts_raw(w, text);
}
void jw_int(NrJsonWriter *w, long long value) { before_value(w); char text[32]; (void)snprintf(text, sizeof(text), "%lld", value); puts_raw(w, text); }
void jw_uint(NrJsonWriter *w, unsigned long long value) { before_value(w); char text[32]; (void)snprintf(text, sizeof(text), "%llu", value); puts_raw(w, text); }
void jw_bool(NrJsonWriter *w, bool value) { before_value(w); puts_raw(w, value ? "true" : "false"); }
void jw_null(NrJsonWriter *w) { before_value(w); puts_raw(w, "null"); }
void jw_raw(NrJsonWriter *w, const char *json) { before_value(w); puts_raw(w, json); }
void jw_kv_number(NrJsonWriter *w, const char *k, double v) { jw_key(w, k); jw_number(w, v); }
void jw_kv_opt(NrJsonWriter *w, const char *k, bool present, double v) { jw_key(w, k); if (present) jw_number(w, v); else jw_null(w); }
void jw_kv_int(NrJsonWriter *w, const char *k, long long v) { jw_key(w, k); jw_int(w, v); }
void jw_kv_uint(NrJsonWriter *w, const char *k, unsigned long long v) { jw_key(w, k); jw_uint(w, v); }
void jw_kv_bool(NrJsonWriter *w, const char *k, bool v) { jw_key(w, k); jw_bool(w, v); }
void jw_kv_string(NrJsonWriter *w, const char *k, const char *v) { jw_key(w, k); jw_string(w, v); }
