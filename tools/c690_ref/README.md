# c690 tree/cull stage: executable C reference (0xFDA0)

This directory is the first-stage reference for the native c690 Edge path. It
is the code that the corpus/differential test in `docs/edge-native-step1.md`
runs against the game's lifted stage output.

## What is here

| file | role |
|---|---|
| `ref_c690_stage.c` | C reference for LS `0xFDA0` and its continuation blocks (`0xF500..0x105C0`) |
| `test_ref_c690_stage.c` | offline differential harness (loads a captured LS+GPR image, runs the reference, compares LS and all 128 GPRs) |
| `run_corpus_test.sh` | extracts the stage pairs from a `PS3RECOMP_C690_CORPUS` directory, builds and runs the harness |
| `gen_native_stage.py` | flattens the reference into `runtime/spu/c690_native_stage.inc` (the native kernel) |

## How the reference was produced

The stage is not one function in the lifted module: `0xFDA0` is a state machine
split across 45 lifted functions (the `0xFDD8`..`0x105B8` continuation blocks,
plus `0xFA20`, `0xFA58`, `0xFCB8`, `0xFCE4`). `ref_c690_stage.c` is a
transliteration of those functions from
`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c` with only the
runtime plumbing removed:

* interrupt checks, the `SPU_DIRECT` trampoline, `spu_drain_call`, and
  `spu_img_restore` are gone; a direct branch is a normal C call and `SPU_RET`
  is a C `return`;
* indirect calls go through `ref_indirect()`, which maps `ctx->pc` to the
  reference function for that LS address;
* the LS and arithmetic operations are still `spu_ls_read128`,
  `spu_ls_write128`, `spu_fma`, `spu_fi`, `spu_frsqest`, `spu_shufb`,
  `spu_rotqby`, ... from `runtime/spu/spu_helpers.h`.

That keeps the reference bit-exact with the lifted module (the game build uses
`-DSPU_FAST_FLOAT -DSPU_LIFTED_FAST_LS -mavx2 -mfma -ffp-contract=off`) while
making the control flow readable. A native rewrite must preserve those helper
semantics: no FMA contraction, SPU rounding, SPU denormal/FTZ behaviour.

## Stage shape

The capture's entry args are `r3 = 0x1330` (an **LS** pointer to the cull
context; it is not a guest EA), `r6/r7` an item range, and `r4/r5/r8/r9` LS
pointers into the batch. The stage:

1. makes a 96-byte stack frame and spills the incoming registers;
2. loads a node/record with `spu_ls_read128` and extracts its bit fields with
   `cwd/chd/cdd/cwx` and `shufb/rotqby`;
3. evaluates 8 vertices at a time: matrix/`fma` transforms, `frsqest` + `fi`
   reciprocal-square-root refinement, and `shufb` packing;
4. calls `0xFA58` for the node/interval bounds test and `0xFCB8`/`0xFCE4` for
   the leaf; kept triangles/indices are packed into the output region (the
   captured output changes at LS `0x2Bxxx..0x2Dxxx`; the `0x2F000` page is the
   vertex-ring image used by the later assembler);
5. recurses at the two child ranges until the node range is exhausted.

The output of the reference is the full 256 KiB LS plus all 128 GPRs, so the
test is stricter than "the index list matches": any change in an intermediate
buffer or a leftover register fails it.

## Reproducing the corpus stage pairs

The corpus module itself is in `runtime/spu/c690_corpus.c`. The stage
snapshots need a small instrumentation edit in the generated lifted tree
(`recomp-work/spu_lifted/jobmod_c690c2f7b09a_eF5B8/spu_recomp.c`): rename the
generated body to `jobmod_c690c2f7b09a_eF5B8_spu_func_0000FDA0_body` and add

```c
void jobmod_c690c2f7b09a_eF5B8_spu_func_0000FDA0(spu_context* ctx) {
    extern int g_c690_corpus_on;
    extern void c690_corpus_stage_enter(const spu_context*, uint32_t);
    extern void c690_corpus_stage_exit(const spu_context*, uint32_t);
    if (__builtin_expect(!g_c690_corpus_on, 1))
        __attribute__((musttail)) return jobmod_c690c2f7b09a_eF5B8_spu_func_0000FDA0_body(ctx);
    c690_corpus_stage_enter(ctx, 0xFDA0u);
    jobmod_c690c2f7b09a_eF5B8_spu_func_0000FDA0_body(ctx);
    c690_corpus_stage_exit(ctx, 0xFDA0u);
}
```

The recursive calls in the generated body keep the original name, so the
wrapper sees every entry and the corpus module folds recursion to the
outermost call. The edit is not committed because that tree is regenerated
game data.

## Native kernel (step 2)

`python3 tools/c690_ref/gen_native_stage.py tools/c690_ref/ref_c690_stage.c \
    runtime/spu/c690_native_stage.inc`

The generator keeps `sr0..sr93` live in one function and turns every
continuation tail call into a `goto`, removing the per-block `ctx->gpr`
save/reload. `runtime/spu/c690_native_cull.c` includes it with
`SPU_FAST_FLOAT` (the lifted module's float path), `SPU_LIFTED_FAST_LS` and
`SPU_FF_FTZ_HW` (hardware FTZ|DAZ, result zero-sign canonicalisation). It
adds the default-off `PS3RECOMP_C690_NATIVE_CULL` / `..._SHADOW` switches, the
layout guard, and the full-LS/GPR shadow comparator. See
`mlb16-recomp/docs/edge-native-step2.md`.

## Cull hooks and the live bypass

`patch_cull_hooks.py` also installs the `MLB16_CULL_BYPASS` dispatch helper and
its three guards (0x13518/0x13520 for family 0x25/0x26, 0x12DC0 for 0x2C). The
helper calls `c690_cull_bypass_mode()` in `runtime/spu/c690_cull_fix.c`, which
reads the live key `PS3_LIVE_CULL_BYPASS` while `PS3RECOMP_CTL` is configured
and otherwise falls back to the environment. Values: 0 normal, 1 both families,
2 0x2C only, 3 0x25/0x26 only. The guards are inert at 0, and a fresh lift gets
them (and the helper) inserted by the same script. See
`mlb16-recomp/docs/cull-bypass-ab.md`.

## Run

```sh
# captured with PS3RECOMP_C690_CORPUS=<dir> in the meter run
tools/c690_ref/run_corpus_test.sh /home/m/Documents/mlb16-online/recomp-work/corpus_edge1_20261103_012604
```

On the step-1 corpus (4 captured `0xFDA0` pairs) the result is **4/4
bit-exact**: 0 differing LS bytes and 0 differing GPRs per pair. The corpus'
stage pairs are all cull jobs (`0x25`/`0x26`); the other sampled jobs do not
enter `0xFDA0`.

## Attribution

The stage split and the corpus/differential method follow the NHL Legacy
Native port (MIT, Copyright (c) 2026 HeckYouMan). No NHL code was copied into
`ref_c690_stage.c`: c690's data layout and cull math are different, so the
reference is derived from MLB's own lifted disassembly. See
`THIRD_PARTY_NOTICES.md`.
