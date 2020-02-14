/* poller_poll.c
   Rémi Attab (remi.attab@gmail.com), 15 Mar 2016
   FreeBSD-style copyright and disclaimer apply
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
    struct optics_poller *poller;

    optics_ts_t ts;
    optics_ts_t elapsed;

    const char *host;
    const char *prefix;

    optics_epoch_t epoch;
};


// -----------------------------------------------------------------------------
// lens
// -----------------------------------------------------------------------------

static enum optics_ret poller_poll_lens(void *ctx_, struct optics_lens *lens)
{
    struct poller_poll_ctx *ctx = ctx_;

    struct optics_key key = {0};
    optics_key_push(&key, ctx->prefix);
    optics_key_push(&key, ctx->host);
    optics_key_push(&key, optics_lens_name(lens));

    enum optics_ret ret;
    struct optics_poll poll = (struct optics_poll) {
        .type = optics_lens_type(lens),

        .host = ctx->host,
        .prefix = ctx->prefix,
        .key = optics_lens_name(lens),

        .ts = ctx->ts,
        .elapsed = ctx->elapsed,
    };

    switch (poll.type) {
    case optics_counter:
        ret = optics_counter_read(lens, ctx->epoch, &poll.value.counter);
        break;

    case optics_gauge:
        ret = optics_gauge_read(lens, ctx->epoch, &poll.value.gauge);
        break;

    case optics_dist:
        ret = optics_dist_read(lens, ctx->epoch, &poll.value.dist);
        break;

    case optics_histo:
        ret = optics_histo_read(lens, ctx->epoch, &poll.value.histo);
        break;

    case optics_quantile:
        ret = optics_quantile_read(lens, ctx->epoch, &poll.value.quantile);
        break;

    default:
        optics_fail("unknown poller type '%d'", poll.type);
        ret = optics_err;
        break;
    }

    if (ret == optics_ok)
        poller_backend_record(ctx->poller, optics_poll_metric, &poll);
    else if (ret == optics_busy)
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
        optics_epoch_t epoch)
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
        .poller = poller,
        .ts = ts,
        .elapsed = elapsed,

        .host = optics_poller_get_host(poller),
        .prefix = optics_get_prefix(poller->optics),

        .epoch = epoch,
    };

    (void) optics_foreach_lens(poller->optics, &ctx, poller_poll_lens);
}

bool optics_poller_poll(struct optics_poller *poller)
{
    return optics_poller_poll_at(poller, clock_wall());
}

bool optics_poller_poll_at(struct optics_poller *poller, optics_ts_t ts)
{
    optics_ts_t last_poll = 0;
    optics_epoch_t epoch = optics_epoch_inc_at(poller->optics, ts, &last_poll);

    // give a chance for stragglers to finish. We'd need full EBR to do this
    // properly but that would add overhead on the record side so we instead
    // just wait a bit and deal with stragglers if we run into them.
    nsleep(1 * 1000 * 1000);

    poller_backend_record(poller, optics_poll_begin, NULL);
    poller_poll_optics(poller, ts, last_poll, epoch);
    poller_backend_record(poller, optics_poll_done, NULL);

    return true;
}
