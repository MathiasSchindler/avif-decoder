#ifndef AVIFENC_IMAGE_INPUT_H
#define AVIFENC_IMAGE_INPUT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    IMAGE_INPUT_OK = 0,
    IMAGE_INPUT_INVALID_ARGUMENT,
    IMAGE_INPUT_TRUNCATED,
    IMAGE_INPUT_INVALID_DATA,
    IMAGE_INPUT_OVERFLOW,
    IMAGE_INPUT_LIMIT_EXCEEDED,
    IMAGE_INPUT_WORKSPACE_TOO_SMALL,
    IMAGE_INPUT_OUTPUT_TOO_SMALL,
    IMAGE_INPUT_UNSUPPORTED
} ImageInputStatus;

typedef enum {
    IMAGE_INPUT_FORMAT_UNKNOWN = 0,
    IMAGE_INPUT_FORMAT_PNG,
    IMAGE_INPUT_FORMAT_JPEG
} ImageInputFormat;

typedef struct {
    ImageInputFormat format;
    uint32_t width;
    uint32_t height;
    size_t rgb_stride;
    size_t output_size;
    size_t workspace_size;
} ImageInputInfo;

const char *image_input_status_string(ImageInputStatus status);

/*
 * Inspect an in-memory PNG or JPEG and return caller-owned buffer
 * requirements for packed 8-bit RGB output. Alpha and PNG transparency are
 * discarded. Output size is exact; workspace size is a conservative bound
 * that permits an unaligned workspace pointer. Input bytes must remain
 * unchanged between query and decode.
 */
ImageInputStatus image_input_query(const void *input,
                                   size_t input_size,
                                   ImageInputInfo *info);

/*
 * Decode to packed RGB with no allocation or platform I/O. workspace and
 * output must not overlap each other or the input. On success, info receives
 * the same values as image_input_query().
 */
ImageInputStatus image_input_decode(const void *input,
                                    size_t input_size,
                                    void *workspace,
                                    size_t workspace_size,
                                    void *output,
                                    size_t output_capacity,
                                    ImageInputInfo *info);

#endif