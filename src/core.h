#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define UNREACHABLE(msg)                                             \
    do {                                                             \
        fprintf(stderr,                                              \
                "%s:%u: execution reached a UNREACHABLE line: %s\n", \
                __FILE__, __LINE__, (msg));                          \
        exit(52);                                                    \
    } while (0)
// CORE_ prefix: tests include greatest.h, which owns ASSERT.
#define CORE_ASSERT(expr, msg)                                     \
    do {                                                           \
        if (!(expr)) {                                             \
            fprintf(stderr,                                        \
                    "%s:%u: failed assertion (" #expr "): %s\n",   \
                    __FILE__, __LINE__, (msg));                    \
            exit(1);                                               \
        }                                                          \
    } while (0)

#define NANOSECONDS_IN_MILLI 1000000

#define U8_MAX UINT8_MAX
#define U16_MAX UINT16_MAX
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX

#define I8_MAX INT8_MAX
#define I16_MAX INT16_MAX
#define I32_MAX INT32_MAX
#define I64_MAX INT64_MAX

#define I8_MIN INT8_MIN
#define I16_MIN INT16_MIN

// core types
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef intptr_t isize;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uintptr_t usize;

typedef float f32;
typedef double f64;
// core types

// ---------- String
/**
 * A string structure that combines length information with character data.
 * This structure provides a safer way to handle strings by storing both
 * the string content and its length together, avoiding reliance on
 * null-terminated strings.
 *
 * The structure contains:
 * - `len`: The length of the string in bytes
 * - `str`: Pointer to the character array (may not be null-terminated)
 *
 * @see CL_stringNew()
 * @see CL_stringNewC()
 */
typedef struct String {
    usize len;
    const char *data;
} String;

#include <assert.h>
#include <string.h>

/**
 * Creates a new String with specified length and content.
 * A more elaborate description would go here explaining the string structure.
 * @param len Length of the string (must be greater than 0)
 * @param str Pointer to the character array
 * @see mcl_stringNewC()
 * @return A new String struct containing the provided string data
 */
static inline String mclStringNew(usize len, const char *str) {
    assert(len > 0);
    String s = {len, str};
    return (s);
}

/**
 * Creates a new String from a C-style null-terminated string.
 * Automatically calculates the string length using strlen().
 * It does not make a copy of the provided string, it just reference it.
 * @param str Null-terminated C string to convert
 * @see mcl_stringNew()
 * @return A new String struct containing the provided string data
 */
static inline String mclStringNewC(const char *str) {
    usize len = strlen(str);
    String s = {len, str};
    return s;
}

static inline void mclPrintString(String str) {
    printf("%.*s", (u32)str.len, str.data);
}

// ---------- String

// ---------- Slices

typedef struct {
    void *start;
    u32 len;
} Slice;

// ---------- Slices

__attribute__((format(printf, 2, 3)))
static inline void mclExitMsg(u32 exit_code, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    exit((int)exit_code);
}

// ---------- Utils
