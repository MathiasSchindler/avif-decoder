#ifndef AVIFENC_AV1_TRANSFORM_WRITE_H
#define AVIFENC_AV1_TRANSFORM_WRITE_H

#include "encoder/av1_transform_forward.h"
#include "encoder/av1_symbol_write.h"
#include "av1_coeff.h"
#include "av1_recon.h"

typedef struct {
    int32_t quantized[1024];
    uint16_t eob;
} AvifencAv1TransformBlock;

typedef struct {
    Av1CoeffCdfs cdfs;
    Av1CoeffContextState contexts;
    uint16_t tx_type_set2[6];
    Av1DequantParams dequant[3];
    uint8_t *matrix_workspace;
} AvifencAv1TransformState;

AvifencStatus avifenc_av1_quantize_4x4(const int32_t input[16],
                                       uint8_t quantizer,
                                       AvifencAv1TransformBlock *block);
AvifencStatus avifenc_av1_quantize(const int32_t *input,
                                   Av1TxSize tx_size,
                                   uint8_t quantizer,
                                   AvifencAv1TransformBlock *block);
AvifencStatus avifenc_av1_transform_context_size(uint32_t mi_columns,
                                                 uint32_t mi_rows,
                                                 size_t *required);
AvifencStatus avifenc_av1_transform_state_init(
    AvifencAv1TransformState *state,
    uint8_t quantizer,
    uint32_t mi_columns,
    uint32_t mi_rows,
    void *workspace,
    size_t workspace_size);
AvifencStatus avifenc_av1_transform_state_set_quantization(
    AvifencAv1TransformState *state,
    const AvifencQuantization *quantization,
    uint8_t quantizer,
    void *matrix_workspace,
    size_t matrix_workspace_size);
AvifencStatus avifenc_av1_transform_encode_4x4(
    AvifencAv1TransformState *state,
    AvifencAv1SymbolWriter *writer,
    unsigned int plane,
    size_t x4,
    size_t y4,
    const uint8_t *source,
    size_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    uint16_t *reconstruction,
    size_t reconstruction_stride,
    uint8_t quantizer,
    int write_tx_type,
    AvifencAv1TransformBlock *block);
AvifencStatus avifenc_av1_transform_trial_4x4(
    const AvifencAv1TransformState *state,
    unsigned int plane,
    size_t x4,
    size_t y4,
    const uint8_t *source,
    size_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    uint16_t *reconstruction,
    size_t reconstruction_stride,
    uint8_t quantizer,
    int write_tx_type,
    AvifencAv1TransformBlock *block,
    uint64_t *distortion,
    uint64_t *rate_cost);
AvifencStatus avifenc_av1_transform_encode(
    AvifencAv1TransformState *state,
    AvifencAv1SymbolWriter *writer,
    unsigned int plane,
    size_t x4,
    size_t y4,
    size_t block_width,
    size_t block_height,
    Av1TxSize tx_size,
    const uint8_t *source,
    size_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    uint16_t *reconstruction,
    size_t reconstruction_stride,
    uint8_t quantizer,
    Av1TxType tx_type,
    int write_tx_type,
    AvifencAv1TransformBlock *block);
AvifencStatus avifenc_av1_transform_trial(
    const AvifencAv1TransformState *state,
    unsigned int plane,
    size_t x4,
    size_t y4,
    size_t block_width,
    size_t block_height,
    Av1TxSize tx_size,
    const uint8_t *source,
    size_t source_stride,
    uint32_t source_width,
    uint32_t source_height,
    uint16_t *reconstruction,
    size_t reconstruction_stride,
    uint8_t quantizer,
    Av1TxType tx_type,
    int write_tx_type,
    AvifencAv1TransformBlock *block,
    uint64_t *distortion,
    uint64_t *rate_cost);

#endif