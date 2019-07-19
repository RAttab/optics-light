/* poller_poll.c
   Rémi Attab (remi.attab@gmail.com), 15 Mar 2016
   FreeBSD-style copyright and disclaimer apply

   \todo Get rid of all the merging logic
*/


// -----------------------------------------------------------------------------
// config
// -----------------------------------------------------------------------------

enum { poller_max_optics = 128 };


// -----------------------------------------------------------------------------
// struct
// -----------------------------------------------------------------------------

struct poller_poll_ctx
{
    optics_ts_t ts;
    optics_ts_t elapsed;

    const char *host;
    const char *prefix;

    optics_epoch_t epoch;
    struct htable *values;
};


// -----------------------------------------------------------------------------
// lens
// -----------------------------------------------------------------------------


static struct optics_poll *poller_get_value(
        struct poller_poll_ctx *ctx,
        struct optics_lens *lens,
        struct optics_key *key)
{
    struct htable_ret ret = htable_get(ctx->values, key->data);
    if (ret.ok) {
        struct optics_poll *poll = pun_itop(ret.value);

        // This might skew the results but trying to normalize the values first
        // would complicate things a great deal and the skew should be temporary.
        if (ctx->elapsed > poll->elapsed) poll->elapsed = ctx->elapsed;

        return poll;
    }

    struct optics_poll *poll = calloc(1, sizeof(*poll));
    optics_assert_alloc(poll);

    *poll = (struct optics_poll) {
        .type = optics_lens_type(lens),

        .host = ctx->host,
        .prefix = ctx->prefix,
        .key = optics_lens_name(lens),

        .ts = ctx->ts,
        .elapsed = ctx->elapsed,
    };

    ret = htable_put(ctx->values, key->data, pun_ptoi(poll));
    optics_assert(ret.ok, "unable to insert '%s' in value table", key->data);
    return poll;
}

static enum optics_ret poller_poll_lens(void *ctx_, struct optics_lens *lens)
{
    struct poller_poll_ctx *ctx = ctx_;

    struct optics_key key = {0};
    optics_key_push(&key, ctx->prefix);
    optics_key_push(&key, ctx->host);
    optics_key_push(&key, optics_lens_name(lens));

    enum optics_ret ret;
    struct optics_poll *poll = poller_get_value(ctx, lens, &key);

    switch (poll->type) {
    case optics_counter:
        ret = optics_counter_read(lens, ctx->epoch, &poll->value.counter);
        break;

    case optics_gauge:
        ret = optics_gauge_read(lens, ctx->epoch, &poll->value.gauge);
        break;

    case optics_dist:
        ret = optics_dist_read(lens, ctx->epoch, &poll->value.dist);
        break;

    case optics_histo:
        ret = optics_histo_read(lens, ctx->epoch, &poll->value.histo);
        break;

    case optics_quantile:
        ret = optics_quantile_read(lens, ctx->epoch, &poll->value.quantile);
        break;

    default:
        optics_fail("unknown poller type '%d'", poll->type);
        ret = optics_err;
        break;
    }

    if (ret == optics_busy)
        optics_warn("skipping lens '%s'", key.data);
    else if (ret == optics_err)
        optics_warn("unable to read lens '%s': %s", key.data, optics_errno.msg);

    return optics_ok;
}


// -----------------------------------------------------------------------------
// poll
// -----------------------------------------------------------------------------

static void poller_poll_optics(
        struct optics_poller *poller,
        optics_ts_t ts,
        optics_ts_t last_poll,
        optics_epoch_t epoch,
        struct htable *values)
{
    optics_ts_t elapsed = 0;
    if (ts > last_poll) elapsed = ts - last_poll;
    else if (ts == last_poll) elapsed = 1;
    else {
            elapsed = 1;
            optics_warn("clock out of sync for '%s': optics=%lu, poller=%lu",
                    optics_get_prefix(poller->optics), last_poll, ts);
    }
    assert(elapsed > 0);

    struct poller_poll_ctx ctx = {
        .ts = ts,
        .elapsed = elapsed,

        .host = optics_poller_get_host(poller),
        .prefix = optics_get_prefix(poller->optics),

        .epoch = epoch,
        .values = values,
    };

    (void) optics_foreach_lens(poller->optics, &ctx, poller_poll_lens);
}

bool optics_poller_poll(struct optics_poller *poller)
{
    return optics_poller_poll_at(poller, clock_wall());
}

bool optics_poller_poll_at(struct optics_poller *poller, optics_ts_t ts)
{
    optics_ts_t last_poll;
    optics_epoch_t epoch = optics_epoch_inc_at(poller->optics, ts, &last_poll);

    // give a chance for stragglers to finish. We'd need full EBR to do this
    // properly but that would add overhead on the record side so we instead
    // just wait a bit and deal with stragglers if we run into them.
    nsleep(1 * 1000 * 1000);

    struct htable values = {0};
    poller_poll_optics(poller, epoch, ts, last_poll, &values);

    poller_backend_record(poller, optics_poll_begin, NULL);

    struct htable_bucket *bucket;
    for (bucket = htable_next(&values, NULL); bucket; bucket = htable_next(&values, bucket)) {
        struct optics_poll *poll = pun_itop(bucket->value);
        poller_backend_record(poller, optics_poll_metric, poll);
        free(poll);
    }

    poller_backend_record(poller, optics_poll_done, NULL);

    htable_reset(&values);
    return true;
}
