/*
 * c690 normal/tangent regeneration kernels, written from the algorithm.
 *
 * Included once per float variant by c690_native_soa.c with K(), FS, FM, FMA,
 * FMS, FNMS, FRSQEST, FREST and FI defined. Every float operation is the one
 * the SPU program performs, in the same order and with the variant's own
 * helper, so each lane is bit-identical to the lifted module; what is gone is
 * the scalar-in-a-quadword emulation around them (x, y and z ride in lanes
 * 0..2 of one register instead of three trips through rotqby/cwd/shufb).
 *
 * SPU functions (job module c690c2f7b09a):
 *   0xFA20 clear      (count, dst)
 *   0xFA58 normals    (pos, out, ntri, ntail, idx, scale)
 *   0xFCB8 normalize  (count, buf)
 *   0xFDA0 tangents   (pos, uv, out, ntri, ntail, idx, scale)
 * Triangles [0, ntri-ntail) add into all three vertices, the tail only into
 * the first. All arrays are 16-byte elements; idx is 8 bytes per triangle
 * holding three big-endian u16.
 */

/* sqrt(v): estimate, interpolate, one Newton step (0x10010 / 0xFD38). */
static inline __attribute__((always_inline)) u128 K(sq)(u128 v)
{
    const u128 f = FI(v, FRSQEST(v));
    const u128 t = FM(f, v);
    return FMA(FNMS(f, t, soa_bc(0x3F800001u)), FM(t, soa_bc(0x3F000000u)), t);
}

/* num / d: estimate, interpolate, one Newton step (0x1007C / 0x100A4). */
static inline __attribute__((always_inline)) u128 K(div)(u128 num, u128 d)
{
    const u128 f = FI(d, FREST(d));
    const u128 g = FM(num, f);
    return FMA(FNMS(g, d, num), f, g);
}

static inline __attribute__((always_inline)) u128 K(cross)(u128 e1, u128 e2)
{
    return FMS(soa_yzx(e1), soa_zxy(e2), FM(soa_zxy(e1), soa_yzx(e2)));
}

static void K(clear)(uint8_t* ls, uint32_t count, uint32_t dst)
{
    const u128 z = soa_bc(0);
    uint32_t i = 0;
    if (!(count & 0xFFFFu)) return;
    do soa_st(ls, dst + ((i & 0xFFFFu) << 4), z);
    while ((++i & 0xFFFFu) != (count & 0xFFFFu));
}

static void K(normalize)(uint8_t* ls, uint32_t count, uint32_t buf)
{
    uint32_t i = 0;
    if (!(count & 0xFFFFu)) return;
    do {
        const uint32_t a = buf + ((i & 0xFFFFu) << 4);
        const u128 q = soa_ld(ls, a);
        const u128 x = soa_sh(q, 0), y = soa_sh(q, 1), z = soa_sh(q, 2);
        const u128 len = K(sq)(FMA(z, z, FMA(x, x, FM(y, y))));
        soa_st(ls, a, soa_keep_w(FM(q, K(div)(soa_bc(0x3F800000u), len)), q));
    } while ((++i & 0xFFFFu) != (count & 0xFFFFu));
}

static void K(normals)(uint8_t* ls, uint32_t pos, uint32_t out, uint32_t ntri,
                       uint32_t ntail, uint32_t idx, u128 scale)
{
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t t0 = pass ? ntri - ntail : 0, t1 = pass ? ntri : ntri - ntail;
        const int nv = pass ? 1 : 3;
        for (uint32_t t = t0; t < t1; t++) {
            uint32_t ix[3];
            soa_idx(ls, idx + 8u * t, ix);
            const u128 v0 = soa_ld(ls, pos + (ix[0] << 4));
            const u128 n = K(cross)(FS(soa_ld(ls, pos + (ix[1] << 4)), v0),
                                    FS(soa_ld(ls, pos + (ix[2] << 4)), v0));
            for (int j = 0; j < nv; j++) {
                const uint32_t a = out + (ix[j] << 4);
                const u128 q = soa_ld(ls, a);
                soa_st(ls, a, soa_keep_w(FMA(scale, n, q), q));
            }
        }
    }
}

static void K(tangents)(uint8_t* ls, uint32_t pos, uint32_t uv, uint32_t out,
                        uint32_t ntri, uint32_t ntail, uint32_t idx, u128 scale)
{
    const u128 one = soa_bc(0x3F800000u);
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t t0 = pass ? ntri - ntail : 0, t1 = pass ? ntri : ntri - ntail;
        const int nv = pass ? 1 : 3;
        for (uint32_t t = t0; t < t1; t++) {
            uint32_t ix[3];
            soa_idx(ls, idx + 8u * t, ix);
            const u128 v0 = soa_ld(ls, pos + (ix[0] << 4));
            const u128 w0 = soa_ld(ls, uv + (ix[0] << 4));
            const u128 e1 = FS(soa_ld(ls, pos + (ix[1] << 4)), v0);
            const u128 e2 = FS(soa_ld(ls, pos + (ix[2] << 4)), v0);
            const u128 d1 = FS(soa_ld(ls, uv + (ix[1] << 4)), w0);
            const u128 d2 = FS(soa_ld(ls, uv + (ix[2] << 4)), w0);
            const u128 du1 = soa_sh(d1, 0), dv1 = soa_sh(d1, 1);
            const u128 du2 = soa_sh(d2, 0), dv2 = soa_sh(d2, 1);

            /* Face normal length = twice the area: the weight. */
            const u128 n = K(cross)(e1, e2);
            const u128 nx = soa_sh(n, 0), ny = soa_sh(n, 1), nz = soa_sh(n, 2);
            const u128 area = K(sq)(FMA(nz, nz, FMA(nx, nx, FM(ny, ny))));

            /* Tangent = (dv2*e1 - dv1*e2) / det, taken through a normalised
             * (x, y, det) triple the way the SPU code does. */
            const u128 det = FMS(du1, dv2, FM(dv1, du2));
            const u128 x = FMS(dv1, e2, FM(dv2, e1));
            const u128 y = FMS(du2, e1, FM(du1, e2));
            const u128 len = K(sq)(FMA(y, y, FMA(x, x, FM(det, det))));
            const u128 inv = K(div)(one, len);
            const u128 a = FM(det, inv);
            const u128 d = soa_sel(a, one, spu_fceq(a, soa_bc(0)));
            const u128 tan = K(div)(soa_xor(FM(x, inv), soa_bc(0x80000000u)), d);
            const u128 add = FM(scale, tan);

            for (int j = 0; j < nv; j++) {
                const uint32_t o = out + (ix[j] << 4);
                u128 r = FMA(area, add, soa_ld(ls, o));
                r._u32[3] = 0x3F800000u;
                soa_st(ls, o, r);
            }
        }
    }
}
