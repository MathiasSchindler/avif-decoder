#ifndef AVIFENC_AV1_TRANSFORM_FORWARD_H
#define AVIFENC_AV1_TRANSFORM_FORWARD_H

#include "encoder/avifenc.h"
#include "av1_recon.h"

AvifencStatus avifenc_av1_forward_dct_4x4(const int16_t *input,
                                           size_t stride,
                                           int32_t output[16]);
AvifencStatus avifenc_av1_forward_dct(const int16_t *input,
                                      size_t stride,
                                      Av1TxSize tx_size,
                                      int32_t *output,
                                      size_t output_capacity);
void avifenc_av1_forward_wht_4x4(const int16_t input[16],
                                 int32_t output[16]);
int avifenc_av1_transform_size_supported(Av1TxSize tx_size);

#endif
