/*
 * replay_rsx_engine <capture-prefix> [options]
 *
 * Times one captured pitch frame through the real register-file draw engine
 * and a null backend: the walker, dispatcher, vertex fetch/remap, texture
 * decode/hash, pipeline key and bind traffic are the shipping code; only
 * Vulkan command recording is absent. The per-frame phase split comes from
 * PS3RECOMP_WALKER_PROF (enabled here unless it is explicitly set to 0).
 */
#include "replay_rsx_engine_core.h"
#include "rsx_draw_engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <cmath>

static void usage(const char* p)
{
    std::printf("usage: %s <capture-prefix> [--frames N] [--warmup N] [--verbose] [--csv]\n", p);
}

int main(int argc, char** argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    const char* prefix = argv[1];
    int frames = 50, warmup = 5;
    bool verbose = false, csv = false;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = std::max(0, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else if (!std::strcmp(argv[i], "--csv")) csv = true;
        else { std::fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    const char* prof = std::getenv("PS3RECOMP_WALKER_PROF");
    if (!prof) setenv("PS3RECOMP_WALKER_PROF", "1", 1);

    char err[256] = {0};
    rsx_engine_replay* r = rsx_engine_replay_create(prefix, err, sizeof err);
    if (!r) { std::fprintf(stderr, "replay_rsx_engine: %s\n", err); return 1; }

    rsx_engine_replay_stats st = {0};
    for (int i = 0; i < warmup; i++)
        if (rsx_engine_replay_run_frame(r, &st) != 0) { std::fprintf(stderr, "warmup walker failed\n"); return 1; }
    const bool gpu_stats = std::getenv("PS3RECOMP_GPU_STATS") != nullptr;
    if (gpu_stats) rsx_draw_engine_gpu_range_stats_reset();

    std::vector<double> us;
    us.reserve(frames);
    uint64_t draws = 0, verts = 0, hits = 0, misses = 0;
    uint64_t gate = 1469598103934665603ull, last_hash = 0;
    for (int i = 0; i < frames; i++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        int rc = rsx_engine_replay_run_frame(r, &st);
        auto t1 = std::chrono::high_resolution_clock::now();
        if (rc != 0) { std::fprintf(stderr, "frame %d walker failed\n", i); return 1; }
        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        draws += st.draws; verts += st.vertices;
        hits += st.read_hits; misses += st.read_misses;
        last_hash = st.frame_hash;
        gate = (gate ^ st.frame_hash) * 1099511628211ull;
        if (verbose)
            std::printf("frame %3d: %.2f us, %llu draws, %llu read misses, hash %016llx\n", i,
                        us.back(), (unsigned long long)st.draws,
                        (unsigned long long)st.read_misses,
                        (unsigned long long)st.frame_hash);
    }

    std::vector<double> sorted = us;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    const double mean = std::accumulate(us.begin(), us.end(), 0.0) / us.size();
    const double p95 = sorted[(size_t)(sorted.size() * 0.95) < sorted.size()
                              ? (size_t)(sorted.size() * 0.95) : sorted.size() - 1];
    const double avg_draws = (double)draws / frames;

    if (csv) {
        std::printf("capture,words,frames,median_us,mean_us,p95_us,avg_draws,read_hits,read_misses,last_hash,gate\n");
        std::printf("%s,%u,%d,%.2f,%.2f,%.2f,%.1f,%llu,%llu,%016llx,%016llx\n",
                    prefix, st.words, frames, med, mean, p95, avg_draws,
                    (unsigned long long)hits, (unsigned long long)misses,
                    (unsigned long long)last_hash, (unsigned long long)gate);
    } else {
        std::printf("capture:      %s\n", prefix);
        std::printf("frames:       %d (+%d warmup), %u words\n", frames, warmup, st.words);
        std::printf("median frame: %.3f ms (mean %.3f, p95 %.3f)\n", med / 1000.0, mean / 1000.0, p95 / 1000.0);
        std::printf("draws/frame:  %.1f\n", avg_draws);
        std::printf("guest reads:  %llu hit, %llu miss\n",
                    (unsigned long long)hits, (unsigned long long)misses);
        std::printf("output hash:  %016llx (last measured frame)\n",
                    (unsigned long long)last_hash);
        std::printf("output gate:  %016llx (%d measured frames)\n",
                    (unsigned long long)gate, frames);
    }
    if (rsx_draw_engine_walker_fast_enabled()) {
        rsx_walker_fast_stats fs = {0};
        rsx_draw_engine_walker_fast_stats(&fs);
        std::printf("fast path:    fetch %llu hit / %llu miss, pipeline %llu hit / %llu miss\n",
                    fs.fetch_hits, fs.fetch_misses, fs.pipeline_hits, fs.pipeline_misses);
    }
    if (gpu_stats) {
        rsx_gpu_range_stats gs = {0};
        rsx_draw_engine_gpu_range_stats_get(&gs);
        std::printf("gpu range:    probes %llu (%.1f/frame), entries %llu (%.1f/probe), "
                    "full %llu, partial %llu, miss %llu, pending %llu, "
                    "merges %llu (%.1f bytes)\n",
                    gs.probes, (double)gs.probes / frames,
                    gs.entries, gs.probes ? (double)gs.entries / gs.probes : 0.0,
                    gs.full, gs.partial, gs.miss, gs.pending,
                    gs.merges, gs.merges ? (double)gs.merge_bytes / gs.merges : 0.0);
    }

    rsx_engine_replay_destroy(r);
    return misses ? 2 : 0;
}
