/*
 * Opt-in thread pinning for benchmarks (PS3RECOMP_PIN=1).
 *
 * The role is taken from the host thread's name, which the runtime already
 * gives every PPU/SPU/present thread ("ppu main", "ppu DisplayFlip",
 * "spurs pm", "present", "rsx walker"). The topology is read from sysfs so
 * the mapping is not hard-coded for one machine:
 *
 *   walker      one hardware thread, sibling left idle
 *   ppu main    one hardware thread on another core of the same L3/CCX
 *   everything  the remaining cores (sibling pairs of the two above excluded)
 *
 * Nothing is pinned unless PS3RECOMP_PIN is set. The mapping is printed once
 * at startup when it is.
 */
#ifndef PS3_THREAD_AFFINITY_H
#define PS3_THREAD_AFFINITY_H

#ifdef __cplusplus
extern "C" {
#endif

void ps3_thread_pin_by_name(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* PS3_THREAD_AFFINITY_H */
