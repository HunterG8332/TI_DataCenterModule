#include "test_util.h"

int g_checks, g_failures;
const char *g_suite = "";

int main(void)
{
    run_safety_tests();
    run_core_tests();
    run_brain_tests();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
