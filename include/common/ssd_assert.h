#ifndef SSD_ASSERT_H
#define SSD_ASSERT_H

/*
 * Production assertion API. Unlike a unit-test assertion, a failed production
 * invariant is non-recoverable: it records the violation then enters the
 * platform fault path. Recoverable I/O errors must be returned as status codes.
 */
void ssd_assert_fail(const char *expression, const char *file, int line);

#if defined(NDEBUG)
#define SSD_ASSERT(expression) ((void)0)
#else
#define SSD_ASSERT(expression)                                                                    \
    do                                                                                            \
    {                                                                                             \
        if (!(expression))                                                                        \
            ssd_assert_fail(#expression, __FILE__, __LINE__);                                    \
    } while (0)
#endif

#endif /* SSD_ASSERT_H */
