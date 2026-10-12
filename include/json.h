#ifndef NETRESCUE_JSON_H
#define NETRESCUE_JSON_H

/* Minimal JSON reader/writer used by the browser-facing API (no external dependencies). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { NR_JSON_NULL, NR_JSON_BOOL, NR_JSON_NUMBER, NR_JSON_STRING, NR_JSON_ARRAY, NR_JSON_OBJECT } NrJsonType;

typedef struct NrJson {
    NrJsonType type;
    bool boolean;
    double number;
    char *string;            /* NR_JSON_STRING */
    struct NrJson *items;    /* array elements or object values */
    char **keys;             /* object keys (parallel to items) */
    size_t count;
} NrJson;

NrJson *nr_json_parse(const char *text, char *error, size_t error_size);
void nr_json_free(NrJson *value);
const NrJson *nr_json_get(const NrJson *object, const char *key);
const NrJson *nr_json_at(const NrJson *array, size_t index);
bool nr_json_has(const NrJson *object, const char *key);
double nr_json_number(const NrJson *value, double fallback);
bool nr_json_bool(const NrJson *value, bool fallback);
const char *nr_json_string(const NrJson *value, const char *fallback);

typedef struct {
    char *data;
    size_t length, capacity;
    bool ok, after_key;
    int depth;
    bool need_comma[64];
} NrJsonWriter;

void jw_init(NrJsonWriter *w);
void jw_free(NrJsonWriter *w);
char *jw_take(NrJsonWriter *w); /* returns the malloc'ed text and resets the writer */
void jw_begin_object(NrJsonWriter *w);
void jw_end_object(NrJsonWriter *w);
void jw_begin_array(NrJsonWriter *w);
void jw_end_array(NrJsonWriter *w);
void jw_key(NrJsonWriter *w, const char *key);
void jw_number(NrJsonWriter *w, double value); /* non-finite values are written as null */
void jw_int(NrJsonWriter *w, long long value);
void jw_uint(NrJsonWriter *w, unsigned long long value);
void jw_bool(NrJsonWriter *w, bool value);
void jw_null(NrJsonWriter *w);
void jw_string(NrJsonWriter *w, const char *value);
void jw_raw(NrJsonWriter *w, const char *json);
void jw_kv_number(NrJsonWriter *w, const char *key, double value);
void jw_kv_opt(NrJsonWriter *w, const char *key, bool present, double value); /* null when !present */
void jw_kv_int(NrJsonWriter *w, const char *key, long long value);
void jw_kv_uint(NrJsonWriter *w, const char *key, unsigned long long value);
void jw_kv_bool(NrJsonWriter *w, const char *key, bool value);
void jw_kv_string(NrJsonWriter *w, const char *key, const char *value);

#endif
