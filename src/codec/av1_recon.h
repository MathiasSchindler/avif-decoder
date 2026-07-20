#ifndef AVIFDEC_AV1_RECON_H
#define AVIFDEC_AV1_RECON_H

#include "av1_coeff.h"

enum { AV1_QM_TOTAL_SIZE = 3344 };

typedef struct {
    uint8_t bit_depth;
    uint8_t q_index;
    uint8_t plane;
    uint8_t using_qmatrix;
    uint8_t qm_level;
    int8_t delta_q_y_dc;
    int8_t delta_q_u_dc;
    int8_t delta_q_u_ac;
    int8_t delta_q_v_dc;
    int8_t delta_q_v_ac;
    const uint8_t *qmatrix;
} Av1DequantParams;

typedef enum {
    AV1_INVERSE_DCT = 0,
    AV1_INVERSE_ADST,
    AV1_INVERSE_IDENTITY,
    AV1_INVERSE_WHT
} Av1Inverse1dType;

uint16_t av1_recon_dc_quant(uint8_t bit_depth, int q_index);
uint16_t av1_recon_ac_quant(uint8_t bit_depth, int q_index);
int32_t av1_recon_cos128(int angle);
AvifdecStatus av1_recon_qmatrix_decode(uint8_t level,
                                       uint8_t chroma,
                                       uint8_t *matrix,
                                       size_t matrix_capacity);
AvifdecStatus av1_recon_quant_step(const Av1DequantParams *params,
                                   Av1TxSize tx_size,
                                   Av1TxType tx_type,
                                   size_t coefficient_index,
                                   uint32_t *step);
AvifdecStatus av1_recon_dequantize(const int32_t *quantized,
                                   size_t quantized_count,
                                   Av1TxSize tx_size,
                                   Av1TxType tx_type,
                                   const Av1DequantParams *params,
                                   int32_t *dequantized,
                                   size_t dequantized_capacity);
AvifdecStatus av1_recon_inverse_1d(int32_t *values,
                                                                     unsigned int length_log2,
                                                                     Av1Inverse1dType type,
                                                                     unsigned int clamp_range,
                                                                     unsigned int wht_shift);
AvifdecStatus av1_recon_inverse_transform(const int32_t *dequantized,
                                                                                    size_t dequantized_count,
                                                                                    Av1TxSize tx_size,
                                                                                    Av1TxType tx_type,
                                                                                    uint8_t bit_depth,
                                                                                    uint8_t lossless,
                                                                                    int32_t *residual,
                                                                                    size_t residual_capacity);
AvifdecStatus av1_recon_add_residual(uint16_t *destination,
                                                                         size_t stride,
                                                                         size_t width,
                                                                         size_t height,
                                                                         const int32_t *residual,
                                                                         size_t residual_count,
                                                                         uint8_t bit_depth,
                                                                         uint8_t flip_lr,
                                                                         uint8_t flip_ud);

#endif