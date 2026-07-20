.globl _av1_dsp_add_residual_8bpc_neon
.text
.p2align 2
_av1_dsp_add_residual_8bpc_neon:
    cbz x3, 3f
    lsl x1, x1, #1
    movi v31.4s, #0
    movi v30.4s, #255
1:
    mov x5, x0
    mov x6, x2
2:
    ld1 {v0.8h}, [x5]
    ld1 {v1.4s, v2.4s}, [x4], #32
    uxtl v3.4s, v0.4h
    uxtl2 v4.4s, v0.8h
    add v1.4s, v1.4s, v3.4s
    add v2.4s, v2.4s, v4.4s
    smax v1.4s, v1.4s, v31.4s
    smax v2.4s, v2.4s, v31.4s
    smin v1.4s, v1.4s, v30.4s
    smin v2.4s, v2.4s, v30.4s
    xtn v0.4h, v1.4s
    xtn2 v0.8h, v2.4s
    st1 {v0.8h}, [x5], #16
    subs x6, x6, #8
    b.ne 2b
    add x0, x0, x1
    subs x3, x3, #1
    b.ne 1b
3:
    ret