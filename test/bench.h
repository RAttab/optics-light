/* bench.h
   Rémi Attab (remi.attab@gmail.com), 23 Jan 2016
   FreeBSD-style copyright and disclaimer apply

   TODO: Framework needs to be reworked as it computes a distribution of
   averages which is bad (tm).
*/

#pragma once

#include "test.h"

// -----------------------------------------------------------------------------
// bench
// -----------------------------------------------------------------------------

struct optics_bench;
typedef void (* optics_bench_fn_t) (
        struct optics_bench *, void * ctx, size_t id, size_t n);

void *optics_bench_setup(struct optics_bench *, void *data);
void optics_bench_start(struct optics_bench *);
void optics_bench_stop(struct optics_bench *);

void optics_bench_st(const char *title, optics_bench_fn_t fn, void *ctx);
void optics_bench_mt(const char *title, optics_bench_fn_t fn, void *ctx);
