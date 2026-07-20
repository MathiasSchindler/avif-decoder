#ifndef AVIFENC_IMAGE_INPUT_JPEG_H
#define AVIFENC_IMAGE_INPUT_JPEG_H

#include "encoder/cli/image_input.h"

ImageInputStatus image_input_jpeg_query(const uint8_t *input,
                                        size_t input_size,
                                        ImageInputInfo *info);

ImageInputStatus image_input_jpeg_decode(const uint8_t *input,
                                         size_t input_size,
                                         void *workspace,
                                         size_t workspace_size,
                                         uint8_t *output,
                                         size_t output_capacity,
                                         ImageInputInfo *info);

#endif
