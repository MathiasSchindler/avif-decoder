#ifndef AVIFDEC_AV1_DSP_H
#define AVIFDEC_AV1_DSP_H

#include <stddef.h>
#include <stdint.h>

void av1_dsp_add_residual_c(uint16_t *destination,
                            size_t stride,
                            size_t width,
                            size_t height,
                            const int32_t *residual,
                            uint8_t bit_depth,
                            uint8_t flip_lr,
                            uint8_t flip_ud);
void av1_dsp_add_residual(uint16_t *destination,
                          size_t stride,
                          size_t width,
                          size_t height,
                          const int32_t *residual,
                          uint8_t bit_depth,
                          uint8_t flip_lr,
                          uint8_t flip_ud);
void av1_dsp_inverse_dct4_c(const int32_t *input, int32_t *output);
void av1_dsp_inverse_dct4(const int32_t *input, int32_t *output);
void av1_dsp_inverse_dct8_c(const int32_t *input, int32_t *output);
void av1_dsp_inverse_dct8(const int32_t *input, int32_t *output);
void av1_dsp_inverse_dct16_c(const int32_t *input, int32_t *output);
void av1_dsp_inverse_dct16(const int32_t *input, int32_t *output);

#endif