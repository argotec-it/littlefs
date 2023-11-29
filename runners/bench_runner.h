/*
 * Runner for littlefs benchmarks
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BENCH_RUNNER_H
#define BENCH_RUNNER_H


// override LFS_TRACE
void bench_trace(const char *fmt, ...);

#define LFS_TRACE_(fmt, ...) \
    bench_trace("%s:%d:trace: " fmt "%s\n", \
        __FILE__, \
        __LINE__, \
        __VA_ARGS__)
#define LFS_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")
#define LFS_EMUBD_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")

// BENCH_START/BENCH_STOP macros measure readed/proged/erased bytes
// through emubd
void bench_start(const char *meas, uintmax_t iter, uintmax_t size);
void bench_stop(const char *meas);

#define BENCH_START(meas, iter, size) \
    bench_start(meas, iter, size)
#define BENCH_STOP(meas) \
    bench_stop(meas)

// BENCH_RESULT/BENCH_FRESULT allow for explicit non-io measurements
void bench_result(const char *meas, uintmax_t iter, uintmax_t size,
        uintmax_t result);
void bench_fresult(const char *meas, uintmax_t iter, uintmax_t size,
        double result);

#define BENCH_RESULT(meas, iter, size, result) \
    bench_result(meas, iter, size, result)
#define BENCH_FRESULT(meas, iter, size, result) \
    bench_fresult(meas, iter, size, result)


// note these are indirectly included in any generated files
#include "bd/lfs_emubd.h"
#include <stdio.h>

// give source a chance to define feature macros
#undef _FEATURES_H
#undef _STDIO_H


// generated bench configurations
struct lfs_config;

enum bench_flags {
    BENCH_INTERNAL  = 0x1,
};
typedef uint8_t bench_flags_t;

typedef struct bench_define {
    intmax_t (*cb)(void *data, size_t i);
    void *data;
    size_t permutations;
} bench_define_t;

struct bench_case {
    const char *name;
    const char *path;
    bench_flags_t flags;

    const bench_define_t *defines;
    size_t permutations;

    bool (*if_)(void);
    void (*run)(struct lfs_config *cfg);
};

struct bench_suite {
    const char *name;
    const char *path;
    bench_flags_t flags;

    const char *const *define_names;
    size_t define_count;

    const struct bench_case *cases;
    size_t case_count;
};

extern const struct bench_suite *const bench_suites[];
extern const size_t bench_suite_count;


// deterministic prng for pseudo-randomness in benches
uint32_t bench_prng(uint32_t *state);

#define BENCH_PRNG(state) bench_prng(state)

// generation of specific permutations of an array for exhaustive benching
size_t bench_factorial(size_t x);
void bench_permutation(size_t i, uint32_t *buffer, size_t size);

#define BENCH_FACTORIAL(x) bench_factorial(x)
#define BENCH_PERMUTATION(i, buffer, size) bench_permutation(i, buffer, size)


// access generated bench defines
intmax_t bench_define(size_t define);

#define BENCH_DEFINE(i) bench_define(i)

// a few preconfigured defines that control how benches run

#define READ_SIZE_i                 0
#define PROG_SIZE_i                 1
#define BLOCK_SIZE_i                2
#define BLOCK_COUNT_i               3
#define DISK_SIZE_i                 4
#define CACHE_SIZE_i                5
#define INLINE_SIZE_i               6
#define SHRUB_SIZE_i                7
#define FRAGMENT_SIZE_i             8
#define CRYSTAL_SIZE_i              9
#define LOOKAHEAD_SIZE_i            10
#define BLOCK_CYCLES_i              11
#define ERASE_VALUE_i               12
#define ERASE_CYCLES_i              13
#define BADBLOCK_BEHAVIOR_i         14
#define POWERLOSS_BEHAVIOR_i        15

#define BENCH_IMPLICIT_DEFINE_COUNT 16

#define READ_SIZE           bench_define(READ_SIZE_i)
#define PROG_SIZE           bench_define(PROG_SIZE_i)
#define BLOCK_SIZE          bench_define(BLOCK_SIZE_i)
#define BLOCK_COUNT         bench_define(BLOCK_COUNT_i)
#define DISK_SIZE           bench_define(DISK_SIZE_i)
#define CACHE_SIZE          bench_define(CACHE_SIZE_i)
#define INLINE_SIZE         bench_define(INLINE_SIZE_i)
#define SHRUB_SIZE          bench_define(SHRUB_SIZE_i)
#define FRAGMENT_SIZE       bench_define(FRAGMENT_SIZE_i)
#define CRYSTAL_SIZE        bench_define(CRYSTAL_SIZE_i)
#define LOOKAHEAD_SIZE      bench_define(LOOKAHEAD_SIZE_i)
#define BLOCK_CYCLES        bench_define(BLOCK_CYCLES_i)
#define ERASE_VALUE         bench_define(ERASE_VALUE_i)
#define ERASE_CYCLES        bench_define(ERASE_CYCLES_i)
#define BADBLOCK_BEHAVIOR   bench_define(BADBLOCK_BEHAVIOR_i)
#define POWERLOSS_BEHAVIOR  bench_define(POWERLOSS_BEHAVIOR_i)

#define BENCH_IMPLICIT_DEFINES \
    /*        name                value (overridable)                      */ \
    BENCH_DEF(READ_SIZE,          1                                         ) \
    BENCH_DEF(PROG_SIZE,          1                                         ) \
    BENCH_DEF(BLOCK_SIZE,         4096                                      ) \
    BENCH_DEF(BLOCK_COUNT,        DISK_SIZE/BLOCK_SIZE                      ) \
    BENCH_DEF(DISK_SIZE,          1024*1024                                 ) \
    BENCH_DEF(CACHE_SIZE,         lfs_max(16, lfs_max(READ_SIZE, PROG_SIZE))) \
    BENCH_DEF(INLINE_SIZE,        BLOCK_SIZE/8                              ) \
    BENCH_DEF(SHRUB_SIZE,         INLINE_SIZE                               ) \
    BENCH_DEF(FRAGMENT_SIZE,      CACHE_SIZE                                ) \
    BENCH_DEF(CRYSTAL_SIZE,       BLOCK_SIZE/8                              ) \
    BENCH_DEF(LOOKAHEAD_SIZE,     16                                        ) \
    BENCH_DEF(BLOCK_CYCLES,       -1                                        ) \
    BENCH_DEF(ERASE_VALUE,        0xff                                      ) \
    BENCH_DEF(ERASE_CYCLES,       0                                         ) \
    BENCH_DEF(BADBLOCK_BEHAVIOR,  LFS_EMUBD_BADBLOCK_PROGERROR              ) \
    BENCH_DEF(POWERLOSS_BEHAVIOR, LFS_EMUBD_POWERLOSS_NOOP                  )

#define BENCH_CFG \
    .read_size          = READ_SIZE,        \
    .prog_size          = PROG_SIZE,        \
    .block_size         = BLOCK_SIZE,       \
    .block_count        = BLOCK_COUNT,      \
    .block_cycles       = BLOCK_CYCLES,     \
    .cache_size         = CACHE_SIZE,       \
    .inline_size        = INLINE_SIZE,      \
    .shrub_size         = SHRUB_SIZE,       \
    .fragment_size      = FRAGMENT_SIZE,    \
    .crystal_size       = CRYSTAL_SIZE,     \
    .lookahead_size     = LOOKAHEAD_SIZE,

#define BENCH_BDCFG \
    .erase_value        = ERASE_VALUE,          \
    .erase_cycles       = ERASE_CYCLES,         \
    .badblock_behavior  = BADBLOCK_BEHAVIOR,


#endif
