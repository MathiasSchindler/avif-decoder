#include "av1_predict.h"

static const uint8_t av1_smooth_weights[5][64] = {
    { 255U, 149U, 85U, 64U },
    { 255U, 197U, 146U, 105U, 73U, 50U, 37U, 32U },
    { 255U, 225U, 196U, 170U, 145U, 123U, 102U, 84U,
      68U, 54U, 43U, 33U, 26U, 20U, 17U, 16U },
    { 255U, 240U, 225U, 210U, 196U, 182U, 169U, 157U,
      145U, 133U, 122U, 111U, 101U, 92U, 83U, 74U,
      66U, 59U, 52U, 45U, 39U, 34U, 29U, 25U,
      21U, 17U, 14U, 12U, 10U, 9U, 8U, 8U },
    { 255U, 248U, 240U, 233U, 225U, 218U, 210U, 203U,
      196U, 189U, 182U, 176U, 169U, 163U, 156U, 150U,
      144U, 138U, 133U, 127U, 121U, 116U, 111U, 106U,
      101U, 96U, 91U, 86U, 82U, 77U, 73U, 69U,
      65U, 61U, 57U, 54U, 50U, 47U, 44U, 41U,
      38U, 35U, 32U, 29U, 27U, 25U, 22U, 20U,
      18U, 16U, 15U, 13U, 12U, 10U, 9U, 8U,
      7U, 6U, 6U, 5U, 5U, 4U, 4U, 4U }
};

static const uint16_t av1_directional_derivative[90] = {
    0U, 0U, 0U, 1023U, 0U, 0U, 547U, 0U, 0U, 372U,
    0U, 0U, 0U, 0U, 273U, 0U, 0U, 215U, 0U, 0U,
    178U, 0U, 0U, 151U, 0U, 0U, 132U, 0U, 0U, 116U,
    0U, 0U, 102U, 0U, 0U, 0U, 90U, 0U, 0U, 80U,
    0U, 0U, 71U, 0U, 0U, 64U, 0U, 0U, 57U, 0U,
    0U, 51U, 0U, 0U, 45U, 0U, 0U, 0U, 40U, 0U,
    0U, 35U, 0U, 0U, 31U, 0U, 0U, 27U, 0U, 0U,
    23U, 0U, 0U, 19U, 0U, 0U, 15U, 0U, 0U, 0U,
    0U, 11U, 0U, 0U, 7U, 0U, 0U, 3U, 0U, 0U
};

static const int8_t av1_filter_intra_taps[5][8][7] = {
    {
        { -6, 10, 0, 0, 0, 12, 0 }, { -5, 2, 10, 0, 0, 9, 0 },
        { -3, 1, 1, 10, 0, 7, 0 }, { -3, 1, 1, 2, 10, 5, 0 },
        { -4, 6, 0, 0, 0, 2, 12 }, { -3, 2, 6, 0, 0, 2, 9 },
        { -3, 2, 2, 6, 0, 2, 7 }, { -3, 1, 2, 2, 6, 3, 5 }
    },
    {
        { -10, 16, 0, 0, 0, 10, 0 }, { -6, 0, 16, 0, 0, 6, 0 },
        { -4, 0, 0, 16, 0, 4, 0 }, { -2, 0, 0, 0, 16, 2, 0 },
        { -10, 16, 0, 0, 0, 0, 10 }, { -6, 0, 16, 0, 0, 0, 6 },
        { -4, 0, 0, 16, 0, 0, 4 }, { -2, 0, 0, 0, 16, 0, 2 }
    },
    {
        { -8, 8, 0, 0, 0, 16, 0 }, { -8, 0, 8, 0, 0, 16, 0 },
        { -8, 0, 0, 8, 0, 16, 0 }, { -8, 0, 0, 0, 8, 16, 0 },
        { -4, 4, 0, 0, 0, 0, 16 }, { -4, 0, 4, 0, 0, 0, 16 },
        { -4, 0, 0, 4, 0, 0, 16 }, { -4, 0, 0, 0, 4, 0, 16 }
    },
    {
        { -2, 8, 0, 0, 0, 10, 0 }, { -1, 3, 8, 0, 0, 6, 0 },
        { -1, 2, 3, 8, 0, 4, 0 }, { 0, 1, 2, 3, 8, 2, 0 },
        { -1, 4, 0, 0, 0, 3, 10 }, { -1, 3, 4, 0, 0, 4, 6 },
        { -1, 2, 3, 4, 0, 4, 4 }, { -1, 2, 2, 3, 4, 3, 3 }
    },
    {
        { -12, 14, 0, 0, 0, 14, 0 }, { -10, 0, 14, 0, 0, 12, 0 },
        { -9, 0, 0, 14, 0, 11, 0 }, { -8, 0, 0, 0, 14, 10, 0 },
        { -10, 12, 0, 0, 0, 0, 14 }, { -9, 1, 12, 0, 0, 0, 12 },
        { -8, 0, 0, 12, 0, 1, 11 }, { -7, 0, 0, 1, 12, 1, 9 }
    }
};

static const uint8_t av1_intra_edge_kernel[3][5] = {
    { 0U, 4U, 8U, 4U, 0U },
    { 0U, 5U, 6U, 5U, 0U },
    { 2U, 4U, 4U, 4U, 2U }
};

static int32_t av1_predict_round_signed(int32_t value, uint8_t bits);
static uint16_t av1_predict_clip(int32_t value, uint8_t bit_depth);

static int av1_predict_size_index(uint32_t size) {
    switch (size) {
        case 4U: return 0;
        case 8U: return 1;
        case 16U: return 2;
        case 32U: return 3;
        case 64U: return 4;
        default: return -1;
    }
}

static int av1_predict_valid_angle(uint16_t angle) {
    if (angle == 90U || angle == 180U) return 1;
    if (angle >= 36U && angle <= 54U) return (angle - 36U) % 3U == 0U;
    if (angle >= 58U && angle <= 76U) return (angle - 58U) % 3U == 0U;
    if (angle >= 81U && angle <= 99U) return (angle - 81U) % 3U == 0U;
    if (angle >= 104U && angle <= 122U) return (angle - 104U) % 3U == 0U;
    if (angle >= 126U && angle <= 144U) return (angle - 126U) % 3U == 0U;
    if (angle >= 148U && angle <= 166U) return (angle - 148U) % 3U == 0U;
    if (angle >= 171U && angle <= 189U) return (angle - 171U) % 3U == 0U;
    if (angle >= 194U && angle <= 212U) return (angle - 194U) % 3U == 0U;
    return 0;
}

AvifdecStatus av1_predict_prepare_references(const uint16_t *plane,
                                              size_t plane_stride,
                                              uint32_t plane_width,
                                              uint32_t plane_height,
                                              uint32_t reference_width,
                                              uint32_t reference_height,
                                              uint32_t x,
                                              uint32_t y,
                                              uint32_t width,
                                              uint32_t height,
                                              uint8_t bit_depth,
                                              uint8_t have_above,
                                              uint8_t have_left,
                                              uint8_t have_above_right,
                                              uint8_t have_below_left,
                                              Av1PreparedReferences *prepared) {
    uint16_t *above;
    uint16_t *left;
    uint32_t count;
    uint32_t index;
    uint16_t middle;

    if (plane == 0 || prepared == 0 || plane_stride < plane_width ||
        plane_width == 0U || plane_height == 0U ||
        reference_width == 0U || reference_width > plane_width ||
        reference_height == 0U || reference_height > plane_height ||
        x >= reference_width || y >= reference_height ||
        width > plane_width - x || height > plane_height - y ||
        av1_predict_size_index(width) < 0 || av1_predict_size_index(height) < 0 ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U) ||
        have_above > 1U || have_left > 1U || have_above_right > 1U ||
        have_below_left > 1U || (have_above && y == 0U) ||
        (have_left && x == 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    above = prepared->above_storage + 2U;
    left = prepared->left_storage + 2U;
    count = width + height;
    middle = (uint16_t)(1U << (bit_depth - 1U));
    for (index = 0U; index < count; ++index) {
        if (!have_above) {
            above[index] = have_left ? plane[(size_t)y * plane_stride + x - 1U]
                                     : (uint16_t)(middle - 1U);
        } else {
            uint32_t limit = x + (have_above_right ? 2U * width : width) - 1U;
            uint32_t sample_x;

            if (limit >= reference_width) limit = reference_width - 1U;
            sample_x = x + index;
            if (sample_x > limit) sample_x = limit;
            above[index] = plane[(size_t)(y - 1U) * plane_stride + sample_x];
        }
        if (!have_left) {
            left[index] = have_above ? plane[(size_t)(y - 1U) * plane_stride + x]
                                    : (uint16_t)(middle + 1U);
        } else {
            uint32_t limit = y + (have_below_left ? 2U * height : height) - 1U;
            uint32_t sample_y;

            if (limit >= reference_height) limit = reference_height - 1U;
            sample_y = y + index;
            if (sample_y > limit) sample_y = limit;
            left[index] = plane[(size_t)sample_y * plane_stride + x - 1U];
        }
    }
    if (have_above && have_left) {
        prepared->references.top_left =
            plane[(size_t)(y - 1U) * plane_stride + x - 1U];
    } else if (have_above) {
        prepared->references.top_left = plane[(size_t)(y - 1U) * plane_stride + x];
    } else if (have_left) {
        prepared->references.top_left = plane[(size_t)y * plane_stride + x - 1U];
    } else {
        prepared->references.top_left = middle;
    }
    above[-1] = prepared->references.top_left;
    left[-1] = prepared->references.top_left;
    prepared->references.above = above;
    prepared->references.left = left;
    prepared->references.have_above = have_above;
    prepared->references.have_left = have_left;
    return AVIFDEC_OK;
}

static AvifdecStatus av1_predict_validate(uint16_t *destination,
                                          size_t stride,
                                          uint32_t width,
                                          uint32_t height,
                                          uint8_t bit_depth,
                                          const Av1IntraReferences *references) {
    if (destination == 0 || references == 0 || stride < width ||
        av1_predict_size_index(width) < 0 ||
        av1_predict_size_index(height) < 0 ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if ((references->have_above && references->above == 0) ||
        (references->have_left && references->left == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    return AVIFDEC_OK;
}

static void av1_predict_fill(uint16_t *destination,
                             size_t stride,
                             uint32_t width,
                             uint32_t height,
                             uint16_t value) {
    uint32_t row;
    uint32_t column;

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            destination[(size_t)row * stride + column] = value;
        }
    }
}

AvifdecStatus av1_predict_dc(uint16_t *destination,
                             size_t stride,
                             uint32_t width,
                             uint32_t height,
                             uint8_t bit_depth,
                             const Av1IntraReferences *references) {
    uint32_t sum = 0U;
    uint32_t count = 0U;
    uint32_t index;
    AvifdecStatus status;

    status = av1_predict_validate(destination, stride, width, height,
                                  bit_depth, references);
    if (status != AVIFDEC_OK) return status;
    if (references->have_left) {
        for (index = 0U; index < height; ++index) sum += references->left[index];
        count += height;
    }
    if (references->have_above) {
        for (index = 0U; index < width; ++index) sum += references->above[index];
        count += width;
    }
    if (count == 0U) {
        av1_predict_fill(destination, stride, width, height,
                         (uint16_t)(1U << (bit_depth - 1U)));
    } else {
        av1_predict_fill(destination, stride, width, height,
                         (uint16_t)((sum + (count >> 1U)) / count));
    }
    return AVIFDEC_OK;
}

static uint16_t av1_predict_paeth(uint16_t left,
                                  uint16_t above,
                                  uint16_t top_left) {
    int base = (int)above + (int)left - (int)top_left;
    unsigned int left_distance = (unsigned int)(base >= (int)left ?
        base - (int)left : (int)left - base);
    unsigned int above_distance = (unsigned int)(base >= (int)above ?
        base - (int)above : (int)above - base);
    unsigned int corner_distance = (unsigned int)(base >= (int)top_left ?
        base - (int)top_left : (int)top_left - base);

    if (left_distance <= above_distance && left_distance <= corner_distance) {
        return left;
    }
    if (above_distance <= corner_distance) return above;
    return top_left;
}

AvifdecStatus av1_predict_nondirectional(uint16_t *destination,
                                         size_t stride,
                                         uint32_t width,
                                         uint32_t height,
                                         uint8_t bit_depth,
                                         Av1PredictMode mode,
                                         const Av1IntraReferences *references) {
    const uint8_t *weights_x;
    const uint8_t *weights_y;
    uint32_t row;
    uint32_t column;
    AvifdecStatus status;

    status = av1_predict_validate(destination, stride, width, height,
                                  bit_depth, references);
    if (status != AVIFDEC_OK) return status;
    if (mode > AV1_PREDICT_SMOOTH_HORIZONTAL) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    weights_x = av1_smooth_weights[av1_predict_size_index(width)];
    weights_y = av1_smooth_weights[av1_predict_size_index(height)];
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint32_t prediction;

            if (mode == AV1_PREDICT_VERTICAL) {
                prediction = references->above[column];
            } else if (mode == AV1_PREDICT_HORIZONTAL) {
                prediction = references->left[row];
            } else if (mode == AV1_PREDICT_PAETH) {
                prediction = av1_predict_paeth(references->left[row],
                    references->above[column], references->top_left);
            } else if (mode == AV1_PREDICT_SMOOTH_VERTICAL) {
                prediction = (weights_y[row] * references->above[column] +
                    (256U - weights_y[row]) * references->left[height - 1U] +
                    128U) >> 8U;
            } else if (mode == AV1_PREDICT_SMOOTH_HORIZONTAL) {
                prediction = (weights_x[column] * references->left[row] +
                    (256U - weights_x[column]) * references->above[width - 1U] +
                    128U) >> 8U;
            } else {
                prediction = (weights_y[row] * references->above[column] +
                    (256U - weights_y[row]) * references->left[height - 1U] +
                    weights_x[column] * references->left[row] +
                    (256U - weights_x[column]) * references->above[width - 1U] +
                    256U) >> 9U;
            }
            destination[(size_t)row * stride + column] = (uint16_t)prediction;
        }
    }
    return AVIFDEC_OK;
}

static uint16_t av1_predict_edge_sample(const uint16_t *edge,
                                         uint16_t top_left,
                                         int index,
                                         int upsampled) {
    return index < 0 && !upsampled ? top_left : edge[index];
}

static uint16_t av1_predict_interpolate(uint16_t first,
                                        uint16_t second,
                                        uint32_t shift) {
    return (uint16_t)(((32U - shift) * first + shift * second + 16U) >> 5U);
}

static AvifdecStatus av1_predict_directional_internal(
    uint16_t *destination,
    size_t stride,
    uint32_t width,
    uint32_t height,
    uint8_t bit_depth,
    uint16_t angle,
    const Av1IntraReferences *references,
    int upsample_above,
    int upsample_left) {
    uint32_t dx = 0U;
    uint32_t dy = 0U;
    uint32_t row;
    uint32_t column;
    AvifdecStatus status;

    status = av1_predict_validate(destination, stride, width, height,
                                  bit_depth, references);
    if (status != AVIFDEC_OK) return status;
    if (!av1_predict_valid_angle(angle)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (angle < 90U) dx = av1_directional_derivative[angle];
    else if (angle > 90U && angle < 180U) {
        dx = av1_directional_derivative[180U - angle];
        dy = av1_directional_derivative[angle - 90U];
    } else if (angle > 180U) dy = av1_directional_derivative[270U - angle];
    if (angle != 90U && angle != 180U && dx == 0U && dy == 0U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }

    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint16_t prediction;

            if (angle < 90U) {
                uint32_t index = (row + 1U) * dx;
                uint32_t base = (index >> (6U - (unsigned int)upsample_above)) +
                                (column << (unsigned int)upsample_above);
                uint32_t shift = ((index << (unsigned int)upsample_above) >> 1U) & 31U;
                uint32_t maximum = (width + height - 1U) <<
                                   (unsigned int)upsample_above;

                prediction = base < maximum ?
                    av1_predict_interpolate(references->above[base],
                                            references->above[base + 1U], shift) :
                    references->above[maximum];
            } else if (angle > 90U && angle < 180U) {
                int index = (int)(column << 6U) - (int)((row + 1U) * dx);
                int base = index >> (6 - upsample_above);

                if (base >= -(1 << upsample_above)) {
                    uint32_t shift = (((uint32_t)index << (unsigned int)upsample_above) >> 1U) & 31U;
                    prediction = av1_predict_interpolate(
                        av1_predict_edge_sample(references->above,
                            references->top_left, base, upsample_above),
                        av1_predict_edge_sample(references->above,
                            references->top_left, base + 1, upsample_above), shift);
                } else {
                    index = (int)(row << 6U) - (int)((column + 1U) * dy);
                    base = index >> (6 - upsample_left);
                    prediction = av1_predict_interpolate(
                        av1_predict_edge_sample(references->left,
                            references->top_left, base, upsample_left),
                        av1_predict_edge_sample(references->left,
                            references->top_left, base + 1, upsample_left),
                        (((uint32_t)index << (unsigned int)upsample_left) >> 1U) & 31U);
                }
            } else if (angle > 180U) {
                uint32_t index = (column + 1U) * dy;
                uint32_t base = (index >> (6U - (unsigned int)upsample_left)) +
                                (row << (unsigned int)upsample_left);
                prediction = av1_predict_interpolate(references->left[base],
                    references->left[base + 1U],
                    ((index << (unsigned int)upsample_left) >> 1U) & 31U);
            } else if (angle == 90U) {
                prediction = references->above[column];
            } else {
                prediction = references->left[row];
            }
            destination[(size_t)row * stride + column] = prediction;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_predict_directional(uint16_t *destination,
                                      size_t stride,
                                      uint32_t width,
                                      uint32_t height,
                                      uint8_t bit_depth,
                                      uint16_t angle,
                                      const Av1IntraReferences *references) {
    return av1_predict_directional_internal(destination, stride, width, height,
                                            bit_depth, angle, references, 0, 0);
}

unsigned int av1_predict_edge_filter_strength(uint32_t width,
                                               uint32_t height,
                                               uint8_t filter_type,
                                               int angle_delta) {
    unsigned int difference = (unsigned int)(angle_delta < 0
                              ? -angle_delta : angle_delta);
    uint32_t size = width + height;
    unsigned int strength = 0U;

    if (filter_type > 1U) return 0U;
    if (filter_type == 0U) {
        if (size <= 8U) {
            if (difference >= 56U) strength = 1U;
        } else if (size <= 16U) {
            if (difference >= 40U) strength = 1U;
        } else if (size <= 24U) {
            if (difference >= 8U) strength = 1U;
            if (difference >= 16U) strength = 2U;
            if (difference >= 32U) strength = 3U;
        } else if (size <= 32U) {
            strength = 1U;
            if (difference >= 4U) strength = 2U;
            if (difference >= 32U) strength = 3U;
        } else {
            strength = 3U;
        }
    } else if (size <= 8U) {
        if (difference >= 40U) strength = 1U;
        if (difference >= 64U) strength = 2U;
    } else if (size <= 16U) {
        if (difference >= 20U) strength = 1U;
        if (difference >= 48U) strength = 2U;
    } else if (size <= 24U) {
        if (difference >= 4U) strength = 3U;
    } else {
        strength = 3U;
    }
    return strength;
}

int av1_predict_edge_upsample_selected(uint32_t width,
                                       uint32_t height,
                                       uint8_t filter_type,
                                       int angle_delta) {
    unsigned int difference = (unsigned int)(angle_delta < 0
                              ? -angle_delta : angle_delta);

    if (filter_type > 1U || difference == 0U || difference >= 40U) return 0;
    return width + height <= (filter_type == 0U ? 16U : 8U);
}

static void av1_predict_filter_edge(uint16_t *edge,
                                    uint32_t size,
                                    unsigned int strength) {
    uint16_t source[129];
    uint32_t index;

    if (strength == 0U) return;
    for (index = 0U; index < size; ++index) source[index] = edge[(int)index - 1];
    for (index = 1U; index < size; ++index) {
        uint32_t sum = 0U;
        uint32_t tap;

        for (tap = 0U; tap < 5U; ++tap) {
            int sample = (int)index - 2 + (int)tap;
            if (sample < 0) sample = 0;
            if (sample >= (int)size) sample = (int)size - 1;
            sum += av1_intra_edge_kernel[strength - 1U][tap] * source[sample];
        }
        edge[index - 1U] = (uint16_t)((sum + 8U) >> 4U);
    }
}

static void av1_predict_upsample_edge(uint16_t *edge,
                                      uint32_t count,
                                      uint8_t bit_depth) {
    uint16_t duplicate[131];
    uint32_t index;

    duplicate[0] = edge[-1];
    for (index = 0U; index <= count; ++index) {
        duplicate[index + 1U] = edge[(int)index - 1];
    }
    duplicate[count + 2U] = edge[count - 1U];
    edge[-2] = duplicate[0];
    for (index = 0U; index < count; ++index) {
        int32_t sum = -(int32_t)duplicate[index] +
                      9 * (int32_t)duplicate[index + 1U] +
                      9 * (int32_t)duplicate[index + 2U] -
                      (int32_t)duplicate[index + 3U];
        int32_t value = sum < -8 ? -1 : (sum + 8) >> 4;

        edge[(int)(2U * index) - 1] = av1_predict_clip(value, bit_depth);
        edge[2U * index] = duplicate[index + 2U];
    }
}

AvifdecStatus av1_predict_directional_edges(uint16_t *destination,
                                            size_t stride,
                                            uint32_t width,
                                            uint32_t height,
                                            uint8_t bit_depth,
                                            uint16_t angle,
                                            uint8_t filter_type,
                                            Av1PreparedReferences *prepared) {
    Av1IntraReferences *references;
    int upsample_above = 0;
    int upsample_left = 0;

    if (prepared == 0 || filter_type > 1U || !av1_predict_valid_angle(angle)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    references = &prepared->references;
    if (angle != 90U && angle != 180U) {
        if (angle > 90U && angle < 180U && width + height >= 24U) {
            references->top_left = (uint16_t)((5U * references->left[0] +
                6U * references->top_left + 5U * references->above[0] + 8U) >> 4U);
            prepared->above_storage[1] = references->top_left;
            prepared->left_storage[1] = references->top_left;
        }
        if (references->have_above) {
            av1_predict_filter_edge(prepared->above_storage + 2U,
                width + (angle < 90U ? height : 0U) + 1U,
                av1_predict_edge_filter_strength(width, height, filter_type,
                                                 (int)angle - 90));
        }
        if (references->have_left) {
            av1_predict_filter_edge(prepared->left_storage + 2U,
                height + (angle > 180U ? width : 0U) + 1U,
                av1_predict_edge_filter_strength(width, height, filter_type,
                                                 (int)angle - 180));
        }
        upsample_above = av1_predict_edge_upsample_selected(
            width, height, filter_type, (int)angle - 90);
        upsample_left = av1_predict_edge_upsample_selected(
            width, height, filter_type, (int)angle - 180);
        if (upsample_above) {
            av1_predict_upsample_edge(prepared->above_storage + 2U,
                                     width + (angle < 90U ? height : 0U),
                                     bit_depth);
        }
        if (upsample_left) {
            av1_predict_upsample_edge(prepared->left_storage + 2U,
                                     height + (angle > 180U ? width : 0U),
                                     bit_depth);
        }
    }
    return av1_predict_directional_internal(destination, stride, width, height,
        bit_depth, angle, references, upsample_above, upsample_left);
}

AvifdecStatus av1_predict_filter_intra(uint16_t *destination,
                                       size_t stride,
                                       uint32_t width,
                                       uint32_t height,
                                       uint8_t bit_depth,
                                       uint8_t filter_mode,
                                       const Av1IntraReferences *references) {
    uint32_t block_row;
    uint32_t block_column;
    AvifdecStatus status = av1_predict_validate(
        destination, stride, width, height, bit_depth, references);

    if (status != AVIFDEC_OK) return status;
    if (filter_mode >= 5U || width > 32U || height > 32U ||
        references->above == 0 || references->left == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (block_row = 0U; block_row < height / 2U; ++block_row) {
        for (block_column = 0U; block_column < width / 4U; ++block_column) {
            uint16_t neighbors[7];
            uint32_t index;
            uint32_t row;
            uint32_t column;

            for (index = 0U; index < 7U; ++index) {
                if (index < 5U) {
                    if (block_row == 0U) {
                        int above_index = (int)(block_column * 4U + index) - 1;
                        neighbors[index] = above_index < 0
                            ? references->top_left : references->above[above_index];
                    } else if (block_column == 0U && index == 0U) {
                        neighbors[index] = references->left[block_row * 2U - 1U];
                    } else {
                        neighbors[index] = destination[(size_t)(block_row * 2U - 1U) *
                            stride + block_column * 4U + index - 1U];
                    }
                } else if (block_column == 0U) {
                    neighbors[index] = references->left[block_row * 2U + index - 5U];
                } else {
                    neighbors[index] = destination[(size_t)(block_row * 2U + index - 5U) *
                        stride + block_column * 4U - 1U];
                }
            }
            for (row = 0U; row < 2U; ++row) {
                for (column = 0U; column < 4U; ++column) {
                    int32_t sum = 0;
                    for (index = 0U; index < 7U; ++index) {
                        sum += av1_filter_intra_taps[filter_mode][row * 4U + column][index] *
                               (int32_t)neighbors[index];
                    }
                    destination[(size_t)(block_row * 2U + row) * stride +
                                block_column * 4U + column] =
                        av1_predict_clip(av1_predict_round_signed(sum, 4U), bit_depth);
                }
            }
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_predict_palette(uint16_t *destination,
                                  size_t stride,
                                  uint32_t width,
                                  uint32_t height,
                                  uint8_t bit_depth,
                                  const uint16_t *palette,
                                  uint8_t palette_size,
                                  const uint8_t *color_map,
                                  size_t map_stride) {
    uint32_t row;
    uint32_t column;
    uint16_t maximum;

    if (destination == 0 || palette == 0 || color_map == 0 || stride < width ||
        map_stride < width || av1_predict_size_index(width) < 0 ||
        av1_predict_size_index(height) < 0 || palette_size < 2U ||
        palette_size > 8U ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    maximum = (uint16_t)((1U << bit_depth) - 1U);
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint8_t index = color_map[(size_t)row * map_stride + column];
            if (index >= palette_size || palette[index] > maximum) {
                return AVIFDEC_INVALID_DATA;
            }
        }
    }
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            destination[(size_t)row * stride + column] =
                palette[color_map[(size_t)row * map_stride + column]];
        }
    }
    return AVIFDEC_OK;
}

static int32_t av1_predict_round_signed(int32_t value, uint8_t bits) {
    int32_t magnitude;

    if (bits == 0U) return value;
    magnitude = value < 0 ? -value : value;
    magnitude = (magnitude + (1 << (bits - 1U))) >> bits;
    return value < 0 ? -magnitude : magnitude;
}

static uint16_t av1_predict_clip(int32_t value, uint8_t bit_depth) {
    int32_t maximum = (1 << bit_depth) - 1;

    if (value < 0) return 0U;
    if (value > maximum) return (uint16_t)maximum;
    return (uint16_t)value;
}

static uint32_t av1_predict_cfl_luma(const uint16_t *luma,
                                     size_t stride,
                                     uint32_t luma_width,
                                     uint32_t luma_height,
                                     uint32_t luma_x,
                                     uint32_t luma_y,
                                     uint32_t row,
                                     uint32_t column,
                                     uint8_t subsampling_x,
                                     uint8_t subsampling_y) {
    uint32_t sum = 0U;
    uint32_t delta_y;
    uint32_t delta_x;
    uint32_t luma_row = luma_y + (row << subsampling_y);
    uint32_t luma_column = luma_x + (column << subsampling_x);
    uint32_t maximum_row = luma_height - (1U << subsampling_y);
    uint32_t maximum_column = luma_width - (1U << subsampling_x);

    if (luma_row > maximum_row) luma_row = maximum_row;
    if (luma_column > maximum_column) luma_column = maximum_column;

    for (delta_y = 0U; delta_y <= subsampling_y; ++delta_y) {
        for (delta_x = 0U; delta_x <= subsampling_x; ++delta_x) {
            sum += luma[(size_t)(luma_row + delta_y) * stride +
                        luma_column + delta_x];
        }
    }
    return sum << (3U - subsampling_x - subsampling_y);
}

AvifdecStatus av1_predict_cfl(uint16_t *destination,
                              size_t destination_stride,
                              const uint16_t *luma,
                              size_t luma_stride,
                              uint32_t luma_width,
                              uint32_t luma_height,
                              uint32_t luma_x,
                              uint32_t luma_y,
                              uint32_t width,
                              uint32_t height,
                              uint8_t subsampling_x,
                              uint8_t subsampling_y,
                              int8_t alpha,
                              uint8_t bit_depth) {
    uint32_t luma_sum = 0U;
    uint32_t sample_count;
    uint32_t luma_average;
    uint32_t row;
    uint32_t column;
    int width_index = av1_predict_size_index(width);
    int height_index = av1_predict_size_index(height);

    if (destination == 0 || luma == 0 || width_index < 0 || height_index < 0 ||
        destination_stride < width || subsampling_x > 1U || subsampling_y > 1U ||
        luma_width < (1U << subsampling_x) ||
        luma_height < (1U << subsampling_y) || luma_stride < luma_width ||
        luma_x >= luma_width || luma_y >= luma_height ||
        alpha < -16 || alpha > 16 ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            luma_sum += av1_predict_cfl_luma(
                luma, luma_stride, luma_width, luma_height, luma_x, luma_y,
                row, column, subsampling_x, subsampling_y);
        }
    }
    sample_count = width * height;
    luma_average = (luma_sum + (sample_count >> 1U)) / sample_count;
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint32_t luma_value = av1_predict_cfl_luma(
                luma, luma_stride, luma_width, luma_height, luma_x, luma_y,
                row, column, subsampling_x, subsampling_y);
            int32_t scaled = av1_predict_round_signed(
                (int32_t)alpha * ((int32_t)luma_value - (int32_t)luma_average), 6U);
            size_t offset = (size_t)row * destination_stride + column;

            destination[offset] = av1_predict_clip(
                (int32_t)destination[offset] + scaled, bit_depth);
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_predict_checksum(const uint16_t *samples,
                                   size_t stride,
                                   uint32_t width,
                                   uint32_t height,
                                   uint8_t plane,
                                   uint64_t *checksum) {
    uint64_t value = (uint64_t)1469598103934665603ULL;
    uint32_t row;
    uint32_t column;

    if (samples == 0 || checksum == 0 || stride < width || width == 0U ||
        height == 0U || plane > 2U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    value ^= plane;
    value *= (uint64_t)1099511628211ULL;
    for (row = 0U; row < height; ++row) {
        for (column = 0U; column < width; ++column) {
            uint16_t sample = samples[(size_t)row * stride + column];
            value ^= (uint8_t)sample;
            value *= (uint64_t)1099511628211ULL;
            value ^= (uint8_t)(sample >> 8U);
            value *= (uint64_t)1099511628211ULL;
        }
    }
    *checksum = value;
    return AVIFDEC_OK;
}