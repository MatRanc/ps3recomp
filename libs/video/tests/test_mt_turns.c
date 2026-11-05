/* Unit test for the walker-MT ordered-commit turn protocol.
 *
 * Models libs/video/rsx_draw_engine.c: the state thread allocates a turn for
 * every texture group at the bind and one for the packet at finalise; a
 * worker segments its chunk at group-epoch changes and takes the turns in
 * order; the resolver waits for the group to close and for its turn, then
 * advances tex_taken.  The shapes are the ones that appeared live: a group
 * inside a chunk, several groups before one packet, a group whose packet was
 * dropped before it got a seq, and a chunk boundary on a group.  Each must
 * finish, with every packet committed once, within the watchdog.
 *
 * Portions derived from inFAMOUS 2 recomp (c) 2026 the infamous2-recomp
 * contributors, MIT.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PKTS 256
#define MAX_GROUPS 256
#define NWORKERS 4
#define CHUNK_PKTS 16

typedef struct {
    int tex_epoch;
    int committed;
} pkt;

typedef struct {
    pkt p[MAX_PKTS];
    int n;
    unsigned long long pkt_seq[MAX_PKTS];
    unsigned long long grp_seq[MAX_GROUPS];
    unsigned int grp_epoch[MAX_GROUPS];
    int n_groups;
    unsigned long long last_seq;

    atomic_ullong next_commit;
    atomic_uint tex_epoch;
    atomic_uint tex_taken;
    atomic_int group_open;
    atomic_ullong closed_seq;
    atomic_int shutdown;
    pthread_mutex_t mu;
    pthread_cond_t gcv;

    int chunk_start[MAX_PKTS], chunk_len[MAX_PKTS], n_chunks;
} model;

static void fail(const char *what)
{
    fprintf(stderr, "test_mt_turns: FAIL: %s\n", what);
    exit(1);
}
static void turn_wait(model *m, unsigned long long seq)
{
    while (atomic_load(&m->next_commit) != seq) sched_yield();
}
static void turn_take(model *m, unsigned long long seq)
{
    atomic_store(&m->next_commit, seq + 1);
}
static void group_close(model *m, unsigned long long gs)
{
    atomic_store(&m->closed_seq, gs);
    atomic_store(&m->group_open, 0);
    pthread_mutex_lock(&m->mu);
    pthread_cond_broadcast(&m->gcv);
    pthread_mutex_unlock(&m->mu);
}

/* State thread. binds[i] = number of groups opened during packet i's prepare;
 * a group whose packet is dropped still consumes its turn, so it appears in
 * the group list without a packet seq (the caller models that by putting the
 * group count on the *next* packet, as the engine's open group does). */
static void build(model *m, const int *binds, int n)
{
    memset(m, 0, sizeof *m);
    atomic_init(&m->next_commit, 1);
    atomic_init(&m->tex_epoch, 0);
    atomic_init(&m->tex_taken, 0);
    atomic_init(&m->group_open, 0);
    atomic_init(&m->closed_seq, 0);
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->gcv, NULL);
    m->n = n;
    unsigned long long s = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < binds[i]; k++) {
            m->grp_seq[m->n_groups] = ++s;
            m->grp_epoch[m->n_groups] = atomic_fetch_add(&m->tex_epoch, 1) + 1;
            /* Each group is closed at its packet's finalise; a group whose
             * packet is dropped stays open until the next packet closes it,
             * which is still before any worker sees the chunk. */
            group_close(m, s);
            m->n_groups++;
        }
        m->p[i].tex_epoch = (int)atomic_load(&m->tex_epoch);
        m->pkt_seq[i] = ++s;
    }
    m->last_seq = s;
}

static void *resolver(void *arg)
{
    model *m = arg;
    for (int g = 0; g < m->n_groups; g++) {
        unsigned long long gs = m->grp_seq[g];
        pthread_mutex_lock(&m->mu);
        while (atomic_load(&m->closed_seq) < gs)
            pthread_cond_wait(&m->gcv, &m->mu);
        pthread_mutex_unlock(&m->mu);
        turn_wait(m, gs);
        atomic_store(&m->tex_taken, m->grp_epoch[g]);
        turn_take(m, gs);
    }
    return NULL;
}

typedef struct { model *m; int w; } worker_arg;

static void *worker(void *arg)
{
    worker_arg *wa = arg;
    model *m = wa->m;
    for (int c = wa->w; c < m->n_chunks; c += NWORKERS) {
        int first = m->chunk_start[c];
        int end = first + m->chunk_len[c];
        int seg_open = 0, prev_epoch = 0;
        unsigned long long seg_first = 0, seg_last = 0;
        for (int i = first; i < end; i++) {
            pkt *p = &m->p[i];
            unsigned long long seq = m->pkt_seq[i];
            if (seg_open && p->tex_epoch > prev_epoch) {
                turn_wait(m, seg_first);
                turn_take(m, seg_last);
                seg_open = 0;
            }
            if (!seg_open) {
                while ((int)atomic_load(&m->tex_taken) < p->tex_epoch) sched_yield();
                seg_open = 1;
                seg_first = seq;
            }
            p->committed = 1;
            seg_last = seq;
            prev_epoch = p->tex_epoch;
        }
        if (seg_open) {
            turn_wait(m, seg_first);
            turn_take(m, seg_last);
        }
    }
    return NULL;
}

static void run(const char *name, const int *binds, int n)
{
    model m;
    build(&m, binds, n);
    m.n_chunks = 0;
    for (int i = 0; i < n; i += CHUNK_PKTS) {
        m.chunk_start[m.n_chunks] = i;
        m.chunk_len[m.n_chunks] = n - i < CHUNK_PKTS ? n - i : CHUNK_PKTS;
        m.n_chunks++;
    }
    pthread_t rt, wt[NWORKERS];
    worker_arg wa[NWORKERS];
    if (pthread_create(&rt, NULL, resolver, &m) != 0) fail("resolver create");
    for (int w = 0; w < NWORKERS; w++) {
        wa[w].m = &m; wa[w].w = w;
        if (pthread_create(&wt[w], NULL, worker, &wa[w]) != 0) fail("worker create");
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    int stuck = 0;
    for (int w = 0; w < NWORKERS; w++)
        if (pthread_timedjoin_np(wt[w], NULL, &ts) != 0) { stuck = 1; break; }
    if (!stuck && pthread_timedjoin_np(rt, NULL, &ts) != 0) stuck = 1;
    if (stuck) {
        fprintf(stderr, "test_mt_turns: %s: DEADLOCK next_commit=%llu last=%llu"
                " tex_taken=%u tex_epoch=%u\n", name,
                (unsigned long long)atomic_load(&m.next_commit), m.last_seq,
                atomic_load(&m.tex_taken), atomic_load(&m.tex_epoch));
        exit(1);
    }
    if (atomic_load(&m.next_commit) != m.last_seq + 1)
        fail("next_commit did not follow the last turn");
    if (atomic_load(&m.tex_taken) != atomic_load(&m.tex_epoch))
        fail("resolver did not take every group");
    for (int i = 0; i < n; i++)
        if (!m.p[i].committed) fail("a packet was not committed");
    printf("test_mt_turns: %-26s ok (%d packets, %d groups)\n",
           name, n, m.n_groups);
}

int main(void)
{
    static int binds[MAX_PKTS];
    for (int i = 0; i < 64; i++) binds[i] = 1;
    run("group-every-packet", binds, 64);
    for (int i = 0; i < 64; i++) binds[i] = (i % 3 == 0);
    run("group-every-third", binds, 64);
    for (int i = 0; i < 64; i++) binds[i] = (i % 5 == 0) ? 2 : 0;
    run("two-groups-one-packet", binds, 64);
    for (int i = 0; i < 64; i++) binds[i] = (i == 16 || i == 32 || i == 48);
    run("group-at-chunk-edge", binds, 64);
    for (int i = 0; i < 64; i++) binds[i] = 0;
    run("no-groups", binds, 64);
    printf("test_mt_turns: all scenarios ok\n");
    return 0;
}
