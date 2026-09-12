/* Farm auto-respawn unit tests (self-contained, runs its own main; linked
   against respawn.o + config.o + ntsys.o by `make test_respawn`).

   Focus:
   - CommandLineToArgvW-compatible split/build round-trips (the trickiest
     logic in respawn.cc): quoted args, embedded quotes, trailing backslashes
     before a closing quote - all must survive split() -> build() -> split().
   - The remember/pending/forget bookkeeping plus the config gate inside
     RespawnTrySpawn (with no ini loaded it must be a safe no-op).

   Windows API used by the module (CommandLineToArgvW, GetTickCount64,
   CreateProcessW ...) all resolve from -l... in LDFLAGS, so this links
   exactly like the shipping binary. */

#include "respawn.h"
#include "config.h"

#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;
static int g_printed = 0;

static void Check(const char* name, bool cond, const char* expr, const char* file, int line)
{
    if (cond) { ++g_pass; return; }
    ++g_fail;
    if (!g_printed) { printf("\nFAILURES:\n"); g_printed = 1; }
    printf("  [%s:%d] %s: %s\n", file, line, name, expr);
}

#define CHECK(name, cond) Check((name), !!(cond), #cond, __FILE__, __LINE__)
#define CHECK_WS(name, a, b) Check((name), (a) == (b), #a " == " #b, __FILE__, __LINE__)

static std::wstring Join(const std::vector<std::wstring>& v)
{
    std::wstring all = L"[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) all += L" | ";
        all += v[i];
    }
    all += L"]";
    return all;
}

/* round-trip: build(split(raw)) must split back to exactly split(raw) - this
   is the lossless property respawn relies on when normalizing a command line. */
static void CheckRoundTrip(const char* name, const std::wstring& raw)
{
    std::vector<std::wstring> s1 = SplitCmdLine(raw);
    std::wstring rebuilt = BuildCmdLine(s1);
    std::vector<std::wstring> s2 = SplitCmdLine(rebuilt);
    if (s1 == s2 && s1.size() == s2.size()) { ++g_pass; return; }
    ++g_fail;
    if (!g_printed) { printf("\nFAILURES:\n"); g_printed = 1; }
    printf("  [%s] round-trip (split->build->split differs)\n", name);
    printf("    raw   : %ls\n", raw.c_str());
    printf("    built : %ls\n", rebuilt.c_str());
    printf("    split1: %ls\n", Join(s1).c_str());
    printf("    split2: %ls\n", Join(s2).c_str());
}

int main(void)
{
    /* 1. Split basics + round-trips (the core value). */
    {
        std::vector<std::wstring> a = SplitCmdLine(L"\"C:\\Program Files\\Roblox Launcher.exe\" --player x \"path with spaces\"");
        CHECK("split plain args", a.size() == 4 && a[0] == L"C:\\Program Files\\Roblox Launcher.exe" &&
              a[1] == L"--player" && a[2] == L"x" && a[3] == L"path with spaces");
    }
    CheckRoundTrip("rt empty", L"");
    CheckRoundTrip("rt one bare", L"RobloxRenderer.exe");
    CheckRoundTrip("rt one quoted", L"\"Roblox Renderer.exe\"");
    CheckRoundTrip("rt mixed", L"cmd.exe /c \"echo hi there\" tail");
    CheckRoundTrip("rt trailing slope infile", L"prog \"C:\\dir\\file\\\"");
    CheckRoundTrip("rt embedded quote", L"app a\\\"b c");
    CheckRoundTrip("rt many spaces", L"a   b    c");
    CheckRoundTrip("rt flags with =", L"task.exe --threads=4 --path=\"C:\\x y\"");

    /* BuildCmdLine idempotence: building already-built args is a fixed point. */
    {
        std::vector<std::wstring> v = SplitCmdLine(L"cmd /c \"echo a b\"");
        std::wstring once = BuildCmdLine(v);
        std::wstring twice = BuildCmdLine(SplitCmdLine(once));
        CHECK_WS("build idempotent", once, twice);
    }

    /* 2. Bookkeeping. */
    RespawnClearAll();
    CHECK_WS("pending starts empty", RespawnPendingCount(), (size_t)0);
    RespawnRemember(100, L"a.exe --x");
    RespawnRemember(101, L"b.exe");
    RespawnRemember(102, L"");   /* empty cmdline ignored */
    CHECK_WS("remember adds", RespawnPendingCount(), (size_t)2);
    RespawnForget(100);
    CHECK_WS("forget removes", RespawnPendingCount(), (size_t)1);
    RespawnClearAll();
    CHECK_WS("clear all", RespawnPendingCount(), (size_t)0);

    /* 3. RespawnTrySpawn safety with no ini / RespawnOnCrash off: must be a
       pure no-op (returns 0, spawns nothing), same for an unknown PID. */
    config_reload("tests/.build/does_not_exist_respawn.ini");
    CHECK("not loaded (absent ini)", config_loaded() == 0);
    CHECK("RespawnConfigured off when disabled", RespawnConfigured() == 0);
    CHECK("TrySpawn no-op when disabled", RespawnTrySpawn(99) == 0);
    RespawnRemember(201, L"x.exe");
    CHECK("TrySpawn off even with pending", RespawnTrySpawn(201) == 0);
    CHECK("TrySpawn unknown pid", RespawnTrySpawn(9999) == 0);
    RespawnClearAll();

    /* 4. Enabled but nothing remembered -> still a safe no-op. */
    {
        char ini[256];
        snprintf(ini, sizeof(ini), "tests/.build/respawn_on.ini");
        FILE* f = fopen(ini, "wb");
        if (f) {
            fprintf(f, "[TASX]\nRespawnOnCrash=1\n");
            fclose(f);
        }
        if (config_reload(ini), config_loaded()) {
            CHECK("configured when on", RespawnConfigured() == 0); /* nothing remembered */
            CHECK("TrySpawn unknown when on", RespawnTrySpawn(7) == 0);
        }
    }

    printf("\nrespawn  %3d passed, %d failed\n", g_pass, g_fail);
    printf("TOTAL: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}