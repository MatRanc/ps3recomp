/* Placing a FIFO dump at a scene the user navigates to (cellGcmSys.c).
 *
 * PS3RECOMP_DUMP_FIFO starts at the first drain, so a later scene could only
 * be captured by guessing a flip count in PS3RECOMP_DUMP_FIFO_SKIP. With
 * PS3RECOMP_DUMP_FIFO_ARM=1 the dump waits for the live switch
 * PS3RECOMP_DUMP_FIFO_TRIGGER in the PS3RECOMP_CTL file: the first flip after
 * the rising edge opens the file, so the dump begins on a frame boundary and
 * holds exactly PS3RECOMP_DUMP_FIFO_FRAMES complete frames. Without ARM the
 * old boot-time behaviour is unchanged.
 *
 * This fixture links the real ps3_live_ctl.c and drives the control file:
 * no trigger -> no file; a control-file write -> one dump of two frames,
 * big-endian method words and 0xFEAD0000|buffer flip markers, then closed.
 *
 * cc -std=gnu17 -O1 -I include -I libs/video
 *    libs/video/tests/test_gcm_fifo_dump_trigger.c libs/video/ps3_live_ctl.c
 *    -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread -o /tmp/t
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
void rsx_draw_engine_methods(u32 method, u32 step, const u8* src, u32 count) { (void)method; (void)step; (void)src; (void)count; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

/* Replace (not truncate) the control file, so the inode changes and the
 * live-ctl stat cache cannot miss the edge. */
static void write_ctl(const char* path, const char* value)
{
    unlink(path);
    FILE* f = fopen(path, "w");
    if (!f) { perror("ctl"); exit(2); }
    fprintf(f, "PS3RECOMP_DUMP_FIFO_TRIGGER=%s\n", value);
    fclose(f);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char dir[] = "/tmp/gcm-fifo-dump-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    char ctl[256], out[256];
    snprintf(ctl, sizeof ctl, "%s/ctl", dir);
    snprintf(out, sizeof out, "%s/fifo.bin", dir);

    setenv("PS3RECOMP_DUMP_FIFO", out, 1);
    setenv("PS3RECOMP_DUMP_FIFO_ARM", "1", 1);
    setenv("PS3RECOMP_DUMP_FIFO_FRAMES", "2", 1);
    unsetenv("PS3RECOMP_DUMP_FIFO_SKIP");
    setenv("PS3RECOMP_CTL", ctl, 1);
    write_ctl(ctl, "0");

    gcm_fifo_dump_init();
    check(s_fifo_dump_armed == 1 && s_fifo_dump_fp == NULL,
          "ARM=1 waits instead of opening at boot");
    ps3_live_ctl_poll(0);
    gcm_fifo_dump_poll();
    gcm_fifo_dump_word(0x11111111u);
    gcm_fifo_dump_flip(0);
    check(access(out, F_OK) != 0, "no trigger: no dump file");

    write_ctl(ctl, "1");
    ps3_live_ctl_poll(1);
    gcm_fifo_dump_poll();
    check(s_fifo_dump_start_pending == 1 && s_fifo_dump_fp == NULL,
          "trigger arms the next flip boundary");

    /* The flip that follows the trigger closes the pre-capture frame and
     * opens the file; its marker is not part of the dump. */
    gcm_fifo_dump_flip(1);
    check(s_fifo_dump_fp != NULL && s_fifo_dump_frames_left == 2,
          "first flip after the trigger opens the dump");

    gcm_fifo_dump_word(0xABCDEF01u);
    gcm_fifo_dump_flip(2);
    gcm_fifo_dump_word(0x12345678u);
    gcm_fifo_dump_flip(3);
    check(s_fifo_dump_fp == NULL && s_fifo_dump_frames_left == 0,
          "two flips fill FRAMES and close the dump");

    /* A still-high trigger must not restart it. */
    ps3_live_ctl_poll(4);
    gcm_fifo_dump_poll();
    gcm_fifo_dump_flip(4);
    check(s_fifo_dump_fp == NULL, "a held trigger does not re-arm");

    FILE* f = fopen(out, "rb");
    check(f != NULL, "dump file exists");
    if (f) {
        unsigned char b[16];
        const size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        const u32 want[4] = { 0xABCDEF01u, 0xFEAD0002u, 0x12345678u, 0xFEAD0003u };
        int ok = (n == sizeof b);
        for (int i = 0; ok && i < 4; i++) {
            u32 got = ((u32)b[i * 4] << 24) | ((u32)b[i * 4 + 1] << 16) |
                      ((u32)b[i * 4 + 2] << 8) | (u32)b[i * 4 + 3];
            if (got != want[i]) ok = 0;
        }
        check(ok, "dump holds the two full frames, big-endian, ending on a flip marker");
    }

    unlink(ctl);
    unlink(out);
    rmdir(dir);
    printf("[gcm_fifo_dump_trigger] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
