/* lens_quantile.c
   Marina C., 19 Feb 2018
   FreeBSD-style copyright and disclaimer apply
*/

// -----------------------------------------------------------------------------
// struct
// -----------------------------------------------------------------------------

struct lens_quantile
{
     double target_quantile;
     double original_estimate;
     double adjustment_value;
     atomic_int_fast64_t multiplier;
     atomic_int_fast64_t count[2];
};

// -----------------------------------------------------------------------------
// impl
// -----------------------------------------------------------------------------

static struct optics_lens *
lens_quantile_alloc(
        struct optics *optics,
        const char *name,
        double target_quantile,
        double original_estimate,
        double adjustment_value)
{
    struct optics_lens *lens = lens_alloc(optics, optics_quantile, sizeof(struct lens_quantile), name);
    if (!lens) goto fail_alloc;

    struct lens_quantile *quantile = lens_sub_ptr(lens, optics_quantile);
    if (!quantile) goto fail_sub;

    quantile->target_quantile = target_quantile;
    quantile->original_estimate = original_estimate;
    quantile->adjustment_value = adjustment_value;

    return lens;

  fail_sub:
    lens_free(lens);
  fail_alloc:
    return NULL;
}

static double calculate_quantile(struct lens_quantile *quantile)
{
    size_t adjustment =
        atomic_load_explicit(&quantile->multiplier, memory_order_relaxed) *
        quantile->adjustment_value;

    return quantile->original_estimate + adjustment;
}

static bool
lens_quantile_update(struct optics_lens *lens, optics_epoch_t epoch, double value)
{
    struct lens_quantile *quantile = lens_sub_ptr(lens, optics_quantile);
    if (!quantile) return false;

    double current_estimate = calculate_quantile(quantile);
    bool probability_check = rng_gen_prob(rng_global(), quantile->target_quantile);

    if (value < current_estimate) {
        if (!probability_check)
            atomic_fetch_sub_explicit(&quantile->multiplier, 1, memory_order_relaxed);
    }
    else {
        if (probability_check)
             atomic_fetch_add_explicit(&quantile->multiplier, 1, memory_order_relaxed);
    }

    // Since we don't care too much how exact the count is (not used to modify
    // our estimates) then the write ordering doesn't matter so relaxed is fine.
    atomic_fetch_add_explicit(&quantile->count[epoch], 1, memory_order_relaxed);

    return true;
}

static enum optics_ret
lens_quantile_read(
        struct optics_lens *lens, optics_epoch_t epoch, struct optics_quantile *value)
{
    struct lens_quantile *quantile = lens_sub_ptr(lens, optics_quantile);
    if (!quantile) return optics_err;

    value->quantile = quantile->target_quantile;
    value->sample = calculate_quantile(quantile);
    value->count =
        atomic_exchange_explicit(&quantile->count[epoch], 0, memory_order_relaxed);
    
    return optics_ok;
}

static bool
lens_quantile_normalize(
        const struct optics_poll *poll, optics_normalize_cb_t cb, void *ctx)
{
    return cb(ctx, poll->ts, poll->key, poll->value.quantile.sample);
}
