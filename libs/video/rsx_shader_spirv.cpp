/*
 * ps3recomp - HLSL -> SPIR-V through glslang, for the Vulkan backend
 *
 * See rsx_shader_spirv.h for the binding contract. The front end is driven
 * exactly as rsx_shader_msl.cpp drives it (C++ API for setEntryPoint, Vulkan
 * rules, auto-mapped bindings and locations, then mapIO, legalized SPIR-V),
 * with three differences, all because the output goes to a Vulkan driver
 * rather than to spirv-cross:
 *
 *   - Vulkan 1.0 client, SPIR-V 1.0: the backend creates a 1.0 instance and
 *     promises to run on 1.0-level drivers. (The MSL path targets 1.3 because
 *     that is spirv-cross's floor, not a driver's.)
 *   - Texture and sampler registers are shifted apart (setShiftBinding),
 *     because Vulkan has one binding namespace per set.
 *   - The vertex stage flips Y (setInvertY): D3D clip space is +y up.
 *
 * Translations are cached. glslang takes 20-150 ms per shader, on the thread
 * that walks the FIFO, and the engine asks for the same HLSL again for every
 * pipeline that shares a program (Twisted Metal: 236 translations in its
 * first minute, 74 distinct). The cache key is the stage plus the whole HLSL
 * text, so a hit returns exactly the words that text translated to. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> the cache is also kept on disk,
 * one file per shader, and the next run starts warm. A file whose key,
 * glslang version or translator build differs from the request is a miss,
 * and is rewritten.
 */

#include "rsx_shader_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#  include <direct.h>
#  include <process.h>
#  include <windows.h>
#  define MKDIR(p) _mkdir(p)
#  define strcasecmp _stricmp
#else
#  include <dirent.h>
#  include <strings.h>
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0777)
#endif

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

int translate(const char* hlsl, int stage, std::vector<unsigned int>& spirv,
              char* log, u32 log_size)
{
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex
                                                                : EShLangFragment;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    /* SPIR-V FragCoord.w is 1/w (FXC's SV_POSITION.w is w): tells the FP
     * decompiler's _rsx_inv_w helper which one it gets. */
    shader.setPreamble("#define RSX_FRAGCOORD_W_IS_RCP 1\n");
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
    shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
    if (lang == EShLangVertex) shader.setInvertY(true);

    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return -1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    return 0;
}

/* ---- translation cache ------------------------------------------------- */

const u32 kCacheFormat = 1;
const u32 kCacheMagic = 0x56505352u;   /* "RSPV" */
/* This file's compile time stands for translate()'s options and the binding
 * contract in rsx_shader_spirv.h: when either changes, this file is rebuilt,
 * and the files an older build wrote become misses. */
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, std::vector<unsigned int>> map;
    std::string dir;
    bool dir_read = false;
    RsxSpirvCacheStats stats = {};
};

Cache& cache()
{
    static Cache c;
    return c;
}

const std::string& cache_dir(Cache& c)
{
    if (!c.dir_read) {
        const char* e = getenv("PS3RECOMP_SPIRV_CACHE");
        c.dir = (e && *e) ? e : "";
        c.dir_read = true;
    }
    return c.dir;
}

/* The key is the stage, then the HLSL text. */
std::string cache_key(const char* hlsl, int stage)
{
    std::string key(1, (char)('0' + stage));
    key += hlsl;
    return key;
}

std::string cache_path(const std::string& dir, const std::string& key)
{
    unsigned long long h = 0xcbf29ce484222325ull;       /* FNV-1a 64 */
    for (unsigned char ch : key) { h ^= ch; h *= 0x100000001b3ull; }
    char name[40];
    snprintf(name, sizeof name, "/%016llx.spv", h);
    return dir + name;
}

/* Header: magic, format, glslang major/minor/patch, translator build (six
 * words), key bytes, word count; then the key and the words. */
const int kHeaderWords = 13;

void cache_header(u32 hdr[kHeaderWords], const std::string& key, size_t nwords)
{
    const glslang::Version v = glslang::GetVersion();
    hdr[0] = kCacheMagic; hdr[1] = kCacheFormat;
    hdr[2] = (u32)v.major; hdr[3] = (u32)v.minor; hdr[4] = (u32)v.patch;
    memcpy(&hdr[5], kTranslatorBuild, sizeof kTranslatorBuild);
    hdr[11] = (u32)key.size(); hdr[12] = (u32)nwords;
}

bool cache_load(const std::string& path, const std::string& key,
                std::vector<unsigned int>& spirv)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 want[kHeaderWords], hdr[kHeaderWords];
    cache_header(want, key, 0);
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, (kHeaderWords - 1) * sizeof(u32)) == 0 &&
              hdr[kHeaderWords - 1] != 0 && hdr[kHeaderWords - 1] <= (1u << 24);
    if (ok) {
        std::string stored(key.size(), '\0');
        ok = fread(&stored[0], 1, stored.size(), f) == stored.size() && stored == key;
    }
    if (ok) {
        spirv.resize(hdr[kHeaderWords - 1]);
        ok = fread(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    }
    fclose(f);
    if (!ok) spirv.clear();
    return ok;
}

/* Written under a temporary name and renamed, so a reader never sees half a file. */
void cache_store(const std::string& path, const std::string& key,
                 const std::vector<unsigned int>& spirv)
{
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".%zx.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 hdr[kHeaderWords];
    cache_header(hdr, key, spirv.size());
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(key.data(), 1, key.size(), f) == key.size() &&
              fwrite(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

/* ---- guest-keyed disk cache ---------------------------------------------
 *
 * One file per (guest key, stage). The key comes from rsx_draw_engine.c and
 * already covers the microcode plus every input that changes the HLSL; this
 * layer adds the translator and glslang versions and validates the HLSL the
 * file was made from before using it. */

const u32 kDiskMagic = 0x44435352u;   /* "RSCD" */
const u32 kDiskFormat = 1;
const int kDiskHeaderWords = 15;

u64 fnv1a64(const void* d, size_t n, u64 h)
{
    const unsigned char* p = (const unsigned char*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

u64 hlsl_hash(const char* hlsl)
{
    /* Seeded with this file's build stamp so a translate()/binding change
     * invalidates disk entries even if nobody bumps the version constant. */
    const u64 seed = fnv1a64(kTranslatorBuild, sizeof kTranslatorBuild,
                             0xcbf29ce484222325ull);
    return fnv1a64(hlsl, strlen(hlsl), seed);
}

bool mkdir_p(const std::string& path)
{
    if (path.empty()) return false;
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 &&
        !mkdir_p(path.substr(0, slash)))
        return false;
    if (MKDIR(path.c_str()) == 0) return true;
    return errno == EEXIST && stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool on_word(const char* s)
{
    return s && (strcmp(s, "1") == 0 || !strcasecmp(s, "on") ||
                 !strcasecmp(s, "true") || !strcasecmp(s, "yes"));
}

struct DiskCache {
    std::mutex lock;
    bool env_read = false;
    bool on = false;
    std::string dir;
    u32 version = RSX_SHADER_TRANSLATOR_VERSION;
    RsxShaderDiskCacheStats stats = {};
};

DiskCache& disk_cache()
{
    static DiskCache c;
    return c;
}

void disk_atexit()
{
    DiskCache& c = disk_cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (!c.on) return;
    fprintf(stderr, "[shader-cache] %s: %u hits, %u misses, %u stores,"
                    " %u rejected (translator v%u)\n",
            c.dir.c_str(), (unsigned)c.stats.hits, (unsigned)c.stats.misses,
            (unsigned)c.stats.stores, (unsigned)c.stats.rejected,
            (unsigned)c.version);
}

bool tmp_older_than_hour(const std::string& path, time_t now)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 &&
           (st.st_mode & S_IFMT) == S_IFREG &&
           st.st_mtime < now - 3600;
}

/* A store writes <file>.<pid>.<serial>.tmp then renames it. A process killed
 * mid-write leaves the temporary behind; sweep those at open, but leave a
 * recent one (another live process may be writing it). */
/* Only our own temp names (<16hex>_<stage>.spv.<pid>.<serial>.tmp): the cache dir may be user-supplied. */
bool own_tmp_name(const char* name)
{
    for (int i = 0; i < 16; ++i) if (!isxdigit((unsigned char)name[i])) return false;
    const size_t len = strlen(name);
    return name[16] == '_' && strstr(name, ".spv.") && len > 4 && strcmp(name + len - 4, ".tmp") == 0;
}

void disk_sweep_tmp(const std::string& dir)
{
    const time_t now = time(NULL);
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    const std::string pattern = dir + "/*.tmp";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!own_tmp_name(fd.cFileName)) continue;
        const std::string path = dir + "/" + fd.cFileName;
        if (tmp_older_than_hour(path, now)) remove(path.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        if (!own_tmp_name(name)) continue;
        const std::string path = dir + "/" + name;
        if (tmp_older_than_hour(path, now)) remove(path.c_str());
    }
    closedir(d);
#endif
}

/* Reads the switch once. A value that is not an on-word is the directory. */
void disk_env(DiskCache& c)
{
    if (c.env_read) return;
    c.env_read = true;
    c.on = false;
    c.dir.clear();
    const char* e = getenv("PS3RECOMP_SHADER_DISK_CACHE");
    if (!e || !*e || strcmp(e, "0") == 0) return;
    if (on_word(e)) {
        const char* root = getenv("PS3_GAMEDATA_ROOT");
        c.dir = (root && *root) ? std::string(root) + "/shader_cache"
                                : std::string("recomp-work/shader_cache");
    } else {
        c.dir = e;
    }
    if (!mkdir_p(c.dir)) {
        fprintf(stderr, "[shader-cache] cannot create %s; disk cache off\n",
                c.dir.c_str());
        c.dir.clear();
        return;
    }
    disk_sweep_tmp(c.dir);
    c.on = true;
    static bool registered = false;
    if (!registered) { registered = true; atexit(disk_atexit); }
    fprintf(stderr, "[shader-cache] %s (translator v%u)\n",
            c.dir.c_str(), (unsigned)c.version);
}

std::string disk_path(const DiskCache& c, u64 key, int stage)
{
    char name[64];
    snprintf(name, sizeof name, "/%016llx_%d.spv",
             (unsigned long long)key, stage);
    return c.dir + name;
}

void disk_header(u32 hdr[kDiskHeaderWords], u32 version, int stage, u64 key,
                 u64 hash, size_t hlen, size_t nwords, u64 sum)
{
    const glslang::Version v = glslang::GetVersion();
    hdr[0] = kDiskMagic;
    hdr[1] = kDiskFormat;
    hdr[2] = version;
    hdr[3] = (u32)v.major;
    hdr[4] = (u32)v.minor;
    hdr[5] = (u32)v.patch;
    hdr[6] = (u32)stage;
    hdr[7] = (u32)key;
    hdr[8] = (u32)(key >> 32);
    hdr[9] = (u32)hash;
    hdr[10] = (u32)(hash >> 32);
    hdr[11] = (u32)hlen;
    hdr[12] = (u32)nwords;
    hdr[13] = (u32)sum;
    hdr[14] = (u32)(sum >> 32);
}

bool disk_load(DiskCache& c, u64 key, int stage, const char* hlsl,
               u32* words, u32 max_words, u32* out_words)
{
    const u64 hash = hlsl_hash(hlsl);
    const size_t hlen = strlen(hlsl);
    std::string path;
    u32 want[kDiskHeaderWords];
    {
        std::lock_guard<std::mutex> guard(c.lock);
        disk_env(c);
        if (!c.on) return false;
        path = disk_path(c, key, stage);
        disk_header(want, c.version, stage, key, hash, hlen, 0, 0);
    }

    /* File I/O runs outside the cache lock: a load must not serialise another
     * thread's store or load on a slow disk. */
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        std::lock_guard<std::mutex> guard(c.lock);
        c.stats.misses++;
        return false;
    }

    u32 hdr[kDiskHeaderWords];
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, 12 * sizeof(u32)) == 0 &&
              hdr[12] != 0 && hdr[12] <= max_words &&
              hdr[12] <= (1u << 24);
    if (ok) {
        ok = fread(words, sizeof(u32), hdr[12], f) == hdr[12];
        if (ok) {
            const u64 sum = fnv1a64(words, (size_t)hdr[12] * sizeof(u32),
                                    0xcbf29ce484222325ull);
            ok = (u32)sum == hdr[13] && (u32)(sum >> 32) == hdr[14];
        }
        /* A file with bytes past the declared words is not this header's. */
        if (ok) {
            const int extra = fgetc(f);
            ok = extra == EOF && !ferror(f);
        }
        if (ok) *out_words = hdr[12];
    }
    fclose(f);

    std::lock_guard<std::mutex> guard(c.lock);
    if (!ok) {
        c.stats.rejected++;
        c.stats.misses++;
        return false;
    }
    c.stats.hits++;
    return true;
}

/* Written under a temporary name and renamed, so a reader never sees half a
 * file. A failed write is silent: the translation is already in hand. */
void disk_store(DiskCache& c, u64 key, int stage, const char* hlsl,
                const u32* words, u32 nwords)
{
    std::lock_guard<std::mutex> guard(c.lock);
    disk_env(c);
    if (!c.on) return;

    static std::atomic<unsigned> serial{0};
    const std::string path = disk_path(c, key, stage);
    char suffix[64];
#ifdef _WIN32
    snprintf(suffix, sizeof suffix, ".%d.%u.tmp",
             (int)_getpid(), (unsigned)serial.fetch_add(1));
#else
    snprintf(suffix, sizeof suffix, ".%d.%u.tmp",
             (int)getpid(), (unsigned)serial.fetch_add(1));
#endif
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;

    const u64 hash = hlsl_hash(hlsl);
    const size_t hlen = strlen(hlsl);
    const u64 sum = fnv1a64(words, (size_t)nwords * sizeof(u32),
                            0xcbf29ce484222325ull);
    u32 hdr[kDiskHeaderWords];
    disk_header(hdr, c.version, stage, key, hash, hlen, nwords, sum);
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(words, sizeof(u32), nwords, f) == nwords;
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return;
    }
    c.stats.stores++;
    if ((c.stats.stores & 63u) == 0)
        fprintf(stderr, "[shader-cache] %u stores, %u hits, %u misses\n",
                (unsigned)c.stats.stores, (unsigned)c.stats.hits,
                (unsigned)c.stats.misses);
}

/* Read the guest-keyed files once to bring them into the page cache. The
 * pipeline worker does this at boot, before it has any work: the bytes are
 * not kept, so memory stays bounded and a store/store race is impossible. */
u32 disk_prefetch(unsigned long long* bytes)
{
    DiskCache& c = disk_cache();
    std::string dir;
    {
        std::lock_guard<std::mutex> guard(c.lock);
        disk_env(c);
        if (!c.on) { if (bytes) *bytes = 0; return 0; }
        dir = c.dir;
    }
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    const std::string pattern = dir + "/*.spv";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { if (bytes) *bytes = 0; return 0; }
    u32 files = 0;
    unsigned long long total = 0;
    const unsigned long long cap = 64ull << 20;
    do {
        const char* name = fd.cFileName;
        const size_t len = strlen(name);
        if (len != 22 || name[16] != '_' || (name[17] != '0' && name[17] != '1') ||
            strcmp(name + 18, ".spv") != 0) continue;
        for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)name[i])) goto next;
        {
            const std::string path = dir + "/" + name;
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) goto next;
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) != 0) {
                total += n;
                if (total >= cap) { fclose(f); goto done; }
            }
            fclose(f);
            files++;
        }
    next:;
    } while (FindNextFileA(h, &fd));
done:
    FindClose(h);
    if (bytes) *bytes = total;
    return files;
#else
    DIR* d = opendir(dir.c_str());
    if (!d) { if (bytes) *bytes = 0; return 0; }
    u32 files = 0;
    unsigned long long total = 0;
    const unsigned long long cap = 64ull << 20;
    struct dirent* e;
    while ((e = readdir(d))) {
        const char* name = e->d_name;
        const size_t len = strlen(name);
        if (len != 22 || name[16] != '_' || (name[17] != '0' && name[17] != '1') ||
            strcmp(name + 18, ".spv") != 0) continue;
        bool hex = true;
        for (int i = 0; i < 16; i++)
            if (!isxdigit((unsigned char)name[i])) { hex = false; break; }
        if (!hex) continue;
        const std::string path = dir + "/" + name;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        char buf[65536];
        size_t n;
        bool stop = false;
        while ((n = fread(buf, 1, sizeof buf, f)) != 0) {
            total += n;
            if (total >= cap) { stop = true; break; }
        }
        fclose(f);
        if (stop) break;
        files++;
    }
    closedir(d);
    if (bytes) *bytes = total;
    return files;
#endif
}

} // namespace

extern "C" u32 rsx_hlsl_to_spirv_disk_prefetch(unsigned long long* bytes)
{
    return disk_prefetch(bytes);
}

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }

    /* One lock around the whole lookup and translation: glslang's process
     * state is not documented as thread-safe either. */
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    const std::string key = cache_key(hlsl, stage);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
        c.stats.memory_hits++;
    } else {
        std::vector<unsigned int> spirv;
        const std::string& dir = cache_dir(c);
        const std::string path = dir.empty() ? std::string() : cache_path(dir, key);
        if (!dir.empty() && cache_load(path, key, spirv)) {
            c.stats.disk_hits++;
        } else {
            if (translate(hlsl, stage, spirv, log, log_size)) return -1;
            c.stats.translations++;
            if (!dir.empty()) cache_store(path, key, spirv);
        }
        it = c.map.emplace(key, std::move(spirv)).first;
    }
    const std::vector<unsigned int>& spirv = it->second;
    if (spirv.size() > max_words) {
        char n[64];
        snprintf(n, sizeof n, "%zu words, room for %u", spirv.size(), max_words);
        put_log(log, log_size, "SPIR-V output too large", n);
        return -1;
    }
    memcpy(words, spirv.data(), spirv.size() * sizeof(u32));
    *out_words = (u32)spirv.size();
    return 0;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (out) *out = c.stats;
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    c.map.clear();
    c.dir_read = false;
    c.stats = {};
}

extern "C" int rsx_hlsl_to_spirv_keyed(u64 guest_key, const char* hlsl,
                                       int stage, u32* words, u32 max_words,
                                       u32* out_words, char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0)
        return rsx_hlsl_to_spirv(hlsl, stage, words, max_words, out_words,
                                 log, log_size);
    DiskCache& d = disk_cache();
    {
        std::lock_guard<std::mutex> guard(d.lock);
        disk_env(d);
        if (!d.on)
            return rsx_hlsl_to_spirv(hlsl, stage, words, max_words, out_words,
                                     log, log_size);
    }
    u32 n = 0;
    if (disk_load(d, guest_key, stage, hlsl, words, max_words, &n)) {
        *out_words = n;
        return 0;
    }
    const int r = rsx_hlsl_to_spirv(hlsl, stage, words, max_words, out_words,
                                    log, log_size);
    if (r == 0 && *out_words)
        disk_store(d, guest_key, stage, hlsl, words, *out_words);
    return r;
}

extern "C" void rsx_hlsl_to_spirv_disk_stats(RsxShaderDiskCacheStats* out)
{
    DiskCache& d = disk_cache();
    std::lock_guard<std::mutex> guard(d.lock);
    if (out) *out = d.stats;
}

extern "C" void rsx_hlsl_to_spirv_disk_reset(void)
{
    DiskCache& d = disk_cache();
    std::lock_guard<std::mutex> guard(d.lock);
    d.env_read = false;
    d.version = RSX_SHADER_TRANSLATOR_VERSION;
    d.stats = {};
}

extern "C" u32 rsx_hlsl_to_spirv_disk_version(void)
{
    DiskCache& d = disk_cache();
    std::lock_guard<std::mutex> guard(d.lock);
    return d.version;
}

extern "C" void rsx_hlsl_to_spirv_disk_set_version(u32 version)
{
    DiskCache& d = disk_cache();
    std::lock_guard<std::mutex> guard(d.lock);
    d.version = version;
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)words; (void)max_words;
    if (out_words) *out_words = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL -> SPIR-V translation not built (glslang not found)");
    return -1;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void) {}

extern "C" int rsx_hlsl_to_spirv_keyed(u64 guest_key, const char* hlsl,
                                       int stage, u32* words, u32 max_words,
                                       u32* out_words, char* log, u32 log_size)
{
    (void)guest_key;
    return rsx_hlsl_to_spirv(hlsl, stage, words, max_words, out_words,
                             log, log_size);
}

extern "C" void rsx_hlsl_to_spirv_disk_stats(RsxShaderDiskCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_spirv_disk_reset(void) {}
extern "C" u32 rsx_hlsl_to_spirv_disk_version(void) { return RSX_SHADER_TRANSLATOR_VERSION; }
extern "C" void rsx_hlsl_to_spirv_disk_set_version(u32 version) { (void)version; }
extern "C" u32 rsx_hlsl_to_spirv_disk_prefetch(unsigned long long* bytes)
{
    if (bytes) *bytes = 0;
    return 0;
}

#endif
