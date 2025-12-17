#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "greatest/greatest.h"

#include "charpool.h"

TEST test_charpool(void) {
    charpool_t *pool = charpool_new();
    ASSERT(pool != NULL);

    size_t expected_total_used = 0;

    for (size_t n = 2; n < 20; n++) {
        char *str = charpool_alloc(pool, n);

        expected_total_used += n;
        for (size_t i = 0; i < n - 1; i++) {
            str[i] = 'a' + (i % 26);
        }
        str[n - 1] = '\0';
        for (size_t i = 0; i < n - 1; i++) {
            ASSERT_EQ((char)('a' + (i % 26)), str[i]);
        }

        if (n % 5 == 4) {
            expected_total_used -= n;
            charpool_release_size(pool, str, n);
        }
    }

    char *large_str = charpool_alloc(pool, pool->block_size);
    ASSERT(large_str != NULL);
    for (size_t i = 0; i < pool->block_size - 1; i++) {
        large_str[i] = 'a' + (i % 26);
    }
    large_str[pool->block_size - 1] = '\0';
    ASSERT(charpool_release_size(pool, large_str, pool->block_size));

    charpool_destroy(pool);
    PASS();
}

/* Add definitions that need to be in the test runner's main file. */
GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();      /* command-line options, initialization. */

    RUN_TEST(test_charpool);

    GREATEST_MAIN_END();        /* display results */
}