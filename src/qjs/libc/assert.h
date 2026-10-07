#ifndef QJS_ASSERT_H
#define QJS_ASSERT_H

void qjs_assert_fail(const char* expr, const char* file, int line) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : qjs_assert_fail(#x, __FILE__, __LINE__))
#endif

#endif
