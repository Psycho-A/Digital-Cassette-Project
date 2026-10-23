#include "ini.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define MAX_LINE 512

/* CFG.c
   ----- */
typedef struct {
    char* section;
    char* key;
    char* value;
} CfgEntry;

static CfgEntry* g_entries = NULL;
static size_t    g_count   = 0;

/* ---------- утилиты ---------- */
static char* dupstr(const char* s)
{
    size_t n; char* r;

    if (!s) return NULL;
    n = strlen(s) + 1;
    r = (char*)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}
static int cfg_handler(void* user, const char* section,
                       const char* name, const char* value)
{
    CfgEntry* e;
    (void)user;

    e = (CfgEntry*)realloc(
        g_entries, (g_count + 1) * sizeof(CfgEntry)
    );
    if (!e) return 0;

    g_entries = e;
    g_entries[g_count].section = dupstr(section);
    g_entries[g_count].key     = dupstr(name);
    g_entries[g_count].value   = dupstr(value);
    g_count++;

    return 1;
}
static const char* cfg_find(const char* section, const char* key)
{
    size_t i;
    for (i = 0; i < g_count; ++i) {
        if (strcmp(g_entries[i].section, section) == 0 &&
            strcmp(g_entries[i].key, key) == 0)
            return g_entries[i].value;
    }
    return NULL;
}

/* ---------- API ---------- */
int cfg_load(const char* filename)
{
    cfg_free();
    return ini_parse(filename, cfg_handler, NULL);
}
void cfg_free(void)
{
    size_t i;
    for (i = 0; i < g_count; ++i) {
        free(g_entries[i].section);
        free(g_entries[i].key);
        free(g_entries[i].value);
    }
    free(g_entries);
    g_entries = NULL;
    g_count   = 0;
}

/* -------- reader --------- */
int cfg_get_int(const char* section, const char* key, int def)
{
    const char* v = cfg_find(section, key);
    return v ? atoi(v) : def;
}
double cfg_get_float(const char* section, const char* key, double def)
{
    const char* v = cfg_find(section, key);
    return v ? atof(v) : def;
}
int cfg_get_bool(const char* section, const char* key, int def)
{
    const char* v = cfg_find(section, key);
    if (!v) return def;
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) return 1;
    if (!strcmp(v, "0") || !strcmp(v, "false")|| !strcmp(v, "no"))  return 0;
    return def;
}
char* cfg_get_string(const char* section, const char* key, const char* def)
{
    const char* v = cfg_find(section, key);
    return dupstr(v ? v : def);
}


/* INI.c
   ----- */
static char* lskip(char* s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static char* rstrip(char* s)
{
    char* p = s + strlen(s);
    while (p > s && isspace((unsigned char)p[-1])) --p;
    *p = '\0';
    return s;
}
static char* strip(char* s)
{
    return rstrip(lskip(s));
}
static void cut_inline_comment(char* s)
{
    char* p = s;
    while (*p) {
        if (*p == '#' || *p == ';') {
            *p = '\0'; break;
        }
        p++;
    }
}
/* C-style escape characters */
static void unescape_string(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\') {
            r++;
            if (!*r) break;
            switch (*r) {
                case 'n':  *w++ = '\n'; break;
                case 't':  *w++ = '\t'; break;
                case 'r':  *w++ = '\r'; break;
                case '\\': *w++ = '\\'; break;
                default:
                    /* unknown escape: keep as-is */
                    *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int ini_parse(const char* filename, ini_handler handler, void* user)
{
    FILE* file;
    char line[MAX_LINE];
    char section[64];
    char* start; char* end;
    char* eq; char* key; char* val;

    section[0] = '\0';
    if (!handler) return -1;

    file = fopen(filename, "r");
    if (!file) return -1;

    while (fgets(line, sizeof(line), file)) {
        start = strip(line);

        if (*start == ';' || *start == '#' || *start == '\0')
            continue;

        if (*start == '[') {
            end = strchr(start, ']');
            if (end) {
                *end = '\0';
                strncpy(section, start + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
        } else {
            eq = strchr(start, '=');
            if (eq) {
                *eq = '\0';
                key = strip(start);
                val = strip(eq + 1);
                cut_inline_comment(val);
                val = strip(val);  // comments
                unescape_string(val); // r,n,t
                handler(user, section, key, val);
            }
        }
    }

    fclose(file);
    return 0;
}


/* NEW: read with clamp value
   -------------------------- */
int cfg_read_bool(const char* section, const char* key, int def)
{
    int v = cfg_get_int(section, key, def);
    if (v < 0 || v > 1) {
        fprintf(stderr, "%s: '%s' has invalid boolean value (%d), fixed to %d.\n", section, key, v, (v < 0) ? 0 : 1);
        if (v < 0) v = 0; else v = 1;
    }
    return v;
}
int cfg_read_int(const char* section, const char* key, int def, int min, int max)
{
    int v = cfg_get_int(section, key, def);
    if (v < min || v > max) {
        fprintf(stderr, "%s: '%s' value %d is out of range (%d..%d), clamped.\n", section, key, v, min, max);
        v = (v < min) ? min : max;
    }
    return v;
}
int cfg_read_even(const char* section, const char* key, int def, int min, int max)
{
    int v = cfg_read_int(section, key, def, min, max);
    if (v & 1) {
        fprintf(stderr, "%s: '%s' value %d is odd, rounding to %d.\n", section, key, v, v+1);
        v = (v + 1) & ~1;
    }
    return v;
}
double cfg_read_float(const char* section, const char* key, double def, double min, double max)
{
    double v = cfg_get_float(section, key, def);
    if (v < min || v > max) {
        fprintf(stderr, "%s: '%s' value %.1f is out of range (%.1f..%.1f), clamped.\n", section, key, v, min, max);
        v = (v < min) ? min : max;
    }
    return v;
}
int cfg_read_pow2(const char* section, const char* key, int def, int min, int max)
{
    int pow2_down = 1, pow2_up, old_v;
    int v = cfg_read_int(section, key, def, min, max);

    /* быстрая проверка степени двойки */
    if ((v & (v - 1)) == 0) return v;

    /* найти ближайшие степени двойки */
    while ((pow2_down << 1) <= v) pow2_down <<= 1;
    pow2_up = pow2_down << 1;

    /* выбор округления (tie < вниз, иначе вверх) */
    old_v = v; v = ((old_v - pow2_down) < (pow2_up - old_v)) ? pow2_down : pow2_up;
    fprintf(stderr, "%s: '%s' value %d is not power-of-two, rounding to %d.\n", section, key, old_v, v);

    return v;
}
