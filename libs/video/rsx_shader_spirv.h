/*
 * ps3recomp - HLSL -> SPIR-V for the Vulkan backend
 *
 * The RSX decompilers emit HLSL. The Metal backend lowers it HLSL -> SPIR-V ->
 * MSL (rsx_shader_msl.cpp); Vulkan consumes SPIR-V directly, so this stops
 * one step earlier and needs glslang only, not spirv-cross. Without glslang
 * the module compiles to a stub that reports itself unavailable, and the
 * Vulkan backend stays on its fixed-function path.
 *
 * Binding contract, which the Vulkan backend relies on (all in set 0). Vulkan
 * has one binding namespace per set where MSL has three, so the register
 * classes are moved apart:
 *   register(bN)  ->  binding N                          (VPConst 0, PSConstants 1)
 *   register(tN)  ->  binding RSX_SPIRV_TEXTURE_BINDING + N
 *   register(sN)  ->  binding RSX_SPIRV_SAMPLER_BINDING + N
 * The fragment decompiler's `Texture2D rsx_tex[16] : register(t0)` is one
 * binding holding an array of 16, and `SamplerState rsx_samp[16]` likewise.
 * Vertex-texture units (t16..t19) land on TEXTURE_BINDING + 16..19.
 *
 * Locations are numbered in declaration order (TProgram::mapIO). The vertex
 * decompiler always declares ATTR0..ATTR15 in order, so vertex input
 * location n is RSX attribute n; both decompilers declare the varyings in the
 * same order, so the stages link.
 *
 * The vertex stage's position is flipped in Y: the decompiled HLSL writes
 * D3D clip space (+y up), and Vulkan's framebuffer has +y down. Depth needs
 * nothing -- the viewport epilogue already maps GL clip z to [0, 1].
 */

#ifndef PS3RECOMP_RSX_SHADER_SPIRV_H
#define PS3RECOMP_RSX_SHADER_SPIRV_H

#include "ps3emu/ps3types.h"
#include "rsx_shader_msl.h"   /* RSX_SHADER_STAGE_VERTEX / _FRAGMENT */

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SPIRV_VPCONST_BINDING  0u
#define RSX_SPIRV_PSCONST_BINDING  1u
#define RSX_SPIRV_TEXTURE_BINDING  16u
#define RSX_SPIRV_SAMPLER_BINDING  48u

/* 1 when built with glslang, 0 when the stub is in. */
int rsx_hlsl_to_spirv_available(void);

/* Translate one HLSL shader whose entry point is `main` into SPIR-V 1.0 for a
 * Vulkan 1.0 client.
 *   hlsl      : NUL-terminated HLSL, as the decompilers emit it.
 *   stage     : RSX_SHADER_STAGE_VERTEX or RSX_SHADER_STAGE_FRAGMENT.
 *   words     : receives the SPIR-V, max_words 32-bit words at most.
 *   out_words : receives the word count.
 *   log       : receives a diagnostic on failure (may be NULL).
 * Returns 0 on success, -1 on failure (bad input, a front-end error, an
 * output buffer too small, or the stub). */
int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                      u32* words, u32 max_words, u32* out_words,
                      char* log, u32 log_size);

/* The translation cache (rsx_shader_spirv.cpp). A request is answered from
 * memory, else from PS3RECOMP_SPIRV_CACHE's directory, else by glslang. */
typedef struct RsxSpirvCacheStats {
    u32 memory_hits;
    u32 disk_hits;
    u32 translations;   /* glslang runs that succeeded */
} RsxSpirvCacheStats;

void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out);

/* Empties the memory cache, zeroes the counters and reads
 * PS3RECOMP_SPIRV_CACHE again on the next request. Disk files stay. */
void rsx_hlsl_to_spirv_cache_reset(void);

/* ---- guest-keyed disk cache ---------------------------------------------
 *
 * PS3RECOMP_SHADER_DISK_CACHE (default off) keeps the translated SPIR-V under
 * a key the engine already computes from the guest program: the vertex and
 * fragment microcode, every input that changes the HLSL and its translation,
 * and the render state that selects the pipeline variant (the engine's
 * pipeline identity in rsx_draw_engine.c). A warm run can then rebuild a
 * pipeline without running glslang at all.
 *
 * `1`/`on` uses $PS3_GAMEDATA_ROOT/shader_cache (recomp-work/shader_cache in
 * the play scripts); any other value is a directory. Files are written under
 * a temporary name and renamed. A file whose version, stage, key, HLSL hash,
 * length or word checksum does not match the request is a miss and is
 * rewritten, never an error.
 *
 * Bump RSX_SHADER_TRANSLATOR_VERSION whenever the HLSL that the decompilers
 * emit, the binding contract in this header, the glslang options in
 * translate(), or the SPIR-V legalisation changes. The glslang version is
 * also part of every file, so a glslang update is a miss too.
 */
#define RSX_SHADER_TRANSLATOR_VERSION 1u

int rsx_hlsl_to_spirv_keyed(u64 guest_key, const char* hlsl, int stage,
                            u32* words, u32 max_words, u32* out_words,
                            char* log, u32 log_size);

typedef struct RsxShaderDiskCacheStats {
    u32 hits;       /* served from a file */
    u32 misses;     /* no usable file: translated */
    u32 stores;     /* files written */
    u32 rejected;   /* file present but not usable (version/key/checksum/...) */
} RsxShaderDiskCacheStats;

void rsx_hlsl_to_spirv_disk_stats(RsxShaderDiskCacheStats* out);

/* Zero the counters and read PS3RECOMP_SHADER_DISK_CACHE again on the next
 * request; the translator version returns to its compiled value. */
void rsx_hlsl_to_spirv_disk_reset(void);

u32  rsx_hlsl_to_spirv_disk_version(void);
void rsx_hlsl_to_spirv_disk_set_version(u32 version);   /* tests */

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SHADER_SPIRV_H */
