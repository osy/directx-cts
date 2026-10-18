#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void indent(json_writer *w)
{
    int i;
    fputc('\n', w->out);
    for (i = 0; i < w->depth; i++)
        fputs("  ", w->out);
}

static void separate(json_writer *w, const char *key)
{
    if (w->depth > 0 && w->count[w->depth - 1]++)
        fputc(',', w->out);
    if (w->depth > 0)
        indent(w);
    if (key)
        fprintf(w->out, "\"%s\": ", key);
}

static void write_escaped(FILE *out, const char *s)
{
    fputc('"', out);
    for (; s && *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(out, "\\%c", c);
        else if (c == '\n')
            fputs("\\n", out);
        else if (c == '\r')
            fputs("\\r", out);
        else if (c == '\t')
            fputs("\\t", out);
        else if (c < 0x20)
            fprintf(out, "\\u%04x", c);
        else
            fputc(c, out);
    }
    fputc('"', out);
}

void json_init(json_writer *w, FILE *out)
{
    memset(w, 0, sizeof(*w));
    w->out = out;
    fputc('{', out);
    w->depth = 1;
}

void json_object_begin(json_writer *w, const char *key)
{
    separate(w, key);
    fputc('{', w->out);
    if (w->depth < JSON_MAX_DEPTH)
        w->count[w->depth++] = 0;
}

void json_object_end(json_writer *w)
{
    int had = w->depth > 0 ? w->count[w->depth - 1] : 0;
    if (w->depth > 0)
        w->depth--;
    if (had)
        indent(w);
    fputc('}', w->out);
}

void json_array_begin(json_writer *w, const char *key)
{
    separate(w, key);
    fputc('[', w->out);
    if (w->depth < JSON_MAX_DEPTH)
        w->count[w->depth++] = 0;
}

void json_array_end(json_writer *w)
{
    int had = w->depth > 0 ? w->count[w->depth - 1] : 0;
    if (w->depth > 0)
        w->depth--;
    if (had)
        indent(w);
    fputc(']', w->out);
}

void json_str(json_writer *w, const char *key, const char *value)
{
    separate(w, key);
    if (value)
        write_escaped(w->out, value);
    else
        fputs("null", w->out);
}

void json_num(json_writer *w, const char *key, double value)
{
    separate(w, key);
    fprintf(w->out, "%.3f", value);
}

void json_int(json_writer *w, const char *key, long long value)
{
    separate(w, key);
    fprintf(w->out, "%lld", value);
}

void json_bool(json_writer *w, const char *key, int value)
{
    separate(w, key);
    fputs(value ? "true" : "false", w->out);
}

/* --- reader --- */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *scan_string(const char *p, char *out, size_t cap, int *too_long)
{
    size_t n = 0;
    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\' && *p)
        {
            char esc = *p++;
            switch (esc)
            {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                /* The catalog is ASCII; a \u escape is passed through as '?'. */
                if (strlen(p) >= 4)
                    p += 4;
                c = '?';
                break;
            default: c = esc; break;
            }
        }
        if (n + 1 < cap)
            out[n++] = c;
        else
            *too_long = 1;
    }
    if (*p != '"')
        return NULL;
    out[n] = 0;
    return p + 1;
}

static int fail(char *err, size_t cap, const char *what)
{
    if (err && cap)
        snprintf(err, cap, "%s", what);
    return 0;
}

int json_parse_line(const char *text, json_line *out, char *err, size_t err_size)
{
    const char *p = skip_ws(text);

    memset(out, 0, sizeof(*out));
    if (*p != '{')
        return fail(err, err_size, "expected '{'");
    p = skip_ws(p + 1);
    if (*p == '}')
        return 1;

    for (;;)
    {
        json_field *f;
        int too_long = 0;

        if (out->count >= JSON_MAX_FIELDS)
            return fail(err, err_size, "too many fields");
        f = &out->fields[out->count];

        p = scan_string(skip_ws(p), f->key, sizeof(f->key), &too_long);
        if (!p)
            return fail(err, err_size, "expected a key string");
        p = skip_ws(p);
        if (*p != ':')
            return fail(err, err_size, "expected ':'");
        p = skip_ws(p + 1);

        if (*p == '"')
        {
            p = scan_string(p, f->value, sizeof(f->value), &too_long);
            if (!p)
                return fail(err, err_size, "unterminated string");
        }
        else
        {
            size_t n = 0;
            while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
            {
                if (n + 1 < sizeof(f->value))
                    f->value[n++] = *p;
                else
                    too_long = 1;
                p++;
            }
            f->value[n] = 0;
            if (!n)
                return fail(err, err_size, "empty value");
        }
        if (too_long)
            return fail(err, err_size, "a key or value too long");

        out->count++;
        p = skip_ws(p);
        if (*p == ',')
        {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}')
            return 1;
        return fail(err, err_size, "expected ',' or '}'");
    }
}

static const json_field *json_field_of(const json_line *line, const char *key)
{
    int i;

    for (i = 0; i < line->count; i++)
        if (strcmp(line->fields[i].key, key) == 0)
            return &line->fields[i];
    return NULL;
}

const char *json_get(const json_line *line, const char *key, const char *fallback)
{
    const json_field *f = json_field_of(line, key);
    return f ? f->value : fallback;
}

long long json_get_int(const json_line *line, const char *key, long long fallback)
{
    const json_field *f = json_field_of(line, key);
    return f ? strtoll(f->value, NULL, 10) : fallback;
}

/* A JSON boolean is not a number, so json_get_int reads `true` as 0. */
int json_get_bool(const json_line *line, const char *key, int fallback)
{
    const json_field *f = json_field_of(line, key);

    if (!f)
        return fallback;
    if (!strcmp(f->value, "true"))
        return 1;
    if (!strcmp(f->value, "false"))
        return 0;
    return strtoll(f->value, NULL, 10) != 0;
}
