/*
 * Minimal test helpers: no framework, exit code = number of failures.
 */

#ifndef ECSDO_TEST_UTIL_H
#define ECSDO_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
                    __LINE__, #cond); \
            test_failures++; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        long long a_ = (long long) (a), b_ = (long long) (b); \
        if (a_ != b_) { \
            fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s (%lld != %lld)\n", \
                    __FILE__, __LINE__, #a, #b, a_, b_); \
            test_failures++; \
        } \
    } while (0)

#define CHECK_STR(a, b) \
    do { \
        const char *a_ = (a), *b_ = (b); \
        if (!a_ || !b_ || strcmp(a_, b_)) { \
            fprintf(stderr, "%s:%d: CHECK_STR failed: %s == %s (\"%s\" != \"%s\")\n", \
                    __FILE__, __LINE__, #a, #b, a_ ? a_ : "(null)", \
                    b_ ? b_ : "(null)"); \
            test_failures++; \
        } \
    } while (0)

#define RUN(fn) \
    do { \
        int before_ = test_failures; \
        fn(); \
        printf("%-44s %s\n", #fn, test_failures == before_ ? "ok" : "FAIL"); \
    } while (0)

#define TEST_RESULT() \
    (printf("%d failure(s)\n", test_failures), test_failures ? 1 : 0)

#endif /* ECSDO_TEST_UTIL_H */
