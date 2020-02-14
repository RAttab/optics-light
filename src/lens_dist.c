/* lens_dist.c
   Rémi Attab (remi.attab@gmail.com), 25 Feb 2016
   FreeBSD-style copyright and disclaimer apply
*/

// -----------------------------------------------------------------------------
// struct
// -----------------------------------------------------------------------------

struct lens_dist_epoch
{
    struct slock lock;

    size_t n;
    double max;
    double samples[optics_dist_samples];
};

struct lens_dist
{
    struct lens_dist_epoch epochs[2];
};


// -----------------------------------------------------------------------------
// impl
// -----------------------------------------------------------------------------


static struct optics_lens *
lens_dist_alloc(struct optics *optics, const char *name)
{
    return lens_alloc(optics, optics_dist, sizeof(struct lens_dist), name);
}

static bool
lens_dist_record(struct optics_lens* lens, optics_epoch_t epoch, double value)
{
    struct lens_dist *dist_head = lens_sub_ptr(lens, optics_dist);
    if (!dist_head) return false;

    struct lens_dist_epoch *dist = &dist_head->epochs[epoch];
    {
        slock_lock(&dist->lock);

        size_t i = dist->n;
        if (i >= optics_dist_samples)
            i = rng_gen_range(rng_global(), 0, dist->n);
        if (i < optics_dist_samples)
            dist->samples[i] = value;

        dist->n++;
        if (value > dist->max) dist->max = value;

        slock_unlock(&dist->lock);
    }
    return true;
}


static int lens_dist_value_cmp(const void *lhs, const void *rhs)
{
    return *((double *) lhs) - *((double *) rhs);
}

static inline size_t lens_dist_p(size_t percentile, size_t n)
{
    return (n * percentile) / 100;
}

static size_t lens_dist_reservoir_len(size_t len)
{
    return len > optics_dist_samples ? optics_dist_samples : len;
}

static enum optics_ret
lens_dist_read(struct optics_lens *lens, optics_epoch_t epoch, struct optics_dist *value)
{
    struct lens_dist *dist_head = lens_sub_ptr(lens, optics_dist);
    if (!dist_head) return optics_err;

    struct lens_dist_epoch *dist = &dist_head->epochs[epoch];

    {
        // Since we're not locking the active epoch, we should only contend
        // with straglers which can be dealt with by the poller.
        if (slock_is_locked(&dist->lock)) return optics_busy;

        value->n = dist->n;
        if (value->max < dist->max) value->max = dist->max;

        size_t to_copy = lens_dist_reservoir_len(value->n);
        memcpy(value->samples, dist->samples, to_copy * sizeof(value->samples[0]));

        dist->max = 0;
        dist->n = 0;

        slock_unlock(&dist->lock);
    }

    if (!value->n) return optics_ok;

    size_t len = value->n <= optics_dist_samples ? value->n : optics_dist_samples;
    qsort(value->samples, len, sizeof(double), lens_dist_value_cmp);

    value->p50 = value->samples[lens_dist_p(50, len)];
    value->p90 = value->samples[lens_dist_p(90, len)];
    value->p99 = value->samples[lens_dist_p(99, len)];

    return optics_ok;
}


static bool
lens_dist_normalize(
        const struct optics_poll *poll, optics_normalize_cb_t cb, void *ctx)
{
    bool ret = false;
    size_t old;

    struct optics_key key = {0};
    optics_key_push(&key, poll->key);

    old = optics_key_push(&key, "count");
    ret = cb(ctx, poll->ts, key.data, lens_rescale(poll, poll->value.dist.n));
    optics_key_pop(&key, old);
    if (!ret) return false;

    old = optics_key_push(&key, "p50");
    ret = cb(ctx, poll->ts, key.data, poll->value.dist.p50);
    optics_key_pop(&key, old);
    if (!ret) return false;

    old = optics_key_push(&key, "p90");
    ret = cb(ctx, poll->ts, key.data, poll->value.dist.p90);
    optics_key_pop(&key, old);
    if (!ret) return false;

    old = optics_key_push(&key, "p99");
    ret = cb(ctx, poll->ts, key.data, poll->value.dist.p99);
    optics_key_pop(&key, old);
    if (!ret) return false;

    old = optics_key_push(&key, "max");
    ret = cb(ctx, poll->ts, key.data, poll->value.dist.max);
    optics_key_pop(&key, old);
    if (!ret) return false;

    return true;
}
