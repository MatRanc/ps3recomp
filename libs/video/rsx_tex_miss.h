/*
 * ps3recomp - dummy-texture census (PS3RECOMP_TEX_MISS_LOG)
 *
 * When a texture unit cannot be resolved, the backends sample a 1x1 dummy
 * (magenta, or black with VK_DUMMY_BLACK). This counts each fallback by
 * reason with the texture's address, format and size, and the shader/draw it
 * was bound to, then prints a per-frame summary and a top-N list.
 *
 * Enabled only by PS3RECOMP_TEX_MISS_LOG=1; default off and zero-cost
 * (a cached flag test on the note path).
 */
#ifndef PS3RECOMP_RSX_TEX_MISS_H
#define PS3RECOMP_RSX_TEX_MISS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RSX_TEX_MISS_FORMAT = 0,    /* format id / mip layout the decoder rejects */
    RSX_TEX_MISS_SURFACE,       /* render target or tiled surface not usable  */
    RSX_TEX_MISS_ADDRESS,       /* guest address could not be resolved/read    */
    RSX_TEX_MISS_SIZE,          /* width/height/pitch outside what we upload   */
    RSX_TEX_MISS_OTHER,         /* allocation, backend create, kind mismatch   */
    RSX_TEX_MISS_REASONS
};

const char* rsx_tex_miss_reason_name(unsigned reason);

void rsx_tex_miss_set_enabled(int on);
int  rsx_tex_miss_enabled(void);

/* Record one unit falling back to the dummy. `format` is the format byte
 * from SET_TEXTURE_FORMAT bits 8-15 (including the LN bit); bits 8-15 of this
 * argument are an optional caller tag (the Vulkan backend puts the object
 * kind there). `fp_hash` and `pipeline` identify the fragment program and the
 * host pipeline the unit was bound to. */
void rsx_tex_miss_note(unsigned reason,
                       uint32_t location, uint32_t offset, uint32_t format,
                       uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t mips, uint32_t cube,
                       uint64_t fp_hash, uint32_t pipeline);

/* Emit the frame's summary (only when it had a miss) and, every 60 emitted
 * frames, the cumulative top-N list. Resets the per-frame counters. Returns
 * the number of misses in the frame. */
unsigned rsx_tex_miss_frame(FILE* out, unsigned frame_no);

/* Emit the cumulative top-N list now (engine shutdown), if anything missed. */
void rsx_tex_miss_flush(FILE* out);

void     rsx_tex_miss_reset(void);
unsigned rsx_tex_miss_run_count(unsigned reason);
unsigned long long rsx_tex_miss_total(void);

#ifdef __cplusplus
}
#endif
#endif
