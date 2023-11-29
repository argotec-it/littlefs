/*
 * Runner for littlefs benchmarks
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "runners/bench_runner.h"
#include "bd/lfs_emubd.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <execinfo.h>
#include <time.h>


// some helpers

// append to an array with amortized doubling
void *mappend(void **p,
        size_t size,
        size_t *count,
        size_t *capacity) {
    uint8_t *p_ = *p;
    size_t count_ = *count;
    size_t capacity_ = *capacity;

    count_ += 1;
    if (count_ > capacity_) {
        capacity_ = (2*capacity_ < 4) ? 4 : 2*capacity_;

        p_ = realloc(p_, capacity_*size);
        if (!p_) {
            return NULL;
        }
    }

    *p = p_;
    *count = count_;
    *capacity = capacity_;
    return &p_[(count_-1)*size];
}

// a quick self-terminating text-safe varint scheme
static void leb16_print(uintmax_t x) {
    // allow 'w' to indicate negative numbers
    if ((intmax_t)x < 0) {
        printf("w");
        x = -x;
    }

    while (true) {
        char nibble = (x & 0xf) | (x > 0xf ? 0x10 : 0);
        printf("%c", (nibble < 10) ? '0'+nibble : 'a'+nibble-10);
        if (x <= 0xf) {
            break;
        }
        x >>= 4;
    }
}

static uintmax_t leb16_parse(const char *s, char **tail) {
    bool neg = false;
    uintmax_t x = 0;
    if (tail) {
        *tail = (char*)s;
    }

    if (s[0] == 'w') {
        neg = true;
        s = s+1;
    }

    size_t i = 0;
    while (true) {
        uintmax_t nibble = s[i];
        if (nibble >= '0' && nibble <= '9') {
            nibble = nibble - '0';
        } else if (nibble >= 'a' && nibble <= 'v') {
            nibble = nibble - 'a' + 10;
        } else {
            // invalid?
            return 0;
        }

        x |= (nibble & 0xf) << (4*i);
        i += 1;
        if (!(nibble & 0x10)) {
            s = s + i;
            break;
        }
    }

    if (tail) {
        *tail = (char*)s;
    }
    return neg ? -x : x;
}



// bench_runner types

typedef struct bench_id {
    const char *name;
    const bench_define_t *defines;
    size_t define_count;
} bench_id_t;


// bench define management
typedef struct bench_define_map {
    const bench_define_t *defines;
    size_t count;
} bench_define_map_t;

typedef struct bench_define_names {
    const char *const *names;
    size_t count;
} bench_define_names_t;

intmax_t bench_define_lit(void *data, size_t i) {
    (void)i;
    return (intptr_t)data;
}

#define BENCH_CONST(x) {bench_define_lit, (void*)(uintptr_t)(x), 1}
#define BENCH_LIT(x) ((bench_define_t)BENCH_CONST(x))


#define BENCH_DEF(k, v) \
    intmax_t bench_define_##k(void *data, size_t i) { \
        (void)data; \
        (void)i; \
        return v; \
    }

    BENCH_IMPLICIT_DEFINES
#undef BENCH_DEF

#define BENCH_DEFINE_MAP_OVERRIDE    0
#define BENCH_DEFINE_MAP_EXPLICIT    1
#define BENCH_DEFINE_MAP_CASE        2
#define BENCH_DEFINE_MAP_IMPLICIT    3

#define BENCH_DEFINE_MAP_COUNT       4

bench_define_map_t bench_define_maps[BENCH_DEFINE_MAP_COUNT] = {
    [BENCH_DEFINE_MAP_IMPLICIT] = {
        (const bench_define_t[BENCH_IMPLICIT_DEFINE_COUNT]) {
            #define BENCH_DEF(k, v) \
                [k##_i] = {bench_define_##k, NULL, 1},

                BENCH_IMPLICIT_DEFINES
            #undef BENCH_DEF
        },
        BENCH_IMPLICIT_DEFINE_COUNT,
    },
};

#define BENCH_DEFINE_NAMES_SUITE    0
#define BENCH_DEFINE_NAMES_IMPLICIT 1
#define BENCH_DEFINE_NAMES_COUNT    2

bench_define_names_t bench_define_names[BENCH_DEFINE_NAMES_COUNT] = {
    [BENCH_DEFINE_NAMES_IMPLICIT] = {
        (const char *const[BENCH_IMPLICIT_DEFINE_COUNT]){
            #define BENCH_DEF(k, v) \
                [k##_i] = #k,

                BENCH_IMPLICIT_DEFINES
            #undef BENCH_DEF
        },
        BENCH_IMPLICIT_DEFINE_COUNT,
    },
};

size_t bench_define_count;

typedef struct bench_define_cache_entry {
    // - >=0 => not cached
    // - -1  => cached
    ssize_t permutation;
    union {
        intmax_t value;
        const bench_define_t *define;
    } u;
} bench_define_cache_entry_t;

bench_define_cache_entry_t *bench_define_cache;
size_t bench_define_cache_capacity;

const char *bench_define_name(size_t define) {
    // lookup in our bench names
    for (size_t i = 0; i < BENCH_DEFINE_NAMES_COUNT; i++) {
        if (define < bench_define_names[i].count
                && bench_define_names[i].names
                && bench_define_names[i].names[define]) {
            return bench_define_names[i].names[define];
        }
    }

    return NULL;
}

bool bench_define_ispermutation(size_t define) {
    // is this define specific to the permutation?
    for (size_t i = 0; i < BENCH_DEFINE_MAP_IMPLICIT; i++) {
        if (define < bench_define_maps[i].count
                && bench_define_maps[i].defines[define].cb) {
            return true;
        }
    }

    return false;
}

size_t bench_define_permutations(size_t define) {
    for (size_t i = 0; i < BENCH_DEFINE_MAP_COUNT; i++) {
        if (define < bench_define_maps[i].count
                && bench_define_maps[i].defines[define].cb) {
            return bench_define_maps[i].defines[define].permutations
                    ? bench_define_maps[i].defines[define].permutations
                    : 1;
        }
    }

    return 0;
}

size_t bench_define_permutationpermutations(void) {
    size_t prod = 1;
    for (size_t d = 0; d < bench_define_count; d++) {
        size_t permutations = bench_define_permutations(d);
        if (permutations > 0) {
            prod *= permutations;
        }
    }
    return prod;
}

intmax_t bench_define(size_t define) {
    // cached?
    if (bench_define_cache[define].permutation == -1) {
        return bench_define_cache[define].u.value;

    // lazily defined?
    } else if (bench_define_cache[define].u.define) {
        // evaluate and store in cache
        bench_define_cache[define].u.value
                = bench_define_cache[define].u.define->cb(
                    bench_define_cache[define].u.define->data,
                    bench_define_cache[define].permutation);
        bench_define_cache[define].permutation = -1;
        return bench_define_cache[define].u.value;

    // not defined?
    } else {
        const char *name = bench_define_name(define);
        fprintf(stderr, "error: undefined define %s (%zd)\n",
                name ? name : "(unknown)",
                define);
        assert(false);
        exit(-1);
    }
}

// permutation updates
void bench_define_permutation(size_t perm) {
    // We can't completely precompute the defines easily, since they may be
    // mutually recursive. But we can precompute the permutations, which is
    // expensive otherwise.
    //
    // Note that it's not really worth it to make define lookup completely
    // lazy, the first thing we do is evaluate all defines for 1. deduplication
    // and 2. logging.

    if (bench_define_cache_capacity < bench_define_count) {
        // align to power of two to avoid any superlinear growth
        bench_define_cache_capacity = 1 << lfs_npw2(bench_define_count);
        bench_define_cache = realloc(
                bench_define_cache,
                bench_define_cache_capacity*sizeof(bench_define_cache_entry_t));
    }

    for (size_t d = 0; d < bench_define_count; d++) {
        // lookup our bench defines
        for (size_t i = 0; i < BENCH_DEFINE_MAP_COUNT; i++) {
            if (d < bench_define_maps[i].count
                    && bench_define_maps[i].defines[d].cb) {
                // note we can't precompute these due to mutual recursion
                const bench_define_t *define = &bench_define_maps[i].defines[d];
                bench_define_cache[d] = (bench_define_cache_entry_t){
                    .permutation = perm % define->permutations,
                    .u.define = define,
                };
                perm /= bench_define_maps[i].defines[d].permutations;
                goto next;
            }
        }

        // default to a null value, these should be unreachable
        bench_define_cache[d] = (bench_define_cache_entry_t){0};
    next:;
    }
}

// case updates
void bench_define_case(
        const struct bench_suite *suite,
        const struct bench_case *case_,
        size_t perm) {
    if (case_->defines) {
        bench_define_maps[BENCH_DEFINE_MAP_CASE] = (bench_define_map_t){
                &case_->defines[perm*suite->define_count],
                suite->define_count};
    } else {
        bench_define_maps[BENCH_DEFINE_MAP_CASE] = (bench_define_map_t){
                NULL, 0};
    }
}

// override updates
typedef struct bench_override {
    const char *name;
    bench_define_t define;
} bench_override_t;

typedef struct bench_override_value {
    intmax_t start;
    intmax_t stop;
    // step == 0 indicates a single value
    intmax_t step;
} bench_override_value_t;

typedef struct bench_override_data {
    bench_override_value_t *values;
    size_t value_count;
} bench_override_data_t;

intmax_t bench_override_cb(void *data, size_t i) {
    const bench_override_data_t *data_ = data;
    for (size_t j = 0; j < data_->value_count; j++) {
        const bench_override_value_t *v = &data_->values[j];
        // range?
        if (v->step) {
            size_t range_count;
            if (v->step > 0) {
                range_count = (v->stop-1 - v->start) / v->step + 1;
            } else {
                range_count = (v->start-1 - v->stop) / -v->step + 1;
            }

            if (i < range_count) {
                return i*v->step + v->start;
            }
            i -= range_count;
        // value?
        } else {
            if (i == 0) {
                return v->start;
            }
            i -= 1;
        }
    }

    // should never get here
    assert(false);
    __builtin_unreachable();
}

const bench_override_t *bench_overrides = NULL;
size_t bench_override_count = 0;

bench_define_t *bench_override_defines = NULL;
size_t bench_override_define_capacity = 0;

// suite/perm updates
void bench_define_suite(const struct bench_suite *suite) {
    // set define names
    bench_define_names[BENCH_DEFINE_NAMES_SUITE] = (bench_define_names_t){
            suite->define_names, suite->define_count};

    // set define count
    bench_define_count = suite->define_count > BENCH_IMPLICIT_DEFINE_COUNT
            ? suite->define_count
            : BENCH_IMPLICIT_DEFINE_COUNT;

    // map any overrides
    if (bench_override_count > 0) {
        if (bench_define_count > bench_override_define_capacity) {
            // align to power of two to avoid any superlinear growth
            bench_override_define_capacity = 1 << lfs_npw2(bench_define_count);
            bench_override_defines = realloc(
                    bench_override_defines,
                    bench_override_define_capacity*sizeof(bench_define_t));
        }

        memset(bench_override_defines, 0,
                bench_define_count*sizeof(bench_define_t));
        for (size_t i = 0; i < bench_override_count; i++) {
            for (size_t d = 0; d < bench_define_count; d++) {
                // name match?
                const char *name = bench_define_name(d);
                if (name && strcmp(name, bench_overrides[i].name) == 0) {
                    bench_override_defines[d] = bench_overrides[i].define;
                }
            }
        }

        bench_define_maps[BENCH_DEFINE_MAP_OVERRIDE] = (bench_define_map_t){
                bench_override_defines, bench_define_count};
    }
}

void bench_define_explicit(
        const bench_define_t *defines,
        size_t define_count) {
    bench_define_maps[BENCH_DEFINE_MAP_EXPLICIT] = (bench_define_map_t){
            defines, define_count};
}

void bench_define_cleanup(void) {
    // bench define management can allocate a few things
    free(bench_define_cache);
    free(bench_override_defines);
}



// bench state
const bench_id_t *bench_ids = (const bench_id_t[]) {
    {NULL, NULL, 0},
};
size_t bench_id_count = 1;

size_t bench_step_start = 0;
size_t bench_step_stop = -1;
size_t bench_step_step = 1;

const char *bench_disk_path = NULL;
const char *bench_trace_path = NULL;
bool bench_trace_backtrace = false;
uint32_t bench_trace_period = 0;
uint32_t bench_trace_freq = 0;
FILE *bench_trace_file = NULL;
uint32_t bench_trace_cycles = 0;
uint64_t bench_trace_time = 0;
uint64_t bench_trace_open_time = 0;
lfs_emubd_sleep_t bench_read_sleep = 0.0;
lfs_emubd_sleep_t bench_prog_sleep = 0.0;
lfs_emubd_sleep_t bench_erase_sleep = 0.0;

// this determines both the backtrace buffer and the trace printf buffer, if
// trace ends up interleaved or truncated this may need to be increased
#ifndef BENCH_TRACE_BACKTRACE_BUFFER_SIZE
#define BENCH_TRACE_BACKTRACE_BUFFER_SIZE 8192
#endif
void *bench_trace_backtrace_buffer[
    BENCH_TRACE_BACKTRACE_BUFFER_SIZE / sizeof(void*)];

// trace printing
void bench_trace(const char *fmt, ...) {
    if (bench_trace_path) {
        // sample at a specific period?
        if (bench_trace_period) {
            if (bench_trace_cycles % bench_trace_period != 0) {
                bench_trace_cycles += 1;
                return;
            }
            bench_trace_cycles += 1;
        }

        // sample at a specific frequency?
        if (bench_trace_freq) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            uint64_t now = (uint64_t)t.tv_sec*1000*1000*1000
                    + (uint64_t)t.tv_nsec;
            if (now - bench_trace_time < (1000*1000*1000) / bench_trace_freq) {
                return;
            }
            bench_trace_time = now;
        }

        if (!bench_trace_file) {
            // Tracing output is heavy and trying to open every trace
            // call is slow, so we only try to open the trace file every
            // so often. Note this doesn't affect successfully opened files
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            uint64_t now = (uint64_t)t.tv_sec*1000*1000*1000
                    + (uint64_t)t.tv_nsec;
            if (now - bench_trace_open_time < 100*1000*1000) {
                return;
            }
            bench_trace_open_time = now;

            // try to open the trace file
            int fd;
            if (strcmp(bench_trace_path, "-") == 0) {
                fd = dup(1);
                if (fd < 0) {
                    return;
                }
            } else {
                fd = open(
                        bench_trace_path,
                        O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK,
                        0666);
                if (fd < 0) {
                    return;
                }
                int err = fcntl(fd, F_SETFL, O_WRONLY | O_CREAT | O_APPEND);
                assert(!err);
            }

            FILE *f = fdopen(fd, "a");
            assert(f);
            int err = setvbuf(f, NULL, _IOFBF,
                    BENCH_TRACE_BACKTRACE_BUFFER_SIZE);
            assert(!err);
            bench_trace_file = f;
        }

        // print trace
        va_list va;
        va_start(va, fmt);
        int res = vfprintf(bench_trace_file, fmt, va);
        va_end(va);
        if (res < 0) {
            fclose(bench_trace_file);
            bench_trace_file = NULL;
            return;
        }

        if (bench_trace_backtrace) {
            // print backtrace
            size_t count = backtrace(
                    bench_trace_backtrace_buffer,
                    BENCH_TRACE_BACKTRACE_BUFFER_SIZE);
            // note we skip our own stack frame
            for (size_t i = 1; i < count; i++) {
                res = fprintf(bench_trace_file, "\tat %p\n",
                        bench_trace_backtrace_buffer[i]);
                if (res < 0) {
                    fclose(bench_trace_file);
                    bench_trace_file = NULL;
                    return;
                }
            }
        }

        // flush immediately
        fflush(bench_trace_file);
    }
}


// bench prng
uint32_t bench_prng(uint32_t *state) {
    // A simple xorshift32 generator, easily reproducible. Keep in mind
    // determinism is much more important than actual randomness here.
    uint32_t x = *state;
    // must be non-zero, use uintmax here so that seed=0 is different
    // from seed=1 and seed=range(0,n) makes a bit more sense
    if (x == 0) {
        x = -1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// bench factorial
size_t bench_factorial(size_t x) {
    size_t y = 1;
    for (size_t i = 2; i <= x; i++) {
        y *= i;
    }
    return y;
}

// bench array permutations
void bench_permutation(size_t i, uint32_t *buffer, size_t size) {
    // https://stackoverflow.com/a/7919887 and
    // https://stackoverflow.com/a/24257996 helped a lot with this, but
    // changed to run in O(n) with no extra memory. This has a tradeoff
    // of generating the permutations in an unintuitive order.

    // initialize array
    for (size_t j = 0; j < size; j++) {
        buffer[j] = j;
    }

    for (size_t j = 0; j < size; j++) {
        // swap index with digit
        //
        //      .- i%rem --.
        //      v     .----+----.
        // [p0 p1 |-> r0 r1 r2 r3]
        //
        size_t t = buffer[j + (i % (size-j))];
        buffer[j + (i % (size-j))] = buffer[j];
        buffer[j] = t;
        // update i
        i /= (size-j);
    }
}


// bench recording state
typedef struct bench_record {
    const char *meas;
    uintmax_t iter;
    uintmax_t size;
    lfs_emubd_io_t last_readed;
    lfs_emubd_io_t last_proged;
    lfs_emubd_io_t last_erased;
} bench_record_t;

static struct lfs_config *bench_cfg = NULL;
static bench_record_t *bench_records;
size_t bench_record_count;
size_t bench_record_capacity;

void bench_reset(struct lfs_config *cfg) {
    bench_cfg = cfg;
    bench_record_count = 0;
}

void bench_start(const char *meas, uintmax_t iter, uintmax_t size) {
    // measure current read/prog/erase
    assert(bench_cfg);
    lfs_emubd_sio_t readed = lfs_emubd_readed(bench_cfg);
    assert(readed >= 0);
    lfs_emubd_sio_t proged = lfs_emubd_proged(bench_cfg);
    assert(proged >= 0);
    lfs_emubd_sio_t erased = lfs_emubd_erased(bench_cfg);
    assert(erased >= 0);

    // allocate a new record
    bench_record_t *record = mappend(
            (void**)&bench_records,
            sizeof(bench_record_t),
            &bench_record_count,
            &bench_record_capacity);
    record->meas = meas;
    record->iter = iter;
    record->size = size;
    record->last_readed = readed;
    record->last_proged = proged;
    record->last_erased = erased;
}

void bench_stop(const char *meas) {
    // measure current read/prog/erase
    assert(bench_cfg);
    lfs_emubd_sio_t readed = lfs_emubd_readed(bench_cfg);
    assert(readed >= 0);
    lfs_emubd_sio_t proged = lfs_emubd_proged(bench_cfg);
    assert(proged >= 0);
    lfs_emubd_sio_t erased = lfs_emubd_erased(bench_cfg);
    assert(erased >= 0);

    // find our record
    for (size_t i = 0; i < bench_record_count; i++) {
        if (strcmp(bench_records[i].meas, meas) == 0) {
            // print results
            printf("benched %s %zd %zd %"PRIu64" %"PRIu64" %"PRIu64"\n",
                    bench_records[i].meas,
                    bench_records[i].iter,
                    bench_records[i].size,
                    readed - bench_records[i].last_readed,
                    proged - bench_records[i].last_proged,
                    erased - bench_records[i].last_erased);

            // remove our record
            memmove(&bench_records[i],
                    &bench_records[i+1],
                    bench_record_count-(i+1));
            bench_record_count -= 1;
            return;
        }
    }

    // not found?
    fprintf(stderr, "error: bench stopped before it was started (%s)\n",
            meas);
    assert(false);
    exit(-1);
}

void bench_result(const char *meas, uintmax_t iter, uintmax_t size,
        uintmax_t result) {
    // we just print these directly
    printf("benched %s %zd %zd %"PRIu64"\n",
            meas,
            iter,
            size,
            result);
}

void bench_fresult(const char *meas, uintmax_t iter, uintmax_t size,
        double result) {
    // we just print these directly
    printf("benched %s %zd %zd %.6f\n",
            meas,
            iter,
            size,
            result);
}


// encode our permutation into a reusable id
static void perm_printid(
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    (void)suite;
    // case[:permutation]
    printf("%s:", case_->name);
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_ispermutation(d)) {
            leb16_print(d);
            leb16_print(BENCH_DEFINE(d));
        }
    }
}

// a quick trie for keeping track of permutations we've seen
typedef struct bench_seen {
    struct bench_seen_branch *branches;
    size_t branch_count;
    size_t branch_capacity;
} bench_seen_t;

struct bench_seen_branch {
    intmax_t define;
    struct bench_seen branch;
};

bool bench_seen_insert(bench_seen_t *seen) {
    // use the currently set defines
    bool was_seen = true;
    for (size_t d = 0; d < bench_define_count; d++) {
        // treat unpermuted defines the same as 0
        intmax_t define = bench_define_ispermutation(d) ? BENCH_DEFINE(d) : 0;

        // already seen?
        struct bench_seen_branch *branch = NULL;
        for (size_t i = 0; i < seen->branch_count; i++) {
            if (seen->branches[i].define == define) {
                branch = &seen->branches[i];
                break;
            }
        }

        // need to create a new node
        if (!branch) {
            was_seen = false;
            branch = mappend(
                    (void**)&seen->branches,
                    sizeof(struct bench_seen_branch),
                    &seen->branch_count,
                    &seen->branch_capacity);
            branch->define = define;
            branch->branch = (bench_seen_t){NULL, 0, 0};
        }

        seen = &branch->branch;
    }

    return was_seen;
}

void bench_seen_cleanup(bench_seen_t *seen) {
    for (size_t i = 0; i < seen->branch_count; i++) {
        bench_seen_cleanup(&seen->branches[i].branch);
    }
    free(seen->branches);
}

// iterate through permutations in a bench case
static void case_forperm(
        const struct bench_suite *suite,
        const struct bench_case *case_,
        const bench_define_t *defines,
        size_t define_count,
        void (*cb)(
            void *data,
            const struct bench_suite *suite,
            const struct bench_case *case_),
        void *data) {
    // explicit permutation?
    if (defines) {
        bench_define_explicit(defines, define_count);

        size_t permutations = bench_define_permutationpermutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            bench_define_permutation(p);

            cb(data, suite, case_);
        }

        return;
    }

    // deduplicate permutations with the same defines
    //
    // this can easily happen when overriding multiple case permutations,
    // we can't tell that multiple case permutations don't change defines,
    // duplicating results
    bench_seen_t seen = {NULL, 0, 0};

    for (size_t k = 0;
            k < (case_->permutations ? case_->permutations : 1);
            k++) {
        // define case permutation
        bench_define_case(suite, case_, k);

        size_t permutations = bench_define_permutationpermutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            bench_define_permutation(p);

            // have we seen this permutation before?
            bool was_seen = bench_seen_insert(&seen);
            if (!(k == 0 && p == 0) && was_seen) {
                continue;
            }

            cb(data, suite, case_);
        }
    }

    bench_seen_cleanup(&seen);
}


// how many permutations are there actually in a bench case
struct perm_count_state {
    size_t total;
    size_t filtered;
};

void perm_count(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct perm_count_state *state = data;
    (void)suite;
    (void)case_;

    state->total += 1;

    if (case_->if_ && !case_->if_()) {
        return;
    }

    state->filtered += 1;
}


// operations we can do
static void summary(void) {
    printf("%-23s  %7s %7s %7s %15s\n",
            "", "flags", "suites", "cases", "perms");
    size_t suites = 0;
    size_t cases = 0;
    bench_flags_t flags = 0;
    struct perm_count_state perms = {0, 0};

    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_count,
                        &perms);
            }

            suites += 1;
            flags |= bench_suites[i]->flags;
        }
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
    char flag_buf[64];
    sprintf(flag_buf, "%s%s",
            (flags & BENCH_INTERNAL)  ? "i" : "",
            (!flags)                  ? "-" : "");
    printf("%-23s  %7s %7zu %7zu %15s\n",
            "TOTAL",
            flag_buf,
            suites,
            cases,
            perm_buf);
}

static void list_suites(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        size_t len = strlen(bench_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %7s %15s\n",
            name_width, "suite", "flags", "cases", "perms");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            size_t cases = 0;
            struct perm_count_state perms = {0, 0};

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_count,
                        &perms);
            }

            // no benches found?
            if (!cases) {
                continue;
            }

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
            char flag_buf[64];
            sprintf(flag_buf, "%s%s",
                    (bench_suites[i]->flags & BENCH_INTERNAL)  ? "i" : "",
                    (!bench_suites[i]->flags)                  ? "-" : "");
            printf("%-*s  %7s %7zu %15s\n",
                    name_width,
                    bench_suites[i]->name,
                    flag_buf,
                    cases,
                    perm_buf);
        }
    }
}

static void list_cases(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
            size_t len = strlen(bench_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %15s\n", name_width, "case", "flags", "perms");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                struct perm_count_state perms = {0, 0};
                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_count,
                        &perms);

                char perm_buf[64];
                sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
                char flag_buf[64];
                sprintf(flag_buf, "%s%s",
                        (bench_suites[i]->cases[j].flags & BENCH_INTERNAL)
                            ? "i" : "",
                        (!bench_suites[i]->cases[j].flags)
                            ? "-" : "");
                printf("%-*s  %7s %15s\n",
                        name_width,
                        bench_suites[i]->cases[j].name,
                        flag_buf,
                        perm_buf);
            }
        }
    }
}

static void list_suite_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        size_t len = strlen(bench_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "suite", "path");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            size_t cases = 0;

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;

                    cases += 1;
                }
            }

            // no benches found?
            if (!cases) {
                continue;
            }

            printf("%-*s  %s\n",
                    name_width,
                    bench_suites[i]->name,
                    bench_suites[i]->path);
        }
    }
}

static void list_case_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
            size_t len = strlen(bench_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "case", "path");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                printf("%-*s  %s\n",
                        name_width,
                        bench_suites[i]->cases[j].name,
                        bench_suites[i]->cases[j].path);
            }
        }
    }
}

struct list_defines_define {
    const char *name;
    intmax_t *values;
    size_t value_count;
    size_t value_capacity;
};

struct list_defines_defines {
    struct list_defines_define *defines;
    size_t define_count;
    size_t define_capacity;
};

static void list_defines_add(
        struct list_defines_defines *defines,
        size_t d) {
    const char *name = bench_define_name(d);
    intmax_t value = BENCH_DEFINE(d);

    // define already in defines?
    for (size_t i = 0; i < defines->define_count; i++) {
        if (strcmp(defines->defines[i].name, name) == 0) {
            // value already in values?
            for (size_t j = 0; j < defines->defines[i].value_count; j++) {
                if (defines->defines[i].values[j] == value) {
                    return;
                }
            }

            *(intmax_t*)mappend(
                (void**)&defines->defines[i].values,
                sizeof(intmax_t),
                &defines->defines[i].value_count,
                &defines->defines[i].value_capacity) = value;

            return;
        }
    }

    // new define?
    struct list_defines_define *define = mappend(
            (void**)&defines->defines,
            sizeof(struct list_defines_define),
            &defines->define_count,
            &defines->define_capacity);
    define->name = name;
    define->values = malloc(sizeof(intmax_t));
    define->values[0] = value;
    define->value_count = 1;
    define->value_capacity = 1;
}

void perm_list_defines(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (d < BENCH_IMPLICIT_DEFINE_COUNT
                || bench_define_ispermutation(d)) {
            list_defines_add(defines, d);
        }
    }
}

void perm_list_permutation_defines(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect permutation_defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_ispermutation(d)) {
            list_defines_add(defines, d);
        }
    }
}

static void list_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_list_defines,
                        &defines);
            }
        }
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}

static void list_permutation_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add permutation defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_list_permutation_defines,
                        &defines);
            }
        }
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}

static void list_implicit_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // yes we do need to define a suite, this does a bit of bookeeping
    // such as setting up the define cache
    bench_define_suite(&(const struct bench_suite){0});
    bench_define_permutation(0);

    // add implicit defines
    for (size_t d = 0; d < BENCH_IMPLICIT_DEFINE_COUNT; d++) {
        list_defines_add(&defines, d);
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}



// global bench step count
size_t bench_step = 0;

void perm_run(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    (void)data;

    // skip this step?
    if (!(bench_step >= bench_step_start
            && bench_step < bench_step_stop
            && (bench_step-bench_step_start) % bench_step_step == 0)) {
        bench_step += 1;
        return;
    }
    bench_step += 1;

    // filter?
    if (case_->if_ && !case_->if_()) {
        printf("skipped ");
        perm_printid(suite, case_);
        printf("\n");
        return;
    }

    // create block device and configuration
    lfs_emubd_t bd;

    struct lfs_config cfg = {
        .context            = &bd,
        .read               = lfs_emubd_read,
        .prog               = lfs_emubd_prog,
        .erase              = lfs_emubd_erase,
        .sync               = lfs_emubd_sync,
        BENCH_CFG
    };

    struct lfs_emubd_config bdcfg = {
        .disk_path          = bench_disk_path,
        .read_sleep         = bench_read_sleep,
        .prog_sleep         = bench_prog_sleep,
        .erase_sleep        = bench_erase_sleep,
        BENCH_BDCFG
    };

    int err = lfs_emubd_createcfg(&cfg, bench_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the bench
    bench_reset(&cfg);
    printf("running ");
    perm_printid(suite, case_);
    printf("\n");

    case_->run(&cfg);

    printf("finished ");
    perm_printid(suite, case_);
    printf("\n");

    // cleanup
    err = lfs_emubd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void run(void) {
    // ignore disconnected pipes
    signal(SIGPIPE, SIG_IGN);

    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        bench_ids[t].defines,
                        bench_ids[t].define_count,
                        perm_run,
                        NULL);
            }
        }
    }
}



// option handling
enum opt_flags {
    OPT_HELP                     = 'h',
    OPT_SUMMARY                  = 'Y',
    OPT_LIST_SUITES              = 'l',
    OPT_LIST_CASES               = 'L',
    OPT_LIST_SUITE_PATHS         = 1,
    OPT_LIST_CASE_PATHS          = 2,
    OPT_LIST_DEFINES             = 3,
    OPT_LIST_PERMUTATION_DEFINES = 4,
    OPT_LIST_IMPLICIT_DEFINES    = 5,
    OPT_DEFINE                   = 'D',
    OPT_STEP                     = 's',
    OPT_DISK                     = 'd',
    OPT_TRACE                    = 't',
    OPT_TRACE_BACKTRACE          = 6,
    OPT_TRACE_PERIOD             = 7,
    OPT_TRACE_FREQ               = 8,
    OPT_READ_SLEEP               = 9,
    OPT_PROG_SLEEP               = 10,
    OPT_ERASE_SLEEP              = 11,
};

const char *short_opts = "hYlLD:s:d:t:";

const struct option long_opts[] = {
    {"help",             no_argument,       NULL, OPT_HELP},
    {"summary",          no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",      no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",       no_argument,       NULL, OPT_LIST_CASES},
    {"list-suite-paths", no_argument,       NULL, OPT_LIST_SUITE_PATHS},
    {"list-case-paths",  no_argument,       NULL, OPT_LIST_CASE_PATHS},
    {"list-defines",     no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-permutation-defines",
                         no_argument,       NULL, OPT_LIST_PERMUTATION_DEFINES},
    {"list-implicit-defines",
                         no_argument,       NULL, OPT_LIST_IMPLICIT_DEFINES},
    {"define",           required_argument, NULL, OPT_DEFINE},
    {"step",             required_argument, NULL, OPT_STEP},
    {"disk",             required_argument, NULL, OPT_DISK},
    {"trace",            required_argument, NULL, OPT_TRACE},
    {"trace-backtrace",  no_argument,       NULL, OPT_TRACE_BACKTRACE},
    {"trace-period",     required_argument, NULL, OPT_TRACE_PERIOD},
    {"trace-freq",       required_argument, NULL, OPT_TRACE_FREQ},
    {"read-sleep",       required_argument, NULL, OPT_READ_SLEEP},
    {"prog-sleep",       required_argument, NULL, OPT_PROG_SLEEP},
    {"erase-sleep",      required_argument, NULL, OPT_ERASE_SLEEP},
    {NULL, 0, NULL, 0},
};

const char *const help_text[] = {
    "Show this help message.",
    "Show quick summary.",
    "List bench suites.",
    "List bench cases.",
    "List the path for each bench suite.",
    "List the path and line number for each bench case.",
    "List all defines in this bench-runner.",
    "List explicit defines in this bench-runner.",
    "List implicit defines in this bench-runner.",
    "Override a bench define.",
    "Comma-separated range of bench permutations to run (start,stop,step).",
    "Direct block device operations to this file.",
    "Direct trace output to this file.",
    "Include a backtrace with every trace statement.",
    "Sample trace output at this period in cycles.",
    "Sample trace output at this frequency in hz.",
    "Artificial read delay in seconds.",
    "Artificial prog delay in seconds.",
    "Artificial erase delay in seconds.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    size_t bench_override_capacity = 0;
    size_t bench_id_capacity = 0;

    // parse options
    while (true) {
        int c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        switch (c) {
            // generate help message
            case OPT_HELP: {
                printf("usage: %s [options] [bench_id]\n", argv[0]);
                printf("\n");

                printf("options:\n");
                size_t i = 0;
                while (long_opts[i].name) {
                    size_t indent;
                    if (long_opts[i].has_arg == no_argument) {
                        if (long_opts[i].val >= '0' && long_opts[i].val < 'z') {
                            indent = printf("  -%c, --%s ",
                                    long_opts[i].val,
                                    long_opts[i].name);
                        } else {
                            indent = printf("  --%s ",
                                    long_opts[i].name);
                        }
                    } else {
                        if (long_opts[i].val >= '0' && long_opts[i].val < 'z') {
                            indent = printf("  -%c %s, --%s %s ",
                                    long_opts[i].val,
                                    long_opts[i].name,
                                    long_opts[i].name,
                                    long_opts[i].name);
                        } else {
                            indent = printf("  --%s %s ",
                                    long_opts[i].name,
                                    long_opts[i].name);
                        }
                    }

                    // a quick, hacky, byte-level method for text wrapping
                    size_t len = strlen(help_text[i]);
                    size_t j = 0;
                    if (indent < 24) {
                        printf("%*s %.80s\n",
                                (int)(24-1-indent),
                                "",
                                &help_text[i][j]);
                        j += 80;
                    } else {
                        printf("\n");
                    }

                    while (j < len) {
                        printf("%24s%.80s\n", "", &help_text[i][j]);
                        j += 80;
                    }

                    i += 1;
                }

                printf("\n");
                exit(0);
            }
            // summary/list flags
            case OPT_SUMMARY:
                op = summary;
                break;
            case OPT_LIST_SUITES:
                op = list_suites;
                break;
            case OPT_LIST_CASES:
                op = list_cases;
                break;
            case OPT_LIST_SUITE_PATHS:
                op = list_suite_paths;
                break;
            case OPT_LIST_CASE_PATHS:
                op = list_case_paths;
                break;
            case OPT_LIST_DEFINES:
                op = list_defines;
                break;
            case OPT_LIST_PERMUTATION_DEFINES:
                op = list_permutation_defines;
                break;
            case OPT_LIST_IMPLICIT_DEFINES:
                op = list_implicit_defines;
                break;
            // configuration
            case OPT_DEFINE: {
                // allocate space
                bench_override_t *override = mappend(
                        (void**)&bench_overrides,
                        sizeof(bench_override_t),
                        &bench_override_count,
                        &bench_override_capacity);

                // parse into string key/intmax_t value, cannibalizing the
                // arg in the process
                char *sep = strchr(optarg, '=');
                char *parsed = NULL;
                if (!sep) {
                    goto invalid_define;
                }
                *sep = '\0';
                override->name = optarg;
                optarg = sep+1;

                // parse comma-separated permutations
                {
                    bench_override_value_t *override_values = NULL;
                    size_t override_value_count = 0;
                    size_t override_value_capacity = 0;
                    size_t override_permutations = 0;
                    while (true) {
                        optarg += strspn(optarg, " ");

                        if (strncmp(optarg, "range", strlen("range")) == 0) {
                            // range of values
                            optarg += strlen("range");
                            optarg += strspn(optarg, " ");
                            if (*optarg != '(') {
                                goto invalid_define;
                            }
                            optarg += 1;

                            intmax_t start = strtoumax(optarg, &parsed, 0);
                            intmax_t stop = -1;
                            intmax_t step = 1;
                            // allow empty string for start=0
                            if (parsed == optarg) {
                                start = 0;
                            }
                            optarg = parsed + strspn(parsed, " ");

                            if (*optarg != ',' && *optarg != ')') {
                                goto invalid_define;
                            }

                            if (*optarg == ',') {
                                optarg += 1;
                                stop = strtoumax(optarg, &parsed, 0);
                                // allow empty string for stop=end
                                if (parsed == optarg) {
                                    stop = -1;
                                }
                                optarg = parsed + strspn(parsed, " ");

                                if (*optarg != ',' && *optarg != ')') {
                                    goto invalid_define;
                                }

                                if (*optarg == ',') {
                                    optarg += 1;
                                    step = strtoumax(optarg, &parsed, 0);
                                    // allow empty string for stop=1
                                    if (parsed == optarg) {
                                        step = 1;
                                    }
                                    optarg = parsed + strspn(parsed, " ");

                                    if (*optarg != ')') {
                                        goto invalid_define;
                                    }
                                }
                            } else {
                                // single value = stop only
                                stop = start;
                                start = 0;
                            }

                            if (*optarg != ')') {
                                goto invalid_define;
                            }
                            optarg += 1;

                            // append range
                            *(bench_override_value_t*)mappend(
                                    (void**)&override_values,
                                    sizeof(bench_override_value_t),
                                    &override_value_count,
                                    &override_value_capacity)
                                    = (bench_override_value_t){
                                .start = start,
                                .stop = stop,
                                .step = step,
                            };
                            if (step > 0) {
                                override_permutations += (stop-1 - start)
                                        / step + 1;
                            } else {
                                override_permutations += (start-1 - stop)
                                        / -step + 1;
                            }
                        } else if (*optarg != '\0') {
                            // single value
                            intmax_t define = strtoumax(optarg, &parsed, 0);
                            if (parsed == optarg) {
                                goto invalid_define;
                            }
                            optarg = parsed + strspn(parsed, " ");

                            // append value
                            *(bench_override_value_t*)mappend(
                                    (void**)&override_values,
                                    sizeof(bench_override_value_t),
                                    &override_value_count,
                                    &override_value_capacity)
                                    = (bench_override_value_t){
                                .start = define,
                                .step = 0,
                            };
                            override_permutations += 1;
                        } else {
                            break;
                        }

                        if (*optarg == ',') {
                            optarg += 1;
                        }
                    }

                    override->define.cb = bench_override_cb;
                    override->define.data = malloc(
                            sizeof(bench_override_data_t));
                    *(bench_override_data_t*)override->define.data
                            = (bench_override_data_t){
                        .values = override_values,
                        .value_count = override_value_count,
                    };
                    override->define.permutations = override_permutations;
                }
                break;

invalid_define:
                fprintf(stderr, "error: invalid define: %s\n", optarg);
                exit(-1);
            }
            case OPT_STEP: {
                char *parsed = NULL;
                bench_step_start = strtoumax(optarg, &parsed, 0);
                bench_step_stop = -1;
                bench_step_step = 1;
                // allow empty string for start=0
                if (parsed == optarg) {
                    bench_step_start = 0;
                }
                optarg = parsed + strspn(parsed, " ");

                if (*optarg != ',' && *optarg != '\0') {
                    goto step_unknown;
                }

                if (*optarg == ',') {
                    optarg += 1;
                    bench_step_stop = strtoumax(optarg, &parsed, 0);
                    // allow empty string for stop=end
                    if (parsed == optarg) {
                        bench_step_stop = -1;
                    }
                    optarg = parsed + strspn(parsed, " ");

                    if (*optarg != ',' && *optarg != '\0') {
                        goto step_unknown;
                    }

                    if (*optarg == ',') {
                        optarg += 1;
                        bench_step_step = strtoumax(optarg, &parsed, 0);
                        // allow empty string for stop=1
                        if (parsed == optarg) {
                            bench_step_step = 1;
                        }
                        optarg = parsed + strspn(parsed, " ");

                        if (*optarg != '\0') {
                            goto step_unknown;
                        }
                    }
                } else {
                    // single value = stop only
                    bench_step_stop = bench_step_start;
                    bench_step_start = 0;
                }

                break;
step_unknown:
                fprintf(stderr, "error: invalid step: %s\n", optarg);
                exit(-1);
            }
            case OPT_DISK:
                bench_disk_path = optarg;
                break;
            case OPT_TRACE:
                bench_trace_path = optarg;
                break;
            case OPT_TRACE_BACKTRACE:
                bench_trace_backtrace = true;
                break;
            case OPT_TRACE_PERIOD: {
                char *parsed = NULL;
                bench_trace_period = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid trace-period: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_TRACE_FREQ: {
                char *parsed = NULL;
                bench_trace_freq = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid trace-freq: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_READ_SLEEP: {
                char *parsed = NULL;
                double read_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid read-sleep: %s\n", optarg);
                    exit(-1);
                }
                bench_read_sleep = read_sleep*1.0e9;
                break;
            }
            case OPT_PROG_SLEEP: {
                char *parsed = NULL;
                double prog_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid prog-sleep: %s\n", optarg);
                    exit(-1);
                }
                bench_prog_sleep = prog_sleep*1.0e9;
                break;
            }
            case OPT_ERASE_SLEEP: {
                char *parsed = NULL;
                double erase_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid erase-sleep: %s\n", optarg);
                    exit(-1);
                }
                bench_erase_sleep = erase_sleep*1.0e9;
                break;
            }
            // done parsing
            case -1:
                goto getopt_done;
            // unknown arg, getopt prints a message for us
            default:
                exit(-1);
        }
    }
getopt_done: ;

    if (argc > optind) {
        // reset our bench identifier list
        bench_ids = NULL;
        bench_id_count = 0;
        bench_id_capacity = 0;
    }

    // parse bench identifier, if any, cannibalizing the arg in the process
    for (; argc > optind; optind++) {
        bench_define_t *defines = NULL;
        size_t define_count = 0;

        // parse name, can be suite or case
        char *name = argv[optind];
        char *defines_ = strchr(name, ':');
        if (defines_) {
            *defines_ = '\0';
            defines_ += 1;
        }

        // remove optional path and .toml suffix
        char *slash = strrchr(name, '/');
        if (slash) {
            name = slash+1;
        }

        size_t name_len = strlen(name);
        if (name_len > 5 && strcmp(&name[name_len-5], ".toml") == 0) {
            name[name_len-5] = '\0';
        }

        if (defines_) {
            // parse defines
            while (true) {
                char *parsed;
                size_t d = leb16_parse(defines_, &parsed);
                intmax_t v = leb16_parse(parsed, &parsed);
                if (parsed == defines_) {
                    break;
                }
                defines_ = parsed;

                if (d >= define_count) {
                    // align to power of two to avoid any superlinear growth
                    size_t ncount = 1 << lfs_npw2(d+1);
                    defines = realloc(defines,
                            ncount*sizeof(bench_define_t));
                    memset(defines+define_count, 0,
                            (ncount-define_count)*sizeof(bench_define_t));
                    define_count = ncount;
                }
                defines[d] = BENCH_LIT(v);
            }
        }

        // append to identifier list
        *(bench_id_t*)mappend(
                (void**)&bench_ids,
                sizeof(bench_id_t),
                &bench_id_count,
                &bench_id_capacity) = (bench_id_t){
            .name = name,
            .defines = defines,
            .define_count = define_count,
        };
    }

    // do the thing
    op();

    // cleanup (need to be done for valgrind benching)
    bench_define_cleanup();
    if (bench_overrides) {
        for (size_t i = 0; i < bench_override_count; i++) {
            free((void*)bench_overrides[i].define.data);
        }
        free((void*)bench_overrides);
    }
    if (bench_id_capacity) {
        for (size_t i = 0; i < bench_id_count; i++) {
            free((void*)bench_ids[i].defines);
        }
        free((void*)bench_ids);
    }
}
