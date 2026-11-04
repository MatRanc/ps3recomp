/*
 * The title's flip-done wait is a 2 x 16683 us deadline (59.94 Hz x 2) that
 * Gfx_DisplayFlip polls with Cond_Wait(..., 17000). Two full 17 ms host
 * timeouts therefore leave the deadline branch at ~34.1 ms instead of the
 * guest's 33.366 ms. This state machine caps the second wait to the remaining
 * time to the guest's deadline, so the branch is reached on the tick the guest
 * asked for without making the first wait any shorter.
 *
 * run == 0: no run in progress, the next wait starts one at its full timeout.
 * run == 1: the first wait timed out; the second is capped to the remainder.
 * run >= 2: the deadline has passed; a further wait returns ETIMEDOUT at once
 *           until the state goes stale, so jitter cannot turn the run into a
 *           50 ms third timeout. A stale run is discarded after the deadline.
 */
#ifndef PS3RECOMP_FLIP_DEADLINE_H
#define PS3RECOMP_FLIP_DEADLINE_H

#define FLIP_DEADLINE_MARGIN_US 200
#define FLIP_DEADLINE_STALE_US  2000

typedef struct {
    int       run;
    long long start_us;
} flip_deadline_state;

/* Returns the timeout to use for this wait; sets *force_timeout when the
 * caller must return ETIMEDOUT without waiting. `requested_us` and
 * `deadline_us` are microseconds; the deadline is measured from the first
 * wait's entry. */
static inline long long flip_deadline_timeout_us(flip_deadline_state* s,
                                                 long long now_us,
                                                 long long requested_us,
                                                 long long deadline_us,
                                                 int* force_timeout)
{
    *force_timeout = 0;
    if (s->run && now_us - s->start_us > deadline_us + FLIP_DEADLINE_STALE_US)
        s->run = 0;                         /* a new frame's run */
    if (s->run == 0) {
        s->start_us = now_us;
        s->run = 1;
        return requested_us;
    }
    if (s->run == 1) {
        long long rem = deadline_us - (now_us - s->start_us);
        s->run = 2;                         /* this is the capped wait */
        if (rem <= 0) { *force_timeout = 1; return 0; }
        rem += FLIP_DEADLINE_MARGIN_US;     /* cover the guest's earlier start */
        return rem < requested_us ? rem : requested_us;
    }
    *force_timeout = 1;                     /* deadline passed; do not wait */
    return 0;
}

/* Call after the wait. A signal ends the run; a timeout keeps run == 1 so the
 * next wait is capped, or run >= 2 so any extra wait returns at once. */
static inline void flip_deadline_wait_result(flip_deadline_state* s, int timedout)
{
    if (!timedout) s->run = 0;
}

#endif /* PS3RECOMP_FLIP_DEADLINE_H */
