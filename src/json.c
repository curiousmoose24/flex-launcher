#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "json.h"

// A minimal JSON reader: finds a string value by a dot-separated path, such as
// "data.0.path" (the "path" of the first element of the "data" array). A "*" path
// component picks a random element of an array, e.g. "data.*.path".

static const char *skip_whitespace(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

// Skips a string starting at its opening quote; returns the position after the closing quote
static const char *skip_string(const char *p)
{
    p++;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && p[1] != '\0')
            p++;
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

#define MAX_DEPTH 64 // Deeper nesting is treated as invalid, so a hostile response can't exhaust the stack

// Skips any value; returns the position after it, or NULL on invalid JSON
static const char *skip_value(const char *p, int depth)
{
    p = skip_whitespace(p);
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        if (depth >= MAX_DEPTH)
            return NULL;
        char close = *p == '{' ? '}' : ']';
        p = skip_whitespace(p + 1);
        if (*p == close)
            return p + 1;
        while (p != NULL && *p != '\0') {
            if (close == '}') {
                if (*p != '"' || (p = skip_string(p)) == NULL)
                    return NULL;
                p = skip_whitespace(p);
                if (*p != ':')
                    return NULL;
                p++;
            }
            if ((p = skip_value(p, depth + 1)) == NULL)
                return NULL;
            p = skip_whitespace(p);
            if (*p == close)
                return p + 1;
            if (*p != ',')
                return NULL;
            p = skip_whitespace(p + 1);
        }
        return NULL;
    }
    // Number, true, false or null
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
        p++;
    return p;
}

// Finds the member of an object with the given key; returns the start of its value
static const char *find_key(const char *p, const char *key, size_t key_length)
{
    p = skip_whitespace(p);
    if (*p != '{')
        return NULL;
    p = skip_whitespace(p + 1);
    while (*p == '"') {
        const char *name = p + 1;
        const char *end = skip_string(p);
        if (end == NULL)
            return NULL;
        bool match = (size_t) (end - 1 - name) == key_length && !strncmp(name, key, key_length);
        p = skip_whitespace(end);
        if (*p != ':')
            return NULL;
        p = skip_whitespace(p + 1);
        if (match)
            return p;
        if ((p = skip_value(p, 0)) == NULL)
            return NULL;
        p = skip_whitespace(p);
        if (*p != ',')
            return NULL;
        p = skip_whitespace(p + 1);
    }
    return NULL;
}

// Finds an element of an array by index (or a random element for index < 0)
static const char *find_index(const char *p, int index)
{
    p = skip_whitespace(p);
    if (*p != '[')
        return NULL;
    const char *start = p;
    if (index < 0) {
        int count = 0;
        p = skip_whitespace(p + 1);
        while (*p != ']' && *p != '\0') {
            if ((p = skip_value(p, 0)) == NULL)
                return NULL;
            count++;
            p = skip_whitespace(p);
            if (*p == ',')
                p = skip_whitespace(p + 1);
        }
        if (count == 0)
            return NULL;
        index = rand() % count;
    }
    p = skip_whitespace(start + 1);
    for (int i = 0; i < index; i++) {
        if (*p == ']' || (p = skip_value(p, 0)) == NULL)
            return NULL;
        p = skip_whitespace(p);
        if (*p != ',')
            return NULL;
        p = skip_whitespace(p + 1);
    }
    return *p == ']' ? NULL : p;
}

// A function to get a string value by path; returns a newly allocated string or NULL
char *json_get_string(const char *json, const char *path)
{
    const char *p = json;
    const char *component = path;
    while (p != NULL && *component != '\0') {
        const char *end = strchr(component, '.');
        size_t length = end != NULL ? (size_t) (end - component) : strlen(component);
        if (length == 1 && component[0] == '*')
            p = find_index(p, -1);
        else if (length > 0 && strspn(component, "0123456789") >= length)
            p = find_index(p, atoi(component));
        else
            p = find_key(p, component, length);
        component += length;
        if (*component == '.')
            component++;
    }
    if (p == NULL)
        return NULL;
    p = skip_whitespace(p);
    if (*p != '"')
        return NULL;

    // Copy the string, decoding the simple escapes used in URLs
    const char *end = skip_string(p);
    if (end == NULL)
        return NULL;
    char *out = malloc((size_t) (end - p));
    size_t n = 0;
    for (const char *c = p + 1; c < end - 1; c++) {
        if (*c == '\\' && c + 1 < end - 1) {
            c++;
            switch (*c) {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': out[n++] = '\r'; break;
                case 'b': out[n++] = '\b'; break;
                case 'f': out[n++] = '\f'; break;
                case 'u': // Only ASCII \u00XX escapes are decoded, which covers URLs
                    if (c + 4 < end - 1 && c[1] == '0' && c[2] == '0') {
                        char hex[3] = {c[3], c[4], '\0'};
                        out[n++] = (char) strtol(hex, NULL, 16);
                        c += 4;
                    }
                    break;
                default: out[n++] = *c; break; // \" \\ \/
            }
        }
        else
            out[n++] = *c;
    }
    out[n] = '\0';
    return out;
}
