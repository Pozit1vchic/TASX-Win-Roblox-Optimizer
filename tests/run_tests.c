/* TASX unit-test runner (config suite). Compile with test_config.c + config.c:
     gcc -IInfra run_tests.c test_config.c ../Infra/config.c -o test_config.exe
   Run from the repository root: `make test`. */

#include "tasx_test.h"

#include <stdio.h>
#include <string.h>

void config_test_suite(void); /* tests/test_config.c */

static int g_pass = 0;
static int g_fail = 0;
static int g_printed = 0;

int test_check(const char* name, int cond, const char* expr, const char* file, int line)
{
    if (cond) {
        ++g_pass;
        return 1;
    }
    ++g_fail;
    if (!g_printed) {
        printf("\nFAILURES:\n");
        g_printed = 1;
    }
    printf("  [%s:%d] %s: %s\n", file, line, name, expr);
    return 0;
}

void test_report(const char* suite)
{
    printf("%-24s %3d passed, %d failed\n", suite, g_pass, g_fail);
}

int main(void)
{
    config_test_suite();
    test_report("config");
    printf("\nTOTAL: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}