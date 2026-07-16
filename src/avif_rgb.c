#include "base.h"

typedef struct {
    int32_t red_cr;
    int32_t green_cb;
    int32_t green_cr;
    int32_t blue_cb;
} AvifRgbMatrix;

static AvifdecStatus avif_rgb_fail(AvifdecError *error,
                                   AvifdecStatus status) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = 0U;
        error->context = 0U;
    }
    return status;
}

static int avif_rgb_bit_depth_valid(uint8_t bit_depth) {
    return bit_depth == 8U || bit_depth == 10U || bit_depth == 12U;
}

static uint32_t avif_rgb_max_sample(uint8_t bit_depth) {
    return ((uint32_t)1U << bit_depth) - 1U;
}

static int64_t avif_rgb_div_round(int64_t numerator, int64_t denominator) {
    if (numerator < 0) {
        return -((-numerator + denominator / 2) / denominator);
    }
    return (numerator + denominator / 2) / denominator;
}

static uint16_t avif_rgb_clamp_u16(int64_t value) {
    if (value <= 0) return 0U;
    if (value >= 65535) return 65535U;
    return (uint16_t)value;
}

static uint16_t avif_rgb_normalize_luma(uint16_t sample,
                                        uint8_t bit_depth,
                                        uint8_t full_range) {
    uint32_t maximum = avif_rgb_max_sample(bit_depth);
    int64_t numerator;
    int64_t denominator;

    if (full_range != 0U) {
        numerator = (int64_t)sample * 65535;
        denominator = (int64_t)maximum;
    } else {
        uint32_t shift = (uint32_t)bit_depth - 8U;
        uint32_t minimum = (uint32_t)16U << shift;
        uint32_t range = (uint32_t)219U << shift;

        numerator = ((int64_t)sample - (int64_t)minimum) * 65535;
        denominator = (int64_t)range;
    }
    return avif_rgb_clamp_u16(
        avif_rgb_div_round(numerator, denominator));
}

static int32_t avif_rgb_normalize_chroma(uint16_t sample,
                                         uint8_t bit_depth,
                                         uint8_t full_range) {
    uint32_t center = (uint32_t)1U << ((uint32_t)bit_depth - 1U);
    uint32_t range = full_range != 0U
        ? avif_rgb_max_sample(bit_depth)
        : (uint32_t)224U << ((uint32_t)bit_depth - 8U);
    int64_t numerator =
        ((int64_t)sample - (int64_t)center) * 65535;

    return (int32_t)avif_rgb_div_round(numerator, (int64_t)range);
}

static uint16_t avif_rgb_premultiply(uint16_t color, uint16_t alpha) {
    uint64_t product = (uint64_t)color * (uint64_t)alpha;

    return (uint16_t)((product + 32767U) / 65535U);
}

static uint16_t avif_rgb_unpremultiply(uint16_t color, uint16_t alpha) {
    uint64_t numerator;
    uint64_t result;

    if (alpha == 0U) return 0U;
    numerator = (uint64_t)color * 65535U + (uint64_t)alpha / 2U;
    result = numerator / (uint64_t)alpha;
    return result > 65535U ? 65535U : (uint16_t)result;
}

static int avif_rgb_plane_valid(const uint16_t *plane,
                                size_t stride,
                                uint32_t width,
                                uint32_t height) {
    size_t row_offset;
    size_t last_sample;

    if (plane == 0 || width == 0U || height == 0U ||
        stride < (size_t)width) {
        return 0;
    }
    if (!avifdec_size_multiply(
            (size_t)height - 1U, stride, &row_offset) ||
        !avifdec_size_add(
            row_offset, (size_t)width - 1U, &last_sample) ||
        last_sample > SIZE_MAX / sizeof(uint16_t)) {
        return 0;
    }
    return 1;
}

static uint32_t avif_rgb_subsampled_dimension(uint32_t dimension,
                                              uint8_t subsampling) {
    return ((dimension - 1U) >> subsampling) + 1U;
}

static int avif_rgb_matrix(uint16_t coefficients,
                           AvifRgbMatrix *matrix) {
    if (coefficients == 1U) {
        matrix->red_cr = 103206;
        matrix->green_cb = 12276;
        matrix->green_cr = 30679;
        matrix->blue_cb = 121609;
    } else if (coefficients == 2U || coefficients == 6U) {
        matrix->red_cr = 91881;
        matrix->green_cb = 22553;
        matrix->green_cr = 46802;
        matrix->blue_cb = 116130;
    } else if (coefficients == 9U) {
        matrix->red_cr = 96639;
        matrix->green_cb = 10784;
        matrix->green_cr = 37444;
        matrix->blue_cb = 123299;
    } else {
        return 0;
    }
    return 1;
}

static void avif_rgb_inverse_map(const AvifdecImageInfo *info,
                                 uint32_t output_x,
                                 uint32_t output_y,
                                 uint32_t *source_x,
                                 uint32_t *source_y) {
    uint32_t crop_width = info->crop.width;
    uint32_t crop_height = info->crop.height;
    uint8_t angle = (info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U
        ? info->irot_angle
        : 0U;
    uint32_t rotated_width =
        (angle & 1U) != 0U ? crop_height : crop_width;
    uint32_t rotated_height =
        (angle & 1U) != 0U ? crop_width : crop_height;
    uint32_t rotated_x = output_x;
    uint32_t rotated_y = output_y;
    uint32_t crop_x;
    uint32_t crop_y;

    if ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U) {
        if (info->imir_axis == 0U) {
            rotated_y = rotated_height - 1U - output_y;
        } else {
            rotated_x = rotated_width - 1U - output_x;
        }
    }

    if (angle == 1U) {
        crop_x = crop_width - 1U - rotated_y;
        crop_y = rotated_x;
    } else if (angle == 2U) {
        crop_x = crop_width - 1U - rotated_x;
        crop_y = crop_height - 1U - rotated_y;
    } else if (angle == 3U) {
        crop_x = rotated_y;
        crop_y = crop_height - 1U - rotated_x;
    } else {
        crop_x = rotated_x;
        crop_y = rotated_y;
    }

    *source_x = info->crop.x + crop_x;
    *source_y = info->crop.y + crop_y;
}

static void avif_rgb_convert_yuv(uint16_t y_sample,
                                 uint16_t u_sample,
                                 uint16_t v_sample,
                                 uint8_t bit_depth,
                                 uint8_t color_range,
                                 const AvifRgbMatrix *matrix,
                                 uint16_t *red,
                                 uint16_t *green,
                                 uint16_t *blue) {
    uint16_t y = avif_rgb_normalize_luma(
        y_sample, bit_depth, color_range);
    int32_t cb = avif_rgb_normalize_chroma(
        u_sample, bit_depth, color_range);
    int32_t cr = avif_rgb_normalize_chroma(
        v_sample, bit_depth, color_range);
    int64_t y_fixed = (int64_t)y * 65536;

    *red = avif_rgb_clamp_u16(avif_rgb_div_round(
        y_fixed + (int64_t)matrix->red_cr * cr, 65536));
    *green = avif_rgb_clamp_u16(avif_rgb_div_round(
        y_fixed -
            (int64_t)matrix->green_cb * cb -
            (int64_t)matrix->green_cr * cr,
        65536));
    *blue = avif_rgb_clamp_u16(avif_rgb_div_round(
        y_fixed + (int64_t)matrix->blue_cb * cb, 65536));
}

static uint8_t avif_rgb_to_u8(uint16_t value) {
    return (uint8_t)(((uint32_t)value * 255U + 32767U) / 65535U);
}

static void avif_rgb_store_u16(unsigned char *destination,
                               uint16_t value) {
    avifdec_memory_copy(destination, &value, sizeof(value));
}

AvifdecStatus avifdec_image_to_rgb(const AvifdecImage *image,
                                   const AvifdecImageInfo *info,
                                   AvifdecRgbImage *rgb,
                                   AvifdecError *error) {
    AvifRgbMatrix matrix = { 0, 0, 0, 0 };
    uint32_t source_width;
    uint32_t source_height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    uint32_t chroma_width = 0U;
    uint32_t chroma_height = 0U;
    size_t channel_count;
    size_t bytes_per_channel;
    size_t bytes_per_pixel;
    size_t row_bytes;
    size_t output_last_row;
    size_t output_size;
    uint32_t output_y;
    int has_output_alpha;
    int is_16_bit;

    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    if (image == 0 || info == 0 || rgb == 0 || rgb->pixels == 0) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }
    if (rgb->format > AVIFDEC_RGBA16 ||
        rgb->alpha_mode > AVIFDEC_ALPHA_PREMULTIPLIED ||
        !avif_rgb_bit_depth_valid(image->bit_depth) ||
        image->bit_depth != info->bit_depth ||
        image->monochrome > 1U || info->monochrome > 1U ||
        image->monochrome != info->monochrome ||
        image->subsampling_x != info->subsampling_x ||
        image->subsampling_y != info->subsampling_y ||
        image->subsampling_x > 1U || image->subsampling_y > 1U ||
        info->color_range > 1U || info->has_alpha > 1U) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }
    if (info->matrix_coefficients != 0U &&
        !avif_rgb_matrix(info->matrix_coefficients, &matrix)) {
        return avif_rgb_fail(error, AVIFDEC_UNSUPPORTED);
    }
    if (!image->monochrome && info->matrix_coefficients == 0U &&
        (image->subsampling_x != 0U || image->subsampling_y != 0U)) {
        return avif_rgb_fail(error, AVIFDEC_UNSUPPORTED);
    }
    if ((info->transform_flags &
         (uint8_t)~(AVIFDEC_TRANSFORM_CLAP |
                    AVIFDEC_TRANSFORM_IROT |
                    AVIFDEC_TRANSFORM_IMIR |
                    AVIFDEC_TRANSFORM_PASP)) != 0U ||
        ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
         info->irot_angle > 3U) ||
        ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U &&
         info->imir_axis > 1U)) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }

    source_width = info->width;
    source_height = info->height;
    if (source_width == 0U || source_height == 0U ||
        image->widths[0] != source_width ||
        image->heights[0] != source_height ||
        info->crop.width == 0U || info->crop.height == 0U ||
        info->crop.x > source_width ||
        info->crop.y > source_height ||
        info->crop.width > source_width - info->crop.x ||
        info->crop.height > source_height - info->crop.y ||
        !avif_rgb_plane_valid(image->planes[0], image->strides[0],
                              source_width, source_height)) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }

    presentation_width = info->crop.width;
    presentation_height = info->crop.height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
        (info->irot_angle & 1U) != 0U) {
        uint32_t swap = presentation_width;

        presentation_width = presentation_height;
        presentation_height = swap;
    }
    if (info->presentation_width != presentation_width ||
        info->presentation_height != presentation_height ||
        rgb->width != presentation_width ||
        rgb->height != presentation_height) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }

    if (!image->monochrome) {
        chroma_width = avif_rgb_subsampled_dimension(
            source_width, image->subsampling_x);
        chroma_height = avif_rgb_subsampled_dimension(
            source_height, image->subsampling_y);
        if (image->widths[1] != chroma_width ||
            image->widths[2] != chroma_width ||
            image->heights[1] != chroma_height ||
            image->heights[2] != chroma_height ||
            !avif_rgb_plane_valid(
                image->planes[1], image->strides[1],
                chroma_width, chroma_height) ||
            !avif_rgb_plane_valid(
                image->planes[2], image->strides[2],
                chroma_width, chroma_height)) {
            return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
        }
    }

    if (info->has_alpha != 0U) {
        if (!avif_rgb_bit_depth_valid(image->alpha_bit_depth) ||
            image->alpha_bit_depth != info->alpha_bit_depth ||
            image->alpha_color_range > 1U ||
            image->alpha_color_range != info->alpha_color_range ||
            image->alpha_premultiplied > 1U ||
            image->alpha_premultiplied != info->alpha_premultiplied ||
            image->alpha_width != source_width ||
            image->alpha_height != source_height ||
            !avif_rgb_plane_valid(
                image->alpha_plane, image->alpha_stride,
                source_width, source_height)) {
            return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
        }
    }

    has_output_alpha = rgb->format == AVIFDEC_RGBA8 ||
                       rgb->format == AVIFDEC_RGBA16;
    is_16_bit = rgb->format == AVIFDEC_RGB16 ||
                rgb->format == AVIFDEC_RGBA16;
    channel_count = has_output_alpha ? 4U : 3U;
    bytes_per_channel = is_16_bit ? sizeof(uint16_t) : 1U;
    if (!avifdec_size_multiply(
            channel_count, bytes_per_channel, &bytes_per_pixel) ||
        !avifdec_size_multiply(
            (size_t)presentation_width, bytes_per_pixel, &row_bytes)) {
        return avif_rgb_fail(error, AVIFDEC_OVERFLOW);
    }
    if (rgb->stride < row_bytes) {
        return avif_rgb_fail(error, AVIFDEC_INVALID_ARGUMENT);
    }
    if (!avifdec_size_multiply(
            (size_t)presentation_height - 1U,
            rgb->stride, &output_last_row) ||
        !avifdec_size_add(output_last_row, row_bytes, &output_size)) {
        return avif_rgb_fail(error, AVIFDEC_OVERFLOW);
    }
    (void)output_size;

    for (output_y = 0U; output_y < presentation_height; ++output_y) {
        unsigned char *output_row =
            (unsigned char *)rgb->pixels +
            (size_t)output_y * rgb->stride;
        uint32_t output_x;

        for (output_x = 0U; output_x < presentation_width; ++output_x) {
            uint32_t source_x;
            uint32_t source_y;
            size_t luma_index;
            uint16_t y_sample;
            uint16_t red;
            uint16_t green;
            uint16_t blue;
            uint16_t alpha = 65535U;
            unsigned char *pixel =
                output_row + (size_t)output_x * bytes_per_pixel;

            avif_rgb_inverse_map(
                info, output_x, output_y, &source_x, &source_y);
            luma_index =
                (size_t)source_y * image->strides[0] + source_x;
            y_sample = image->planes[0][luma_index];
            if (y_sample > avif_rgb_max_sample(image->bit_depth)) {
                return avif_rgb_fail(error, AVIFDEC_INVALID_DATA);
            }

            if (image->monochrome) {
                red = avif_rgb_normalize_luma(
                    y_sample, image->bit_depth, info->color_range);
                green = red;
                blue = red;
            } else {
                uint32_t chroma_x = source_x >> image->subsampling_x;
                uint32_t chroma_y = source_y >> image->subsampling_y;
                size_t u_index =
                    (size_t)chroma_y * image->strides[1] + chroma_x;
                size_t v_index =
                    (size_t)chroma_y * image->strides[2] + chroma_x;
                uint16_t u_sample = image->planes[1][u_index];
                uint16_t v_sample = image->planes[2][v_index];

                if (u_sample > avif_rgb_max_sample(image->bit_depth) ||
                    v_sample > avif_rgb_max_sample(image->bit_depth)) {
                    return avif_rgb_fail(error, AVIFDEC_INVALID_DATA);
                }
                if (info->matrix_coefficients == 0U) {
                    red = avif_rgb_normalize_luma(
                        v_sample, image->bit_depth, info->color_range);
                    green = avif_rgb_normalize_luma(
                        y_sample, image->bit_depth, info->color_range);
                    blue = avif_rgb_normalize_luma(
                        u_sample, image->bit_depth, info->color_range);
                } else {
                    avif_rgb_convert_yuv(
                        y_sample, u_sample, v_sample,
                        image->bit_depth, info->color_range,
                        &matrix, &red, &green, &blue);
                }
            }

            if (info->has_alpha != 0U) {
                size_t alpha_index =
                    (size_t)source_y * image->alpha_stride + source_x;
                uint16_t alpha_sample = image->alpha_plane[alpha_index];

                if (alpha_sample >
                    avif_rgb_max_sample(image->alpha_bit_depth)) {
                    return avif_rgb_fail(error, AVIFDEC_INVALID_DATA);
                }
                alpha = avif_rgb_normalize_luma(
                    alpha_sample, image->alpha_bit_depth,
                    image->alpha_color_range);
                if (image->alpha_premultiplied != rgb->alpha_mode) {
                    if (rgb->alpha_mode == AVIFDEC_ALPHA_PREMULTIPLIED) {
                        red = avif_rgb_premultiply(red, alpha);
                        green = avif_rgb_premultiply(green, alpha);
                        blue = avif_rgb_premultiply(blue, alpha);
                    } else {
                        red = avif_rgb_unpremultiply(red, alpha);
                        green = avif_rgb_unpremultiply(green, alpha);
                        blue = avif_rgb_unpremultiply(blue, alpha);
                    }
                }
            }

            if (is_16_bit) {
                avif_rgb_store_u16(
                    pixel + 0U * sizeof(uint16_t), red);
                avif_rgb_store_u16(
                    pixel + 1U * sizeof(uint16_t), green);
                avif_rgb_store_u16(
                    pixel + 2U * sizeof(uint16_t), blue);
                if (has_output_alpha) {
                    avif_rgb_store_u16(
                        pixel + 3U * sizeof(uint16_t), alpha);
                }
            } else {
                pixel[0] = avif_rgb_to_u8(red);
                pixel[1] = avif_rgb_to_u8(green);
                pixel[2] = avif_rgb_to_u8(blue);
                if (has_output_alpha) {
                    pixel[3] = avif_rgb_to_u8(alpha);
                }
            }
        }
    }
    return AVIFDEC_OK;
}
