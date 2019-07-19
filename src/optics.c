/* optics.c
   Rémi Attab (remi.attab@gmail.com), 25 Feb 2016
   FreeBSD-style copyright and disclaimer apply
*/

#include "optics_priv.h"
#include "utils/compiler.h"
#include "utils/errors.h"
#include "utils/type_pun.h"
#include "utils/htable.h"
#include "utils/lock.h"
#include "utils/rng.h"
#include "utils/time.h"
#include "utils/bits.h"
#include "utils/log.h"
#include "utils/socket.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <bsd/string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>


// -----------------------------------------------------------------------------
// config
// -----------------------------------------------------------------------------

typedef size_t optics_epoch_t; // 0 - 1 value.
typedef atomic_size_t atomic_optics_epoch_t;

enum { cache_line_len = 64 };


// -----------------------------------------------------------------------------
// impl
// -----------------------------------------------------------------------------

static bool optics_defer_free(struct optics *optics, struct optics_lens *ptr);

// contains struct optics_lens
#include "lens.c"


// -----------------------------------------------------------------------------
// struct
// -----------------------------------------------------------------------------

struct optics_defer
{
    struct optics_lens *ptr;
    struct optics_defer *next;
};

struct optics
{
    // Synchronizes:
    //   - optics.keys: read and write
    //   - optics.lens_head: write-only (reads are lock-free).
    //
    // Even though it's not strictly required, it's simpler to keep both of
    // these structures consistent with each-other.
    struct slock lock;

    struct htable keys;
    atomic_uintptr_t lens_head;

    atomic_size_t epoch;
    optics_ts_t epoch_last_inc;
    atomic_uintptr_t epoch_defers[2];

    char prefix[optics_name_max_len];
};


// -----------------------------------------------------------------------------
// open/close
// -----------------------------------------------------------------------------

struct optics * optics_create_at(const char *name, optics_ts_t now)
{
    struct optics *optics = calloc(1, sizeof(*optics));
    optics_assert_alloc(optics);

    if (!optics_set_prefix(optics, name)) goto fail_prefix;
    optics->epoch_last_inc = now;

    return optics;

  fail_prefix:
    free(optics);
    return NULL;
}

struct optics * optics_create(const char *name)
{
    return optics_create_at(name, clock_wall());
}

void optics_close(struct optics *optics)
{
    optics_assert(slock_try_lock(&optics->lock),
            "closing optics with active thread");

    htable_reset(&optics->keys);
    free(optics);
}


const char *optics_get_prefix(struct optics *optics)
{
    return optics->prefix;
}

bool optics_set_prefix(struct optics *optics, const char *prefix)
{
    if (strnlen(prefix, optics_name_max_len) == optics_name_max_len) {
        optics_fail("prefix '%s' length is greater than max length '%d'",
                prefix, optics_name_max_len);
        return false;
    }

    strlcpy(optics->prefix, prefix, optics_name_max_len);
    return true;
}


// -----------------------------------------------------------------------------
// alloc
// -----------------------------------------------------------------------------

static bool optics_defer_free(struct optics *optics, struct optics_lens *ptr)
{
    struct optics_defer *node = malloc(sizeof(*node));
    if (!node) return false;

    node->ptr = ptr;

    optics_epoch_t epoch = optics_epoch(optics);
    atomic_uintptr_t *head = &optics->epoch_defers[epoch];

    // Synchronizes with optics_free_defered to make sure that our node is fully
    // written befor it is read.
    uintptr_t old = atomic_load_explicit(head, memory_order_relaxed);
    do {
        node->next = pun_itop(old);
    } while (!atomic_compare_exchange_weak_explicit(
                    head, &old, pun_ptoi(node), memory_order_release, memory_order_relaxed));

    return true;
}

static void optics_free_defered(struct optics *optics, optics_epoch_t epoch)
{
    // Synchronizes with optics_defer_free to make sure that all nodes have been
    // fully written before we read them.
    atomic_uintptr_t *head = &optics->epoch_defers[epoch];
    struct optics_defer *node = pun_itop(atomic_exchange_explicit(head, 0, memory_order_acquire));

    while (node) {
        free(node->ptr);

        struct optics_defer *next = node->next;
        free(node);
        node = next;
    }
}


// -----------------------------------------------------------------------------
// epoch
// -----------------------------------------------------------------------------

// Memory order semantics are pretty weird here since the write (inc) op does
// not need to synchronize any data with the read op yet the read op should
// still prevent hoisting.

optics_epoch_t optics_epoch(struct optics *optics)
{
    return atomic_load_explicit(&optics->epoch, memory_order_acquire) & 1;
}

optics_epoch_t optics_epoch_inc(struct optics *optics)
{
    optics_free_defered(optics, optics_epoch(optics) ^ 1);

    return atomic_fetch_add(&optics->epoch, 1) & 1;
}

optics_epoch_t optics_epoch_inc_at(
        struct optics *optics, optics_ts_t now, optics_ts_t *last_inc)
{
    *last_inc = optics->epoch_last_inc;
    optics->epoch_last_inc = now;

    return optics_epoch_inc(optics);
}


// -----------------------------------------------------------------------------
// list
// -----------------------------------------------------------------------------

static void optics_push_lens(struct optics *optics, struct optics_lens *lens)
{
    optics_assert(!slock_try_lock(&optics->lock), "pushing lens without lock held");

    atomic_uintptr_t *head = &optics->lens_head;
    struct optics_lens *old_head = pun_itop(atomic_load_explicit(head, memory_order_relaxed));
    lens_set_next(lens, old_head);

    // Synchronizes with optics_foreach_lens to ensure that the node is fully
    // written before it is accessed.
    atomic_store_explicit(head, pun_ptoi(lens), memory_order_release);
}

// Removes the lens from the polling which is different then defer free which
// gets rid of the memory associated with lens.
static void optics_remove_lens(struct optics *optics, struct optics_lens *lens)
{
    optics_assert(!slock_try_lock(&optics->lock), "removing lens without lock held");

    lens_kill(lens);

    atomic_uintptr_t *head = &optics->lens_head;
    struct optics_lens *old_head = pun_itop(atomic_load_explicit(head, memory_order_relaxed));
    if (old_head == lens)
        atomic_store_explicit(head, pun_ptoi(lens_next(lens)), memory_order_relaxed);
}

// Should be a lock-free traversal of the lenses so that the poller doens't
// block on any record operations.
enum optics_ret optics_foreach_lens(struct optics *optics, void *ctx, optics_foreach_t cb)
{

    // Synchronizes with optics_push_lens to ensure that all nodes are fully
    // written before we access them.
    atomic_uintptr_t *head = &optics->lens_head;
    struct optics_lens *lens = pun_itop(atomic_load_explicit(head, memory_order_acquire));

    while (lens) {
        struct optics_lens ol = { .optics = optics };
        enum optics_ret ret = cb(ctx, &ol);
        if (ret != optics_ok) return ret;

        lens = lens_next(lens);
    }

    return optics_ok;
}


// -----------------------------------------------------------------------------
// lens
// -----------------------------------------------------------------------------

struct optics_lens * optics_lens_get(struct optics *optics, const char *name)
{
    struct optics_lens *lens = NULL;

    {
        slock_lock(&optics->lock);

        struct htable_ret ret = htable_get(&optics->keys, name);
        if (ret.ok) lens = pun_itop(ret.value);

        slock_unlock(&optics->lock);
    }

    return lens;
}

static bool
optics_lens_create(struct optics *optics, struct optics_lens *lens)
{
    bool ok = false;
    {
        slock_lock(&optics->lock);

        ok = htable_put(&optics->keys, lens_name(lens), pun_ptoi(lens)).ok;
        if (ok) optics_push_lens(optics, lens);

        slock_unlock(&optics->lock);
    }

    if (!ok) {
        optics_fail("lens '%s' already exists", lens_name(lens));
        return false;
    }

    return true;
}

static struct optics_lens *
optics_lens_open(struct optics *optics, struct optics_lens *lens)
{
    {
        slock_lock(&optics->lock);

        struct htable_ret ret = htable_put(&optics->keys, lens_name(lens), pun_ptoi(lens));

        if (ret.ok) optics_push_lens(optics, lens);
        else lens = pun_itop(ret.value);

        slock_unlock(&optics->lock);
    }

    return lens;
}

bool optics_lens_close(struct optics_lens *lens)
{
    bool ok;
    {
        slock_lock(&lens->optics->lock);

        ok = htable_del(&lens->optics->keys, lens_name(lens)).ok;
        if (ok) optics_remove_lens(lens->optics, lens);

        slock_unlock(&lens->optics->lock);
    }

    if (!ok) return false;
    if (!lens_defer_free(lens->optics, lens)) return false;
    return true;
}


enum optics_lens_type optics_lens_type(struct optics_lens *lens)
{
    return lens_type(lens);
}

const char * optics_lens_name(struct optics_lens *lens)
{
    return lens_name(lens);
}


// -----------------------------------------------------------------------------
// counter
// -----------------------------------------------------------------------------

struct optics_lens * optics_counter_create(struct optics *optics, const char *name)
{
    struct optics_lens *counter = lens_counter_alloc(optics, name);
    if (!counter) return NULL;

    if (!optics_lens_create(optics, counter)) {
        lens_free(counter);
        return NULL;
    }

    return counter;
}

struct optics_lens * optics_counter_open(struct optics *optics, const char *name)
{
    struct optics_lens *counter = lens_counter_alloc(optics, name);
    if (!counter) return NULL;

    struct optics_lens *lens = optics_lens_open(optics, counter);
    if (lens != counter) lens_free(counter);

    return lens;
}

bool optics_counter_inc(struct optics_lens *lens, int64_t value)
{
    return lens_counter_inc(lens, optics_epoch(lens->optics), value);
}

enum optics_ret
optics_counter_read(struct optics_lens *lens, optics_epoch_t epoch, int64_t *value)
{
    return lens_counter_read(lens, epoch, value);
}

// -----------------------------------------------------------------------------
// quantile
// -----------------------------------------------------------------------------

struct optics_lens * optics_quantile_create(
        struct optics *optics,
        const char *name,
        double target_quantile,
        double estimate,
        double adjustment)
{
    struct optics_lens *quantile =
        lens_quantile_alloc(optics, name, target_quantile, estimate, adjustment);
    if (!quantile) return NULL;

    if (!optics_lens_create(optics, quantile)) {
        lens_free(quantile);
        return NULL;
    }

    return quantile;
}

struct optics_lens * optics_quantile_open(
        struct optics *optics,
        const char *name,
        double target_quantile,
        double estimate,
        double adjustment)
{
    struct optics_lens *quantile =
        lens_quantile_alloc(optics, name, target_quantile, estimate, adjustment);
    if (!quantile) return NULL;

    struct optics_lens *lens = optics_lens_open(optics, quantile);
    if (lens != quantile) lens_free(quantile);

    return lens;
}

bool optics_quantile_update(struct optics_lens *lens, double value) {
    return lens_quantile_update(lens, optics_epoch(lens->optics), value);
}

enum optics_ret optics_quantile_read(
        struct optics_lens *lens, optics_epoch_t epoch, struct optics_quantile *value)
{
    return lens_quantile_read(lens, epoch, value);
}


// -----------------------------------------------------------------------------
// gauge
// -----------------------------------------------------------------------------

struct optics_lens * optics_gauge_create(struct optics *optics, const char *name)
{
    struct optics_lens *gauge = lens_gauge_alloc(optics, name);
    if (!gauge) return NULL;

    if (!optics_lens_create(optics, gauge)) {
        lens_free(gauge);
        return NULL;
    }

    return gauge;
}

struct optics_lens * optics_gauge_open(struct optics *optics, const char *name)
{
    struct optics_lens *gauge = lens_gauge_alloc(optics, name);
    if (!gauge) return NULL;

    struct optics_lens *lens = optics_lens_open(optics, gauge);
    if (lens != gauge) lens_free(gauge);

    return lens;
}

bool optics_gauge_set(struct optics_lens *lens, double value)
{
    return lens_gauge_set(lens, optics_epoch(lens->optics), value);
}

enum optics_ret
optics_gauge_read(struct optics_lens *lens, optics_epoch_t epoch, double *value)
{
    return lens_gauge_read(lens, epoch, value);
}


// -----------------------------------------------------------------------------
// dist
// -----------------------------------------------------------------------------

struct optics_lens * optics_dist_create(struct optics *optics, const char *name)
{
    struct optics_lens *dist = lens_dist_alloc(optics, name);
    if (!dist) return NULL;

    if (!optics_lens_create(optics, dist)) {
        lens_free(dist);
        return NULL;
    }

    return dist;
}

struct optics_lens * optics_dist_open(struct optics *optics, const char *name)
{
    struct optics_lens *dist = lens_dist_alloc(optics, name);
    if (!dist) return NULL;

    struct optics_lens *lens = optics_lens_open(optics, dist);
    if (lens != dist) lens_free(dist);

    return lens;
}

bool optics_dist_record(struct optics_lens *lens, double value)
{
    return lens_dist_record(lens, optics_epoch(lens->optics), value);
}

enum optics_ret
optics_dist_read(struct optics_lens *lens, optics_epoch_t epoch, struct optics_dist *value)
{
    return lens_dist_read(lens, epoch, value);
}


// -----------------------------------------------------------------------------
// histo
// -----------------------------------------------------------------------------

struct optics_lens * optics_histo_create(
        struct optics *optics, const char *name, const uint64_t *buckets, size_t buckets_len)
{
    struct optics_lens *histo = lens_histo_alloc(optics, name, buckets, buckets_len);
    if (!histo) return NULL;

    if (!optics_lens_create(optics, histo)) {
        lens_free(histo);
        return NULL;
    }

    return histo;
}

struct optics_lens * optics_histo_open(
        struct optics *optics, const char *name, const uint64_t *buckets, size_t buckets_len)
{
    struct optics_lens *histo = lens_histo_alloc(optics, name, buckets, buckets_len);
    if (!histo) return NULL;

    struct optics_lens *lens = optics_lens_open(optics, histo);
    if (lens != histo) lens_free(histo);

    return lens;
}

bool optics_histo_inc(struct optics_lens *lens, double value)
{
    return lens_histo_inc(lens, optics_epoch(lens->optics), value);
}

enum optics_ret
optics_histo_read(struct optics_lens *lens, optics_epoch_t epoch, struct optics_histo *value)
{
    return lens_histo_read(lens, epoch, value);
}


// -----------------------------------------------------------------------------
// value
// -----------------------------------------------------------------------------

bool optics_poll_normalize(
        const struct optics_poll *poll, optics_normalize_cb_t cb, void *ctx)
{
    switch (poll->type) {
    case optics_counter: return lens_counter_normalize(poll, cb, ctx);
    case optics_gauge: return lens_gauge_normalize(poll, cb, ctx);
    case optics_dist: return lens_dist_normalize(poll, cb, ctx);
    case optics_histo: return lens_histo_normalize(poll, cb, ctx);
    case optics_quantile: return lens_quantile_normalize(poll, cb, ctx);
    default:
        optics_fail("unknown lens type '%d'", poll->type);
        return false;
    }
}


// -----------------------------------------------------------------------------
// misc
// -----------------------------------------------------------------------------

extern inline void optics_timer_start(optics_timer_t *t0);
extern inline double optics_timer_elapsed(optics_timer_t *t0, double scale);
