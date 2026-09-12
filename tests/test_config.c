/* config.c parser unit tests - mirrors the documented TASX.ini grammar:
   BOM skip, sections, quote-aware inline comments, bool/int coercions,
   case-insensitive keys, hot-reload via config_reload. */

#include "tasx_test.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TASX_TEST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TASX_TEST_MKDIR(p) mkdir((p), 0755)
#endif

static void WriteBytes(const char* path, const char* bytes)
{
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(bytes, 1, strlen(bytes), f);
        fclose(f);
    }
}

void config_test_suite(void)
{
    /* A real FILE inside tests/.build - the folder itself is not an ini, so
       fopen() on it fails on Windows and every check below would silently
       fall back to defaults. Created here so the suite also runs standalone. */
    const char* dir = "tests/.build/test.ini";
    TASX_TEST_MKDIR("tests/.build"); /* EEXIST is fine */

    /* 1. BOM + CRLF + basic section/key/value. */
    WriteBytes(dir, "\xEF\xBB\xBF[TASX]\r\nFoo = bar\r\n");
    config_reload(dir);
    CHECK_EQ_S("BOM+CRLF parses key",
               config_get_str("TASX", "Foo", ""), "bar");

    /* 2. Inline comments: ';' only after whitespace, '#' always. */
    WriteBytes(dir,
               "[TASX]\n"
               "A=val ; trailing comment\n"
               "B=a#b\n"
               "C=\"x;y\" ; quoted semicolon kept\n"
               "D=C:\\A;E:\\B\n"
               "E=no-comment#here\n");
    config_reload(dir);
    CHECK_EQ_S("semicolon comment stripped", config_get_str("TASX", "A", ""), "val");
    CHECK_EQ_S("hash comment stripped",      config_get_str("TASX", "B", ""), "a");
    CHECK_EQ_S("quoted segment preserved",   config_get_str("TASX", "C", ""), "\"x;y\"");
    CHECK_EQ_S("unstripped semicolon (path)",config_get_str("TASX", "D", ""), "C:\\A;E:\\B");
    CHECK_EQ_S("hash in value is always comment", config_get_str("TASX", "E", ""), "no-comment");

    /* 3. Booleans. */
    WriteBytes(dir,
               "[TASX]\n"
               "OnFlag=on\nOffFlag=off\nTFlag=true\nFFlag=false\n"
               "YFlag=yes\nNFlag=no\n1Flag=1\n0Flag=0\n2Flag=2\nGarbageFlag=maybe\n");
    config_reload(dir);
    CHECK_EQ_I("bool on",  config_get_bool("TASX", "OnFlag", 0), 1);
    CHECK_EQ_I("bool off", config_get_bool("TASX", "OffFlag", 1), 0);
    CHECK_EQ_I("bool true",  config_get_bool("TASX", "TFlag", 0), 1);
    CHECK_EQ_I("bool false", config_get_bool("TASX", "FFlag", 1), 0);
    CHECK_EQ_I("bool yes", config_get_bool("TASX", "YFlag", 0), 1);
    CHECK_EQ_I("bool no",  config_get_bool("TASX", "NFlag", 1), 0);
    CHECK_EQ_I("bool 1",   config_get_bool("TASX", "1Flag", 0), 1);
    CHECK_EQ_I("bool 0",   config_get_bool("TASX", "0Flag", 1), 0);
    CHECK_EQ_I("bool 2 -> default", config_get_bool("TASX", "2Flag", 7), 7);
    CHECK_EQ_I("bool garbage -> default", config_get_bool("TASX", "GarbageFlag", 7), 7);

    /* 4. Integers. */
    WriteBytes(dir,
               "[TASX]\n"
               "Pos=123\nNeg=-7\nPlus=+42\nJunk=15abc\nEmpty=\n");
    config_reload(dir);
    CHECK_EQ_I("int pos",  config_get_int("TASX", "Pos", 0), 123);
    CHECK_EQ_I("int neg",  config_get_int("TASX", "Neg", 0), -7);
    CHECK_EQ_I("int plus", config_get_int("TASX", "Plus", 0), 42);
    CHECK_EQ_I("int junk prefix", config_get_int("TASX", "Junk", 0), 15);
    CHECK_EQ_I("int empty -> default", config_get_int("TASX", "Empty", 9), 9);

    /* 5. Case-insensitive section/key, key outside section ignored. */
    WriteBytes(dir,
               "; comment only header\n"
               "[TaSx]\n"
               "FoO=bar\n"
               "[EmptySection]\n"
               "Lonely=orphan\n"
               "after=ignored\n"
               "\n"
               "[Next]\n"
               "Key=value\n");
    config_reload(dir);
    CHECK_EQ_S("section case-insensitive", config_get_str("tasx", "foo", ""), "bar");
    CHECK_EQ_S("missing key default",      config_get_str("TASX", "Nope", "def"), "def");

    /* 6. Hot-reload picks up edits. */
    WriteBytes(dir, "[TASX]\nReloadMe=one\n");
    config_reload(dir);
    CHECK_EQ_S("reload before", config_get_str("TASX", "ReloadMe", ""), "one");
    WriteBytes(dir, "[TASX]\nReloadMe=two\n");
    config_reload(dir);
    CHECK_EQ_S("reload after", config_get_str("TASX", "ReloadMe", ""), "two");

    /* 7. Comment-only file -> not loaded (count == 0). */
    WriteBytes(dir, "; only comments\n# and hashes\n");
    config_reload(dir);
    CHECK_EQ_I("comment-only not loaded", config_loaded(), 0);

    /* 8. Enumeration. */
    WriteBytes(dir, "[Sec]\nA=1\nB=2\n");
    config_reload(dir);
    CHECK_EQ_I("entry count", config_get_entry_count(), 2);
    {
        char s[32], k[64], v[192];
        CHECK("entry 0 readable",
              config_get_entry(0, s, sizeof(s), k, sizeof(k), v, sizeof(v)) == 1 &&
              strcmp(s, "Sec") == 0 && strcmp(k, "A") == 0 && strcmp(v, "1") == 0);
        CHECK("entry OOB returns 0",
              config_get_entry(2, s, sizeof(s), k, sizeof(k), v, sizeof(v)) == 0);
    }

    /* 9. Default creation is idempotent. */
    WriteBytes(dir, "x=y\n");
    CHECK_EQ_I("create_default refuses existing", config_create_default(dir), 0);

    /* 10. Missing file -> defaults, no crash. */
    config_reload("tests/.build/does_not_exist.ini");
    CHECK_EQ_S("missing file default", config_get_str("TASX", "Whatever", "dflt"), "dflt");
    CHECK_EQ_I("missing file not loaded", config_loaded(), 0);

    /* 11. Oversized file: the table is capped and the truncation is
       REPORTED (config_truncated) instead of silently dropping keys. */
    {
        const int maxEntries = config_max_entries();
        CHECK("max entries positive", maxEntries > 0);
        FILE* f = fopen(dir, "wb");
        if (f) {
            fprintf(f, "[Big]\n");
            for (int i = 0; i < maxEntries + 5; ++i)
                fprintf(f, "K%d=%d\n", i, i);
            fclose(f);
        }
        config_reload(dir);
        CHECK_EQ_I("entry count capped", config_get_entry_count(), maxEntries);
        CHECK_EQ_I("truncation flagged", config_truncated(), 1);
        CHECK_EQ_I("accepted key readable", config_get_int("Big", "K0", -1), 0);
    }

    /* 12. A normal reload clears a previous truncation flag. */
    WriteBytes(dir, "[TASX]\nSmall=1\n");
    config_reload(dir);
    CHECK_EQ_I("truncation flag cleared", config_truncated(), 0);
    CHECK_EQ_I("small file loaded", config_get_int("TASX", "Small", 0), 1);
}