/* PS3_AUDIO_VOLUME / PS3_AUDIO_MUTE / PS3_MUTE -> the host output gain.
 * macOS: clang -std=gnu17 -D_GNU_SOURCE -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_volume.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_volume && /tmp/test_audio_volume
 */
#include "../cellAudio.c"
#include <assert.h>

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

int main(void)
{
    /* Nothing set, or 100: exactly 1.0, the value that queues the mix untouched. */
    assert(audio_gain_from(NULL, NULL, NULL) == 1.0f);
    assert(audio_gain_from("", NULL, NULL) == 1.0f);
    assert(audio_gain_from("100", NULL, NULL) == 1.0f);
    assert(audio_gain_from("250", NULL, NULL) == 1.0f);
    /* The scale. */
    assert(audio_gain_from("50", NULL, NULL) == 0.5f);
    assert(audio_gain_from("25", NULL, NULL) == 0.25f);
    assert(audio_gain_from("1", NULL, NULL) == 1.0f / 100.0f);
    assert(audio_gain_from("0", NULL, NULL) == 0.0f);
    assert(audio_gain_from("-5", NULL, NULL) == 0.0f);
    assert(audio_gain_from("loud", NULL, NULL) == 0.0f);   /* atoi: not a number is 0 */
    /* Mute wins over the volume; either name; 0 and the empty string do not mute. */
    assert(audio_gain_from(NULL, "1", NULL) == 0.0f);
    assert(audio_gain_from("80", "1", NULL) == 0.0f);
    assert(audio_gain_from("80", NULL, "1") == 0.0f);
    assert(audio_gain_from(NULL, "0", "0") == 1.0f);
    assert(audio_gain_from("80", "", NULL) == 0.8f);
    puts("audio volume: gain from PS3_AUDIO_VOLUME / PS3_AUDIO_MUTE / PS3_MUTE passed");
    return 0;
}
