#pragma once

/*
 * Minimal dependency-free unit-test harness.
 *
 * Each suite is a void function that records checks via the CHECK* macros.
 * main.c runs the suites and prints a summary.
 */

#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_fails;

#define CHECK(cond) do {                                                   \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
        g_fails++;                                                         \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                      \
} while (0)

#define CHECK_EQ_INT(a, b) do {                                            \
    long long _a = (long long)(a), _b = (long long)(b);                    \
    g_checks++;                                                            \
    if (_a != _b) {                                                        \
        g_fails++;                                                         \
        printf("    FAIL %s:%d: %s == %s (%lld != %lld)\n",                \
               __FILE__, __LINE__, #a, #b, _a, _b);                        \
    }                                                                      \
} while (0)

#define CHECK_EQ_STR(a, b) do {                                            \
    const char *_a = (a), *_b = (b);                                       \
    g_checks++;                                                            \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                               \
        g_fails++;                                                         \
        printf("    FAIL %s:%d: \"%s\" != \"%s\"\n",                       \
               __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)");\
    }                                                                      \
} while (0)

void ut_tree(void);
void ut_util(void);
void ut_protocol(void);
void ut_conn(void);
