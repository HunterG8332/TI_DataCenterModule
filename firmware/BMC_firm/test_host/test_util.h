#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Tiny test harness: CHECK records a failure and keeps going; main() returns
 * non-zero if any check failed. */
extern int g_checks, g_failures;
extern const char *g_suite;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        g_checks++;                                                                       \
        if (!(cond)) {                                                                    \
            g_failures++;                                                                 \
            printf("  FAIL [%s] %s:%d: %s\n", g_suite, __FILE__, __LINE__, #cond);        \
        }                                                                                 \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (double)(tol))
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)
#define SUITE(name) do { g_suite = name; printf("suite: %s\n", name); } while (0)

void run_safety_tests(void);
void run_core_tests(void);
void run_brain_tests(void);
