/*
 * ps3recomp - HLSL -> SPIR-V translation cache test
 *
 * rsx_hlsl_to_spirv keeps every translation it makes, in memory and, with
 * PS3RECOMP_SPIRV_CACHE=<dir>, on disk. This checks that:
 *   - the same stage and text is translated once, and the hit returns the
 *     same words;
 *   - the same text for another stage is its own entry;
 *   - a new process (simulated by rsx_hlsl_to_spirv_cache_reset) is served
 *     from the disk file, with the same words;
 *   - a damaged file is a miss, and is rewritten;
 *   - a failed translation is not cached;
 *   - a hit larger than the caller's buffer still fails as before.
 *
 * Built as a CMake target (test_shader_spirv) because it links glslang.
 * POSIX only (mkdtemp). Needs no GPU.
 */
#include "../rsx_shader_spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

static void check(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static const char* kVS =
    "struct VOut { float4 pos : SV_Position; float4 col : COLOR0; };\n"
    "VOut main(float4 p : ATTR0, float4 c : ATTR3) {\n"
    "    VOut o; o.pos = p; o.col = c; return o;\n"
    "}\n";

static const char* kPS =
    "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target0 {\n"
    "    return col.zyxw;\n"
    "}\n";

/* Valid as either stage: no inputs, one output. */
static const char* kBoth =
    "float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }\n";

#define MAX_WORDS 4096u
static u32 g_a[MAX_WORDS], g_b[MAX_WORDS];

static int translate(const char* hlsl, int stage, u32* words, u32* n)
{
    char log[512] = "";
    int r = rsx_hlsl_to_spirv(hlsl, stage, words, MAX_WORDS, n, log, sizeof log);
    if (r) printf("       (%s)\n", log);
    return r;
}

static int translate_keyed(u64 key, const char* hlsl, int stage,
                           u32* words, u32* n)
{
    char log[512] = "";
    int r = rsx_hlsl_to_spirv_keyed(key, hlsl, stage, words, MAX_WORDS, n,
                                    log, sizeof log);
    if (r) printf("       (%s)\n", log);
    return r;
}

static RsxShaderDiskCacheStats disk_stats(void)
{
    RsxShaderDiskCacheStats s;
    rsx_hlsl_to_spirv_disk_stats(&s);
    return s;
}

static RsxSpirvCacheStats stats(void)
{
    RsxSpirvCacheStats s;
    rsx_hlsl_to_spirv_cache_stats(&s);
    return s;
}

/* The one .spv file in dir, or "" when there is not exactly one. */
static void only_file(const char* dir, char* out, size_t size)
{
    DIR* d = opendir(dir);
    int n = 0;
    out[0] = 0;
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".spv") == 0) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n != 1) out[0] = 0;
}

int main(void)
{
    u32 na = 0, nb = 0;
    RsxSpirvCacheStats s;
    RsxShaderDiskCacheStats ds;

    if (!rsx_hlsl_to_spirv_available()) {
        printf("[FAIL] built without glslang\n");
        return 1;
    }
    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();

    /* Memory. */
    check("vertex program translates", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0 && na > 5);
    check("SPIR-V magic", g_a[0] == 0x07230203u);
    check("same request again succeeds", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    s = stats();
    check("... served from memory, glslang ran once", s.translations == 1 && s.memory_hits == 1);

    check("fragment program translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    check("same text as a vertex program", translate(kBoth, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0);
    check("same text as a fragment program", translate(kBoth, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... two entries, not a hit", s.translations == 4 && s.memory_hits == 1);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    {
        u32 small[4], n = 123;
        int r = rsx_hlsl_to_spirv(kPS, RSX_SHADER_STAGE_FRAGMENT, small, 4, &n, NULL, 0);
        check("a hit larger than the buffer fails", r == -1 && n == 0);
    }

    check("broken HLSL fails", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    check("... and again (not cached)", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    s = stats();
    check("... counted as no translation", s.translations == 4);

    /* Disk. */
    char dir[] = "/tmp/ps3recomp_spirv_cache_XXXXXX";
    if (!mkdtemp(dir)) { printf("[FAIL] mkdtemp\n"); return 1; }
    setenv("PS3RECOMP_SPIRV_CACHE", dir, 1);
    rsx_hlsl_to_spirv_cache_reset();

    check("disk: first run translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &na) == 0);
    s = stats();
    check("... with glslang", s.translations == 1 && s.disk_hits == 0);
    char path[1024];
    only_file(dir, path, sizeof path);
    check("... and writes one file", path[0] != 0);

    rsx_hlsl_to_spirv_cache_reset();
    check("disk: next run succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... from the file, no glslang", s.disk_hits == 1 && s.translations == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... then from memory", s.memory_hits == 1 && s.disk_hits == 1);

    /* Damage the stored key: the file no longer matches the request. */
    if (path[0]) {
        FILE* f = fopen(path, "r+b");
        if (f) { fseek(f, 13 * 4 + 10, SEEK_SET); fputc('#', f); fclose(f); }
    }
    rsx_hlsl_to_spirv_cache_reset();
    check("disk: damaged file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... as a miss, with glslang", s.disk_hits == 0 && s.translations == 1);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    rsx_hlsl_to_spirv_cache_reset();
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... and the file was rewritten", s.disk_hits == 1 && s.translations == 0);

    /* A truncated file. */
    if (path[0] && truncate(path, 20) == 0) {
        rsx_hlsl_to_spirv_cache_reset();
        check("disk: truncated file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
        s = stats();
        check("... as a miss", s.disk_hits == 0 && s.translations == 1);
    } else {
        check("truncate", 0);
    }

    only_file(dir, path, sizeof path);
    if (path[0]) remove(path);
    rmdir(dir);

    /* Guest-keyed disk cache (PS3RECOMP_SHADER_DISK_CACHE). */
    char kdir[] = "/tmp/ps3recomp_shader_cache_XXXXXX";
    if (!mkdtemp(kdir)) { printf("[FAIL] mkdtemp (keyed)\n"); return 1; }
    setenv("PS3RECOMP_SHADER_DISK_CACHE", kdir, 1);
    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();
    rsx_hlsl_to_spirv_disk_reset();

    check("keyed: translator version", rsx_hlsl_to_spirv_disk_version() ==
          RSX_SHADER_TRANSLATOR_VERSION);
    check("keyed: first use translates",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0 && na > 5);
    ds = disk_stats();
    check("keyed: ... a miss that stores a file", ds.hits == 0 && ds.stores == 1);
    char kpath[1024];
    snprintf(kpath, sizeof kpath, "%s/%016llx_%d.spv", kdir,
             (unsigned long long)0x1234u, RSX_SHADER_STAGE_VERTEX);
    check("keyed: ... at the expected path", access(kpath, F_OK) == 0);

    rsx_hlsl_to_spirv_disk_reset();
    check("keyed: next process succeeds",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... from the file, no translation", ds.hits == 1 && ds.misses == 0);
    check("keyed: ... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);

    rsx_hlsl_to_spirv_disk_reset();
    check("keyed: another guest key is a miss",
          translate_keyed(0x1235u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... and stores its own file", ds.hits == 0 && ds.misses == 1 && ds.stores == 1);

    /* Corrupt the stored words: header still matches, checksum does not. */
    {
        FILE* f = fopen(kpath, "r+b");
        if (f) { fseek(f, 15 * 4 + 8, SEEK_SET); fputc('#', f); fclose(f); }
        else check("keyed: open for corruption", 0);
    }
    rsx_hlsl_to_spirv_disk_reset();
    check("keyed: corrupt file still succeeds",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... as a rejected miss, then stored", ds.hits == 0 &&
          ds.misses == 1 && ds.rejected == 1 && ds.stores == 1);
    check("keyed: ... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);

    /* A short file is the same kind of miss. */
    if (truncate(kpath, 20) == 0) {
        rsx_hlsl_to_spirv_disk_reset();
        check("keyed: short file still succeeds",
              translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
        ds = disk_stats();
        check("keyed: ... as a rejected miss", ds.hits == 0 && ds.rejected == 1);
    } else {
        check("keyed: truncate", 0);
    }

    /* A translator version bump must invalidate every file. */
    rsx_hlsl_to_spirv_disk_reset();
    const u32 old_version = rsx_hlsl_to_spirv_disk_version();
    rsx_hlsl_to_spirv_disk_set_version(old_version + 1u);
    check("keyed: version bump is a miss",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... rejected and rewritten", ds.hits == 0 && ds.rejected == 1 &&
          ds.stores == 1);
    rsx_hlsl_to_spirv_disk_reset();
    rsx_hlsl_to_spirv_disk_set_version(old_version + 1u);
    check("keyed: bumped version hits its own file",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... from the file", ds.hits == 1 && ds.misses == 0);
    rsx_hlsl_to_spirv_disk_reset();
    check("keyed: old version misses the rewritten file",
          translate_keyed(0x1234u, kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    ds = disk_stats();
    check("keyed: ... as a rejected miss", ds.hits == 0 && ds.rejected == 1);

    {
        DIR* d = opendir(kdir);
        struct dirent* e;
        if (d) {
            while ((e = readdir(d))) {
                if (strstr(e->d_name, ".spv")) {
                    char p[1024];
                    snprintf(p, sizeof p, "%s/%s", kdir, e->d_name);
                    remove(p);
                }
            }
            closedir(d);
        }
    }
    rmdir(kdir);
    unsetenv("PS3RECOMP_SHADER_DISK_CACHE");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
