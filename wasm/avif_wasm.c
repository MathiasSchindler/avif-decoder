#include "avifdec.h"
#include "base.h"

#include <stdint.h>
#include <stdlib.h>

#define AVIF_WASM_MAX_DIMENSION 8192U
#define AVIF_WASM_MAX_PIXELS 33554432U
#define AVIF_WASM_MAX_WORKSPACE (768U * 1024U * 1024U)

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

void avif_wasm_reset(void) {
    avif_wasm_release_pixels();
    avif_wasm_stage_value = 0U;
    avifdec_memory_fill(
        &avif_wasm_error_value, 0U, sizeof(avif_wasm_error_value));
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