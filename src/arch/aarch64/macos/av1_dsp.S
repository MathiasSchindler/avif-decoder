.globl _av1_dsp_add_residual_neon
.text
.p2align 2
_av1_dsp_add_residual_neon:
    cbz x2, 5f
    cbz x3, 5f
    lsl x1, x1, #1
    movi v31.4s, #0
    dup v30.4s, w5
1:
    mov x7, x0
    mov x6, x2
    cmp x6, #8
    b.lo 3f
2:
    ld1 {v0.8h}, [x7]
    ld1 {v1.4s, v2.4s}, [x4], #32
    uxtl v3.4s, v0.4h
    uxtl2 v4.4s, v0.8h
    sqadd v1.4s, v1.4s, v3.4s
    sqadd v2.4s, v2.4s, v4.4s
    smax v1.4s, v1.4s, v31.4s
    smax v2.4s, v2.4s, v31.4s
    smin v1.4s, v1.4s, v30.4s
    smin v2.4s, v2.4s, v30.4s
    xtn v0.4h, v1.4s
    xtn2 v0.8h, v2.4s
    st1 {v0.8h}, [x7], #16
    subs x6, x6, #8
    cmp x6, #8
    b.hs 2b
    cbz x6, 4f
3:
    ld1 {v0.4h}, [x7]
    ld1 {v1.4s}, [x4], #16
    uxtl v2.4s, v0.4h
    sqadd v1.4s, v1.4s, v2.4s
    smax v1.4s, v1.4s, v31.4s
    smin v1.4s, v1.4s, v30.4s
    xtn v0.4h, v1.4s
    st1 {v0.4h}, [x7]
4:
    add x0, x0, x1
    subs x3, x3, #1
    b.ne 1b
5:
    ret

.globl _av1_dsp_inverse_wht4_neon
.p2align 2
_av1_dsp_inverse_wht4_neon:
    ld1 {v0.4s-v3.4s}, [x0]

    trn1 v4.4s, v0.4s, v1.4s
    trn2 v5.4s, v0.4s, v1.4s
    trn1 v6.4s, v2.4s, v3.4s
    trn2 v7.4s, v2.4s, v3.4s
    trn1 v0.2d, v4.2d, v6.2d
    trn2 v2.2d, v4.2d, v6.2d
    trn1 v1.2d, v5.2d, v7.2d
    trn2 v3.2d, v5.2d, v7.2d

    sshr v0.4s, v0.4s, #2
    sshr v1.4s, v1.4s, #2
    sshr v2.4s, v2.4s, #2
    sshr v3.4s, v3.4s, #2
    add v0.4s, v0.4s, v1.4s
    sub v2.4s, v2.4s, v3.4s
    sub v16.4s, v0.4s, v2.4s
    sshr v16.4s, v16.4s, #1
    sub v3.4s, v16.4s, v3.4s
    sub v1.4s, v16.4s, v1.4s
    sub v0.4s, v0.4s, v3.4s
    add v2.4s, v2.4s, v1.4s
    mov v4.16b, v1.16b
    mov v1.16b, v3.16b
    mov v3.16b, v2.16b
    mov v2.16b, v4.16b

    mov w3, #1
    sub w4, w2, #1
    lsl w3, w3, w4
    sub w4, w3, #1
    neg w3, w3
    dup v30.4s, w3
    dup v31.4s, w4
    smax v0.4s, v0.4s, v30.4s
    smax v1.4s, v1.4s, v30.4s
    smax v2.4s, v2.4s, v30.4s
    smax v3.4s, v3.4s, v30.4s
    smin v0.4s, v0.4s, v31.4s
    smin v1.4s, v1.4s, v31.4s
    smin v2.4s, v2.4s, v31.4s
    smin v3.4s, v3.4s, v31.4s

    trn1 v4.4s, v0.4s, v1.4s
    trn2 v5.4s, v0.4s, v1.4s
    trn1 v6.4s, v2.4s, v3.4s
    trn2 v7.4s, v2.4s, v3.4s
    trn1 v0.2d, v4.2d, v6.2d
    trn2 v2.2d, v4.2d, v6.2d
    trn1 v1.2d, v5.2d, v7.2d
    trn2 v3.2d, v5.2d, v7.2d

    add v0.4s, v0.4s, v1.4s
    sub v2.4s, v2.4s, v3.4s
    sub v16.4s, v0.4s, v2.4s
    sshr v16.4s, v16.4s, #1
    sub v3.4s, v16.4s, v3.4s
    sub v1.4s, v16.4s, v1.4s
    sub v0.4s, v0.4s, v3.4s
    add v2.4s, v2.4s, v1.4s
    mov v4.16b, v1.16b
    mov v1.16b, v3.16b
    mov v3.16b, v2.16b
    mov v2.16b, v4.16b
    st1 {v0.4s-v3.4s}, [x1]
    ret
