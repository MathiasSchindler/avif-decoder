#include "avifdec.h"
#include "base.h"
#include "encoder/avifenc.h"
#include "encoder/cli/image_input.h"

#include <stdint.h>
#include <stdlib.h>

#define AVIF_WASM_MAX_DIMENSION 8192U
#define AVIF_WASM_MAX_PIXELS 33554432U
#define AVIF_WASM_MAX_WORKSPACE (768U * 1024U * 1024U)
#define AVIF_WASM_ENCODE_INPUT_ERROR_BASE 256
#define AVIF_WASM_ENCODE_OUT_OF_MEMORY 512
#define AVIF_WASM_ENCODE_LIMIT_EXCEEDED 513

static unsigned char *avif_wasm_pixels;
static size_t avif_wasm_pixel_size;
static uint32_t avif_wasm_width_value;
static uint32_t avif_wasm_height_value;
static uint32_t avif_wasm_source_width_value;
static uint32_t avif_wasm_source_height_value;
static uint8_t avif_wasm_bit_depth_value;
static uint8_t avif_wasm_alpha_value;
static uint32_t avif_wasm_stage_value;
static AvifdecError avif_wasm_error_value;
static unsigned char *avif_wasm_encoded;
static size_t avif_wasm_encoded_size;
static uint32_t avif_wasm_encoded_width_value;
static uint32_t avif_wasm_encoded_height_value;
static uint32_t avif_wasm_encoder_stage_value;
static AvifencError avif_wasm_encoder_error_value;

static void avif_wasm_release_pixels(void) {
    free(avif_wasm_pixels);
    avif_wasm_pixels = 0;
    avif_wasm_pixel_size = 0U;
    avif_wasm_width_value = 0U;
    avif_wasm_height_value = 0U;
    avif_wasm_source_width_value = 0U;
    avif_wasm_source_height_value = 0U;
    avif_wasm_bit_depth_value = 0U;
    avif_wasm_alpha_value = 0U;
}

static void avif_wasm_release_encoded(void) {
    free(avif_wasm_encoded);
    avif_wasm_encoded = 0;
    avif_wasm_encoded_size = 0U;
    avif_wasm_encoded_width_value = 0U;
    avif_wasm_encoded_height_value = 0U;
}

void avif_wasm_reset(void) {
    avif_wasm_release_pixels();
    avif_wasm_release_encoded();
    avif_wasm_stage_value = 0U;
    avif_wasm_encoder_stage_value = 0U;
    avifdec_memory_fill(
        &avif_wasm_error_value, 0U, sizeof(avif_wasm_error_value));
    avifdec_memory_fill(
        &avif_wasm_encoder_error_value, 0U,
        sizeof(avif_wasm_encoder_error_value));
}

static uint8_t avif_wasm_color_clamp(int32_t value) {
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return (uint8_t)value;
}

static int32_t avif_wasm_color_divide_256(int32_t value) {
    return value >= 0
        ? (value + 128) / 256
        : -((-value + 128) / 256);
}

static uint8_t avif_wasm_color_luma(uint8_t red,
                                    uint8_t green,
                                    uint8_t blue) {
    return avif_wasm_color_clamp(
        16 + (47 * (int32_t)red + 157 * (int32_t)green +
              16 * (int32_t)blue + 128) / 256);
}

static uint8_t avif_wasm_color_blue_difference(uint8_t red,
                                               uint8_t green,
                                               uint8_t blue) {
    return avif_wasm_color_clamp(
        128 + avif_wasm_color_divide_256(
            -26 * (int32_t)red - 87 * (int32_t)green +
            112 * (int32_t)blue));
}

static uint8_t avif_wasm_color_red_difference(uint8_t red,
                                              uint8_t green,
                                              uint8_t blue) {
    return avif_wasm_color_clamp(
        128 + avif_wasm_color_divide_256(
            112 * (int32_t)red - 102 * (int32_t)green -
            10 * (int32_t)blue));
}

static void avif_wasm_rgb_to_yuv420(const uint8_t *rgb,
                                    const ImageInputInfo *source,
                                    const AvifencImage *image,
                                    unsigned char *yuv,
                                    size_t luma_size,
                                    size_t chroma_size) {
    unsigned char *luma = yuv;
    unsigned char *blue_difference = yuv + luma_size;
    unsigned char *red_difference = blue_difference + chroma_size;
    uint32_t output_y;

    for (output_y = 0U; output_y < image->height; ++output_y) {
        uint32_t source_y = output_y < source->height
            ? output_y : source->height - 1U;
        uint32_t output_x;

        for (output_x = 0U; output_x < image->width; ++output_x) {
            uint32_t source_x = output_x < source->width
                ? output_x : source->width - 1U;
            const uint8_t *pixel = rgb + (size_t)source_y *
                source->rgb_stride + (size_t)source_x * 3U;

            luma[(size_t)output_y * image->width + output_x] =
                avif_wasm_color_luma(pixel[0], pixel[1], pixel[2]);
        }
    }
    for (output_y = 0U; output_y < image->height / 2U; ++output_y) {
        uint32_t output_x;

        for (output_x = 0U; output_x < image->width / 2U; ++output_x) {
            uint32_t red = 0U;
            uint32_t green = 0U;
            uint32_t blue = 0U;
            unsigned int offset_y;

            for (offset_y = 0U; offset_y < 2U; ++offset_y) {
                uint32_t source_y = output_y * 2U + offset_y;
                unsigned int offset_x;

                if (source_y >= source->height) {
                    source_y = source->height - 1U;
                }
                for (offset_x = 0U; offset_x < 2U; ++offset_x) {
                    uint32_t source_x = output_x * 2U + offset_x;
                    const uint8_t *pixel;

                    if (source_x >= source->width) {
                        source_x = source->width - 1U;
                    }
                    pixel = rgb + (size_t)source_y * source->rgb_stride +
                        (size_t)source_x * 3U;
                    red += pixel[0];
                    green += pixel[1];
                    blue += pixel[2];
                }
            }
            red = (red + 2U) / 4U;
            green = (green + 2U) / 4U;
            blue = (blue + 2U) / 4U;
            blue_difference[(size_t)output_y * (image->width / 2U) +
                            output_x] = avif_wasm_color_blue_difference(
                                (uint8_t)red, (uint8_t)green, (uint8_t)blue);
            red_difference[(size_t)output_y * (image->width / 2U) +
                           output_x] = avif_wasm_color_red_difference(
                               (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        }
    }
}

static AvifdecStatus avif_wasm_allocate_image(
    const AvifdecImageInfo *info,
    AvifdecImage *image,
    void **memory) {
    size_t luma_samples;
    size_t chroma_samples = 0U;
    size_t sample_count;
    size_t memory_size;

    if (!avifdec_size_multiply(
            info->width, info->height, &luma_samples)) {
        return AVIFDEC_OVERFLOW;
    }
    sample_count = luma_samples;
    if (!info->monochrome) {
        size_t chroma_width =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        size_t chroma_height =
            (info->height + ((size_t)1U << info->subsampling_y) - 1U) >>
            info->subsampling_y;

        if (!avifdec_size_multiply(
                chroma_width, chroma_height, &chroma_samples) ||
            chroma_samples > (SIZE_MAX - sample_count) / 2U) {
            return AVIFDEC_OVERFLOW;
        }
        sample_count += 2U * chroma_samples;
    }
    if (info->has_alpha) {
        if (luma_samples > SIZE_MAX - sample_count) {
            return AVIFDEC_OVERFLOW;
        }
        sample_count += luma_samples;
    }
    if (!avifdec_size_multiply(
            sample_count, sizeof(uint16_t), &memory_size)) {
        return AVIFDEC_OVERFLOW;
    }
    *memory = malloc(memory_size);
    if (*memory == 0) return AVIFDEC_OUT_OF_MEMORY;

    avifdec_memory_fill(image, 0U, sizeof(*image));
    image->planes[0] = (uint16_t *)*memory;
    image->strides[0] = info->width;
    if (!info->monochrome) {
        image->planes[1] = image->planes[0] + luma_samples;
        image->planes[2] = image->planes[1] + chroma_samples;
        image->strides[1] =
            (info->width + ((size_t)1U << info->subsampling_x) - 1U) >>
            info->subsampling_x;
        image->strides[2] = image->strides[1];
    }
    if (info->has_alpha) {
        image->alpha_plane =
            image->planes[0] + luma_samples + 2U * chroma_samples;
        image->alpha_stride = info->width;
    }
    return AVIFDEC_OK;
}

int avif_wasm_decode(const unsigned char *data, size_t size) {
    AvifdecLimits limits;
    AvifdecImageInfo info;
    AvifdecImage image;
    AvifdecRgbImage rgb;
    AvifdecStatus status;
    void *workspace = 0;
    void *image_memory = 0;
    size_t row_size;

    avif_wasm_reset();
    if (data == 0 || size == 0U) return AVIFDEC_INVALID_ARGUMENT;

    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    limits.max_width = AVIF_WASM_MAX_DIMENSION;
    limits.max_height = AVIF_WASM_MAX_DIMENSION;
    limits.max_pixels = AVIF_WASM_MAX_PIXELS;
    avif_wasm_stage_value = 1U;
    status = avifdec_query(
        data, size, &limits, 0, 0U, &info, &avif_wasm_error_value);
    if (status != AVIFDEC_OK) return status;
    if (info.workspace_required > AVIF_WASM_MAX_WORKSPACE) {
        avif_wasm_error_value.status = AVIFDEC_LIMIT_EXCEEDED;
        return AVIFDEC_LIMIT_EXCEEDED;
    }

    workspace = malloc(info.workspace_required);
    if (workspace == 0 && info.workspace_required != 0U) {
        return AVIFDEC_OUT_OF_MEMORY;
    }
    status = avif_wasm_allocate_image(&info, &image, &image_memory);
    if (status != AVIFDEC_OK) goto cleanup;
    avif_wasm_stage_value = 2U;
    status = avifdec_decode(
        data, size, &limits, workspace, info.workspace_required,
        &image, 0, &avif_wasm_error_value);
    if (status != AVIFDEC_OK) goto cleanup;

    if (!avifdec_size_multiply(
            info.presentation_width, 4U, &row_size) ||
        !avifdec_size_multiply(
            row_size, info.presentation_height,
            &avif_wasm_pixel_size)) {
        status = AVIFDEC_OVERFLOW;
        goto cleanup;
    }
    avif_wasm_pixels = (unsigned char *)malloc(avif_wasm_pixel_size);
    if (avif_wasm_pixels == 0) {
        status = AVIFDEC_OUT_OF_MEMORY;
        goto cleanup;
    }
    rgb.pixels = avif_wasm_pixels;
    rgb.stride = row_size;
    rgb.width = info.presentation_width;
    rgb.height = info.presentation_height;
    rgb.format = AVIFDEC_RGBA8;
    rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    avif_wasm_stage_value = 3U;
    status = avifdec_image_to_rgb(
        &image, &info, &rgb, &avif_wasm_error_value);
    if (status != AVIFDEC_OK) goto cleanup;

    avif_wasm_width_value = rgb.width;
    avif_wasm_height_value = rgb.height;
    avif_wasm_source_width_value = info.width;
    avif_wasm_source_height_value = info.height;
    avif_wasm_bit_depth_value = info.bit_depth;
    avif_wasm_alpha_value = info.has_alpha;

cleanup:
    free(image_memory);
    free(workspace);
    if (status != AVIFDEC_OK) avif_wasm_release_pixels();
    return status;
}

int avif_wasm_encode(const unsigned char *data,
                     size_t size,
                     uint32_t quantizer,
                     uint32_t speed) {
    ImageInputInfo source;
    ImageInputStatus input_status;
    AvifencImage image;
    AvifencOptions options;
    AvifencRequirements requirements;
    int status;
    void *image_workspace = 0;
    unsigned char *rgb = 0;
    unsigned char *yuv = 0;
    void *workspace = 0;
    size_t pixels;
    size_t luma_size;
    size_t chroma_size;
    size_t yuv_size;
    size_t memory_required;

    avif_wasm_reset();
    if (data == 0 || size == 0U) {
        return AVIF_WASM_ENCODE_INPUT_ERROR_BASE +
            IMAGE_INPUT_INVALID_ARGUMENT;
    }
    if (quantizer > 255U || speed > AVIFENC_MAX_SPEED) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    avif_wasm_encoder_stage_value = 1U;
    input_status = image_input_query(data, size, &source);
    if (input_status != IMAGE_INPUT_OK) {
        return AVIF_WASM_ENCODE_INPUT_ERROR_BASE + input_status;
    }
    if (source.width > AVIF_WASM_MAX_DIMENSION ||
        source.height > AVIF_WASM_MAX_DIMENSION ||
        !avifdec_size_multiply(source.width, source.height, &pixels) ||
        pixels > AVIF_WASM_MAX_PIXELS) {
        return AVIF_WASM_ENCODE_LIMIT_EXCEEDED;
    }

    avifdec_memory_fill(&image, 0U, sizeof(image));
    image.width = (source.width + 1U) & ~1U;
    image.height = (source.height + 1U) & ~1U;
    if (!avifdec_size_multiply(image.width, image.height, &luma_size) ||
        !avifdec_size_multiply(
            image.width / 2U, image.height / 2U, &chroma_size) ||
        chroma_size > (SIZE_MAX - luma_size) / 2U) {
        return AVIF_WASM_ENCODE_LIMIT_EXCEEDED;
    }
    yuv_size = luma_size + 2U * chroma_size;
    if (source.workspace_size > AVIF_WASM_MAX_WORKSPACE ||
        source.output_size > AVIF_WASM_MAX_WORKSPACE ||
        !avifdec_size_add(
            source.workspace_size, source.output_size, &memory_required) ||
        !avifdec_size_add(memory_required, yuv_size, &memory_required) ||
        memory_required > AVIF_WASM_MAX_WORKSPACE) {
        return AVIF_WASM_ENCODE_LIMIT_EXCEEDED;
    }
    image_workspace = malloc(source.workspace_size);
    rgb = (unsigned char *)malloc(source.output_size);
    yuv = (unsigned char *)malloc(yuv_size);
    if ((image_workspace == 0 && source.workspace_size != 0U) ||
        (rgb == 0 && source.output_size != 0U) || yuv == 0) {
        status = AVIF_WASM_ENCODE_OUT_OF_MEMORY;
        goto cleanup;
    }

    avif_wasm_encoder_stage_value = 2U;
    input_status = image_input_decode(
        data, size, image_workspace, source.workspace_size,
        rgb, source.output_size, &source);
    if (input_status != IMAGE_INPUT_OK) {
        status = AVIF_WASM_ENCODE_INPUT_ERROR_BASE + input_status;
        goto cleanup;
    }
    image.planes[0] = yuv;
    image.planes[1] = yuv + luma_size;
    image.planes[2] = yuv + luma_size + chroma_size;
    image.strides[0] = image.width;
    image.strides[1] = image.width / 2U;
    image.strides[2] = image.width / 2U;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    avif_wasm_rgb_to_yuv420(
        rgb, &source, &image, yuv, luma_size, chroma_size);
    free(rgb);
    rgb = 0;
    free(image_workspace);
    image_workspace = 0;

    avifenc_options_default(&options);
    options.quantizer = (uint16_t)quantizer;
    options.speed = (uint8_t)speed;
    avif_wasm_encoder_stage_value = 3U;
    status = avifenc_query(
        &image, &options, &requirements, &avif_wasm_encoder_error_value);
    if (status != AVIFENC_OK) goto cleanup;
    if (!avifdec_size_add(
            yuv_size, requirements.workspace_required, &memory_required) ||
        !avifdec_size_add(
            memory_required, requirements.output_capacity_required,
            &memory_required) ||
        memory_required > AVIF_WASM_MAX_WORKSPACE) {
        status = AVIF_WASM_ENCODE_LIMIT_EXCEEDED;
        goto cleanup;
    }
    workspace = malloc(requirements.workspace_required);
    avif_wasm_encoded = (unsigned char *)malloc(
        requirements.output_capacity_required);
    if ((workspace == 0 && requirements.workspace_required != 0U) ||
        (avif_wasm_encoded == 0 &&
         requirements.output_capacity_required != 0U)) {
        status = AVIF_WASM_ENCODE_OUT_OF_MEMORY;
        goto cleanup;
    }

    avif_wasm_encoder_stage_value = 4U;
    status = avifenc_encode(
        &image, &options, workspace, requirements.workspace_required,
        avif_wasm_encoded, requirements.output_capacity_required,
        &avif_wasm_encoded_size, &avif_wasm_encoder_error_value);
    if (status != AVIFENC_OK) goto cleanup;
    avif_wasm_encoded_width_value = image.width;
    avif_wasm_encoded_height_value = image.height;

cleanup:
    free(workspace);
    free(yuv);
    free(rgb);
    free(image_workspace);
    if (status != AVIFENC_OK) avif_wasm_release_encoded();
    return status;
}

uintptr_t avif_wasm_pixel_pointer(void) {
    return (uintptr_t)avif_wasm_pixels;
}

size_t avif_wasm_pixel_bytes(void) {
    return avif_wasm_pixel_size;
}

uint32_t avif_wasm_width(void) {
    return avif_wasm_width_value;
}

uint32_t avif_wasm_height(void) {
    return avif_wasm_height_value;
}

uint32_t avif_wasm_source_width(void) {
    return avif_wasm_source_width_value;
}

uint32_t avif_wasm_source_height(void) {
    return avif_wasm_source_height_value;
}

uint32_t avif_wasm_bit_depth(void) {
    return avif_wasm_bit_depth_value;
}

uint32_t avif_wasm_has_alpha(void) {
    return avif_wasm_alpha_value;
}

uint32_t avif_wasm_stage(void) {
    return avif_wasm_stage_value;
}

size_t avif_wasm_error_offset(void) {
    return avif_wasm_error_value.offset;
}

uint32_t avif_wasm_error_context(void) {
    return avif_wasm_error_value.context;
}

uintptr_t avif_wasm_encoded_pointer(void) {
    return (uintptr_t)avif_wasm_encoded;
}

size_t avif_wasm_encoded_bytes(void) {
    return avif_wasm_encoded_size;
}

uint32_t avif_wasm_encoded_width(void) {
    return avif_wasm_encoded_width_value;
}

uint32_t avif_wasm_encoded_height(void) {
    return avif_wasm_encoded_height_value;
}

uint32_t avif_wasm_encoder_stage(void) {
    return avif_wasm_encoder_stage_value;
}

uint32_t avif_wasm_encoder_error_context(void) {
    return avif_wasm_encoder_error_value.context;
}