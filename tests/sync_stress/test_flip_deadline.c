/* The state machine that caps the title's 2 x 16683 us flip deadline
 * (runtime/ppu/flip_deadline.h). Pure, header-only, no host threads. */
#include "../../runtime/ppu/flip_deadline.h"
#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    const long long deadline = 33366;
    flip_deadline_state s = {0, 0};
    int force = -1;

    /* First wait: full guest timeout, starts the run. */
    long long t = 1000;
    CHECK(flip_deadline_timeout_us(&s, t, 17000, deadline, &force) == 17000);
    CHECK(s.run == 1 && s.start_us == t && force == 0);

    /* First timeout keeps the run so the next wait is capped. */
    flip_deadline_wait_result(&s, 1);
    CHECK(s.run == 1);

    /* Second wait: 33366 - 17500 remaining + margin. */
    CHECK(flip_deadline_timeout_us(&s, t + 17500, 17000, deadline, &force) ==
          (deadline - 17500) + FLIP_DEADLINE_MARGIN_US);
    CHECK(s.run == 2 && force == 0);

    /* Second timeout: a third call must not wait another 17 ms. */
    flip_deadline_wait_result(&s, 1);
    CHECK(s.run == 2);
    CHECK(flip_deadline_timeout_us(&s, t + 33400, 17000, deadline, &force) == 0);
    CHECK(force == 1 && s.run == 2);

    /* A new frame's wait after the stale window starts a fresh run. */
    CHECK(flip_deadline_timeout_us(&s, t + deadline + FLIP_DEADLINE_STALE_US + 1,
                                   17000, deadline, &force) == 17000);
    CHECK(s.run == 1 && force == 0 && s.start_us == t + deadline + FLIP_DEADLINE_STALE_US + 1);

    /* A signal at any point ends the run. */
    flip_deadline_wait_result(&s, 0);
    CHECK(s.run == 0);

    /* If the first wait ends by signal, the next one is a fresh full wait. */
    s.run = 0;
    CHECK(flip_deadline_timeout_us(&s, 50000, 17000, deadline, &force) == 17000);
    flip_deadline_wait_result(&s, 0);
    CHECK(flip_deadline_timeout_us(&s, 60000, 17000, deadline, &force) == 17000);
    CHECK(s.run == 1);

    /* Already past the deadline at the second call: force, do not wait. */
    s.run = 1; s.start_us = 0;
    CHECK(flip_deadline_timeout_us(&s, deadline + 1, 17000, deadline, &force) == 0);
    CHECK(force == 1 && s.run == 2);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("flip_deadline: PASS\n");
    return 0;
}
