/*
 * RSX FIFO Replay & Benchmark CLI Tool
 *
 * Deterministically replays captured RSX FIFO command streams (.bin) through
 * the RSX FIFO walker and draw engine in a loop.
 *
 * Usage:
 *   replay_rsx <fifo_dump.bin> [options]
 *
 * Options:
 *   --frames <N>    Number of benchmark iterations (default: 50)
 *   --warmup <N>    Number of warmup iterations (default: 5)
 *   --engine        Use register-file draw engine (default)
 *   --legacy        Use legacy rsx_state vtable (PS3RECOMP_FIFO_LEGACY=1)
 *   --vtable        Disable draw engine, vtable only
 *   --verify        Verify state hash at every callback
 *   --csv           Output benchmark metrics in CSV format
 *   --verbose       Print per-frame timing details
 *   --help          Show help message
 */

#include "replay_rsx_core.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <cstring>

struct BenchmarkConfig {
    std::string file_path;
    int frames = 50;
    int warmup = 5;
    int engine_mode = 1; // 1 = engine, 2 = legacy, 0 = vtable
    bool verify = false;
    bool csv = false;
    bool verbose = false;
};

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " <fifo_dump.bin> [options]\n\n"
              << "Options:\n"
              << "  --frames <N>   Number of benchmark iterations (default: 50)\n"
              << "  --warmup <N>   Number of warmup iterations (default: 5)\n"
              << "  --engine       Use register-file draw engine (default)\n"
              << "  --legacy       Use legacy rsx_state vtable (PS3RECOMP_FIFO_LEGACY=1)\n"
              << "  --vtable       Disable draw engine; use vtable only\n"
              << "  --verify       Verify state hash at every dispatcher callback\n"
              << "  --csv          Output benchmark metrics in CSV format\n"
              << "  --verbose      Print per-frame timing details\n"
              << "  --help         Show this help message\n";
}

static bool parse_args(int argc, char** argv, BenchmarkConfig& cfg) {
    if (argc < 2) {
        print_usage(argv[0]);
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        } else if (arg == "--frames" && i + 1 < argc) {
            cfg.frames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--warmup" && i + 1 < argc) {
            cfg.warmup = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--engine") {
            cfg.engine_mode = 1;
        } else if (arg == "--legacy") {
            cfg.engine_mode = 2;
#ifdef _WIN32
            _putenv("PS3RECOMP_FIFO_LEGACY=1");
#else
            setenv("PS3RECOMP_FIFO_LEGACY", "1", 1);
#endif
        } else if (arg == "--vtable") {
            cfg.engine_mode = 0;
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--csv") {
            cfg.csv = true;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        } else {
            cfg.file_path = arg;
        }
    }

    if (cfg.file_path.empty()) {
        std::cerr << "Error: No FIFO dump binary file specified.\n";
        return false;
    }
    return true;
}

static bool load_fifo_dump(const std::string& path, uint32_t& io_addr, uint32_t& io_size,
                          std::vector<uint32_t>& words) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Error: Could not open file: " << path << "\n";
        return false;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (file_size < 4 || (file_size % 4) != 0) {
        std::cerr << "Error: File size must be a multiple of 4 bytes (got " << file_size << " bytes)\n";
        return false;
    }

    // Check for optional "RSXF" magic header
    char magic[4] = {0};
    file.read(magic, 4);
    file.seekg(0, std::ios::beg);

    io_addr = 0x00100000u;
    io_size = 0x00400000u;

    if (std::memcmp(magic, "RSXF", 4) == 0 && file_size >= (std::streamsize)sizeof(rsx_replay_file_header)) {
        rsx_replay_file_header hdr;
        file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (hdr.io_addr) io_addr = hdr.io_addr;
        if (hdr.io_size) io_size = hdr.io_size;
        size_t word_count = (file_size - sizeof(hdr)) / 4;
        words.resize(word_count);
        file.read(reinterpret_cast<char*>(words.data()), word_count * 4);
    } else {
        // Raw sequence of 32-bit words
        size_t word_count = file_size / 4;
        words.resize(word_count);
        file.read(reinterpret_cast<char*>(words.data()), file_size);
    }

    return true;
}

int main(int argc, char** argv) {
    BenchmarkConfig cfg;
    if (!parse_args(argc, argv, cfg)) return 1;

    uint32_t io_addr = 0, io_size = 0;
    std::vector<uint32_t> words;
    if (!load_fifo_dump(cfg.file_path, io_addr, io_size, words)) return 1;

    // Create replay context
    rsx_replay_context* ctx = rsx_replay_create(io_addr, io_size, cfg.engine_mode);
    if (!ctx) {
        std::cerr << "Error: Failed to allocate replay context or guest VM.\n";
        return 1;
    }

    if (rsx_replay_load_words(ctx, words.data(), words.size()) != 0) {
        std::cerr << "Error: Failed to load method words into IO memory.\n";
        rsx_replay_destroy(ctx);
        return 1;
    }

    if (!cfg.csv) {
        std::cout << "============================================================\n";
        std::cout << " RSX FIFO Deterministic Replay & Benchmark Tool\n";
        std::cout << "============================================================\n";
        std::cout << " File:           " << cfg.file_path << "\n";
        std::cout << " Method Words:   " << words.size() << " (" << (words.size() * 4 / 1024.0) << " KB)\n";
        std::cout << " Replay Engine:  " << (cfg.engine_mode == 1 ? "Register-File Draw Engine" :
                                            (cfg.engine_mode == 2 ? "Legacy rsx_state + Engine" : "Vtable Only")) << "\n";
        std::cout << " Verification:   " << (cfg.verify ? "ENABLED (Per-Call Hash)" : "Final State Hash") << "\n";
        std::cout << " Warmup Frames:  " << cfg.warmup << "\n";
        std::cout << " Benchmark Runs: " << cfg.frames << "\n";
        std::cout << "------------------------------------------------------------\n";
    }

    // Warmup phase
    for (int w = 0; w < cfg.warmup; ++w) {
        rsx_replay_frame_stats stats;
        rsx_replay_run_frame(ctx, &stats, 0);
    }

    // Timed benchmark phase
    std::vector<double> frame_times_us;
    frame_times_us.reserve(cfg.frames);

    uint64_t expected_hash = 0;
    uint64_t expected_calls = 0;
    uint32_t last_draws = 0;
    uint32_t last_clears = 0;
    uint32_t last_flips = 0;
    bool hash_mismatch = false;

    for (int f = 0; f < cfg.frames; ++f) {
        rsx_replay_frame_stats stats;
        auto t0 = std::chrono::high_resolution_clock::now();
        int rc = rsx_replay_run_frame(ctx, &stats, cfg.verify ? 1 : 0);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (rc != 0) {
            std::cerr << "Error: Frame " << f << " walker failed to reach put!\n";
            rsx_replay_destroy(ctx);
            return 1;
        }

        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        frame_times_us.push_back(us);

        if (f == 0) {
            expected_hash = stats.state_hash;
            expected_calls = stats.total_calls;
            last_draws = stats.draws;
            last_clears = stats.clears;
            last_flips = stats.flips;
        } else {
            if (stats.state_hash != expected_hash || stats.total_calls != expected_calls) {
                hash_mismatch = true;
                std::cerr << "WARNING: Non-deterministic execution detected at frame " << f << "!\n"
                          << "  Expected hash: 0x" << std::hex << expected_hash << ", Got: 0x" << stats.state_hash << "\n"
                          << "  Expected calls: " << std::dec << expected_calls << ", Got: " << stats.total_calls << "\n";
            }
        }

        if (cfg.verbose) {
            std::cout << "Frame " << std::setw(3) << f << ": "
                      << std::fixed << std::setprecision(2) << us << " us"
                      << " (" << stats.draws << " draws, " << stats.clears << " clears)\n";
        }
    }

    // Compute statistics
    std::sort(frame_times_us.begin(), frame_times_us.end());
    double min_us = frame_times_us.front();
    double max_us = frame_times_us.back();
    double sum_us = std::accumulate(frame_times_us.begin(), frame_times_us.end(), 0.0);
    double mean_us = sum_us / cfg.frames;
    double median_us = frame_times_us[cfg.frames / 2];
    double p95_us = frame_times_us[static_cast<size_t>(cfg.frames * 0.95)];

    double variance = 0.0;
    for (double us : frame_times_us) {
        variance += (us - mean_us) * (us - mean_us);
    }
    double stddev_us = std::sqrt(variance / cfg.frames);

    double ns_per_word = (median_us * 1000.0) / words.size();
    double mwords_per_sec = (words.size() / (median_us / 1000000.0)) / 1000000.0;
    double mb_per_sec = (words.size() * 4.0 / (median_us / 1000000.0)) / (1024.0 * 1024.0);
    double eq_fps = 1000000.0 / median_us;
    double draws_per_sec = (last_draws / (median_us / 1000000.0));

    if (cfg.csv) {
        std::cout << "words,min_us,max_us,mean_us,median_us,p95_us,stddev_us,ns_per_word,mwords_sec,mb_sec,eq_fps,draws,state_hash,deterministic\n";
        std::cout << words.size() << ","
                  << std::fixed << std::setprecision(2)
                  << min_us << "," << max_us << "," << mean_us << "," << median_us << ","
                  << p95_us << "," << stddev_us << "," << ns_per_word << ","
                  << mwords_per_sec << "," << mb_per_sec << "," << eq_fps << ","
                  << last_draws << ",0x" << std::hex << expected_hash << std::dec << ","
                  << (!hash_mismatch ? "1" : "0") << "\n";
    } else {
        std::cout << " Throughput & Latency Results:\n";
        std::cout << "   Median Frame Time:  " << std::fixed << std::setprecision(2) << median_us / 1000.0 << " ms (" << median_us << " us)\n";
        std::cout << "   Mean Frame Time:    " << mean_us / 1000.0 << " ms (StdDev: ±" << stddev_us << " us)\n";
        std::cout << "   Min / Max Frame:    " << min_us / 1000.0 << " ms / " << max_us / 1000.0 << " ms\n";
        std::cout << "   95th Percentile:    " << p95_us / 1000.0 << " ms\n";
        std::cout << "   Equivalent FPS:     " << std::setprecision(1) << eq_fps << " fps\n";
        std::cout << "   Processing Rate:    " << std::setprecision(2) << ns_per_word << " ns/word ("
                  << mwords_per_sec << " Mwords/s, " << mb_per_sec << " MB/s)\n";
        std::cout << "   Draw Calls / Sec:   " << std::setprecision(0) << draws_per_sec << " draws/s ("
                  << last_draws << " draws, " << last_clears << " clears)\n";
        std::cout << "------------------------------------------------------------\n";
        std::cout << " Deterministic Verification:\n";
        std::cout << "   Dispatcher Calls:   " << expected_calls << "\n";
        std::cout << "   State Hash:         0x" << std::hex << expected_hash << std::dec << "\n";
        std::cout << "   Consistency Status: " << (!hash_mismatch ? "VERIFIED (100% Deterministic)" : "FAILED (Divergent State)") << "\n";
        std::cout << "============================================================\n";
    }

    rsx_replay_destroy(ctx);
    return hash_mismatch ? 1 : 0;
}
