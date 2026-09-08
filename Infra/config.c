/* TASX.ini reader — plain C, no CRT-specific helpers so it builds as C
   with both MSVC and MinGW. */
#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define CFG_MAX_ENTRIES 128
#define CFG_SECTION_LEN 32
#define CFG_KEY_LEN     48
#define CFG_VALUE_LEN   128

typedef struct {
    char section[CFG_SECTION_LEN];
    char key[CFG_KEY_LEN];
    char value[CFG_VALUE_LEN];
} CfgEntry;

static CfgEntry g_entries[CFG_MAX_ENTRIES];
static int      g_count     = 0;
static int      g_wasLoaded = 0;

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int ascii_ieq(const char* a, const char* b)
{
    while (*a && *b) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

static void trim_inplace(char* s)
{
    size_t len;
    char* end;

    while (*s == ' ' || *s == '\t') ++s;

    len = strlen(s);
    end = s + len;
    while (end > s && (end[-1] == ' '  || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        --end;
    *end = '\0';
}

static int find_entry(const char* section, const char* key)
{
    int i;
    for (i = 0; i < g_count; ++i) {
        if (ascii_ieq(g_entries[i].section, section) &&
            ascii_ieq(g_entries[i].key, key))
            return i;
    }
    return -1;
}

void config_load(const char* iniPath)
{
    FILE* fp;
    char line[256];
    char curSection[CFG_SECTION_LEN];

    if (g_wasLoaded) return;
    g_wasLoaded = 1;

    fp = fopen(iniPath, "r");
    if (!fp) return;

    curSection[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        char* p = line;

        trim_inplace(p);
        if (p[0] == '\0' || p[0] == ';' || p[0] == '#') continue;

        if (p[0] == '[') {
            char* close = strchr(p, ']');
            size_t len;
            if (!close) continue;
            len = (size_t)(close - p) - 1;
            if (len >= CFG_SECTION_LEN) len = CFG_SECTION_LEN - 1;
            memcpy(curSection, p + 1, len);
            curSection[len] = '\0';
            trim_inplace(curSection);
            continue;
        }
        else {
            char* eq = strchr(p, '=');
            char* key;
            char* val;
            size_t keyLen;

            if (!eq) continue;
            *eq = '\0';
            key = p;
            val = eq + 1;
            trim_inplace(key);
            trim_inplace(val);

            if (key[0] == '\0' || curSection[0] == '\0') continue;

            if (g_count >= CFG_MAX_ENTRIES) break;

            keyLen = strlen(key);
            if (keyLen >= CFG_KEY_LEN) keyLen = CFG_KEY_LEN - 1;
            memcpy(g_entries[g_count].key, key, keyLen);
            g_entries[g_count].key[keyLen] = '\0';

            keyLen = strlen(curSection);
            if (keyLen >= CFG_SECTION_LEN) keyLen = CFG_SECTION_LEN - 1;
            memcpy(g_entries[g_count].section, curSection, keyLen);
            g_entries[g_count].section[keyLen] = '\0';

            keyLen = strlen(val);
            if (keyLen >= CFG_VALUE_LEN) keyLen = CFG_VALUE_LEN - 1;
            memcpy(g_entries[g_count].value, val, keyLen);
            g_entries[g_count].value[keyLen] = '\0';

            ++g_count;
        }
    }

    fclose(fp);
}

void config_default_path(char* out, int outLen)
{
    char exePath[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    DWORD i;

    if (n == 0 || n >= MAX_PATH) {
        snprintf(out, (size_t)outLen, "TASX.ini");
        return;
    }

    for (i = n; i > 0; --i) {
        if (exePath[i - 1] == '\\' || exePath[i - 1] == '/') {
            exePath[i] = '\0';
            break;
        }
    }
    if (i == 0) exePath[0] = '\0';

    snprintf(out, (size_t)outLen, "%sTASX.ini", exePath);
}

int config_loaded(void)
{
    return g_wasLoaded && g_count > 0;
}

const char* config_get_str(const char* section, const char* key, const char* defVal)
{
    int idx = find_entry(section, key);
    if (idx < 0) return defVal;
    return g_entries[idx].value;
}

int config_get_int(const char* section, const char* key, int defVal)
{
    const char* v = config_get_str(section, key, NULL);
    int result = 0;
    int sign = 1;

    if (!v || v[0] == '\0') return defVal;

    if (*v == '-') { sign = -1; ++v; }
    else if (*v == '+') { ++v; }

    while (*v >= '0' && *v <= '9') {
        result = result * 10 + (*v - '0');
        ++v;
    }
    return sign * result;
}

int config_get_bool(const char* section, const char* key, int defVal)
{
    const char* v = config_get_str(section, key, NULL);
    char c;

    if (!v || v[0] == '\0') return defVal;

    c = ascii_lower(v[0]);
    if (c == '0' || c == 'f' || c == 'n' || c == 'o') {
        /* 0, false, no, off */
        if (ascii_ieq(v, "off")) return 0;
        if (c == 'o') return 1; /* "on" */
        return 0;
    }
    if (c == '1' || c == 't' || c == 'y') return 1;
    return defVal;
}
