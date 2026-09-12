#ifndef TASX_TEST_H
#define TASX_TEST_H

/* Minimal self-contained test harness (no external framework - fits the
   project's zero-dependency style). Shared state lives in run_tests.c;
   each suite file (test_*.c) exports a `void xxx_test_suite(void)`. */

#include <string.h> /* the CHECK_EQ_* macros below call strcmp */

/* cond is an int (the macros pass !!(cond) / comparison results); expr is
   the *text* of the expression, printed on failure. */
int  test_check(const char* name, int cond, const char* expr, const char* file, int line);
void test_report(const char* suite);

#define CHECK(name, cond) \
    test_check((name), !!(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ_S(name, a, b) \
    test_check((name), strcmp((a), (b)) == 0, #a " == " #b, __FILE__, __LINE__)
#define CHECK_EQ_I(name, a, b) \
    test_check((name), (long)(a) == (long)(b), #a " == " #b, __FILE__, __LINE__)

#endif /* TASX_TEST_H */