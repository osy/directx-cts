#ifndef DXCTS_JSON_H
#define DXCTS_JSON_H

#include <stdio.h>

/* Writer: emits a JSON document incrementally.  Separators are tracked per nesting
 * level so callers never place commas themselves. */

#define JSON_MAX_DEPTH 16

typedef struct
{
    FILE *out;
    int depth;
    int count[JSON_MAX_DEPTH];
} json_writer;

void json_init(json_writer *w, FILE *out);
void json_object_begin(json_writer *w, const char *key);
void json_object_end(json_writer *w);
void json_array_begin(json_writer *w, const char *key);
void json_array_end(json_writer *w);
void json_str(json_writer *w, const char *key, const char *value);
void json_num(json_writer *w, const char *key, double value);
void json_int(json_writer *w, const char *key, long long value);
void json_bool(json_writer *w, const char *key, int value);

/* Reader for one flat JSON object per line, values being strings, numbers or booleans.
 * The catalog uses that shape deliberately so the runner needs no general parser, and a
 * malformed line is reported with its number rather than skipped. */

#define JSON_MAX_FIELDS 24

/* A value longer than this is refused, not cut: an exclusion's case list is one. */
typedef struct
{
    char key[64];
    char value[4096];
} json_field;

typedef struct
{
    json_field fields[JSON_MAX_FIELDS];
    int count;
} json_line;

/* Returns 1 on success, 0 on a malformed line (message written to err). */
int json_parse_line(const char *text, json_line *out, char *err, size_t err_size);

const char *json_get(const json_line *line, const char *key, const char *fallback);
long long json_get_int(const json_line *line, const char *key, long long fallback);
int json_get_bool(const json_line *line, const char *key, int fallback);

#endif
