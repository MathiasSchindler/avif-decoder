#include "av1_filter.h"
#include "av1_tile_internal.h"
#include "base.h"

static int av1_filter_abs(int value) {
    return value < 0 ? -value : value;
}

static int av1_filter_clip(int low, int high, int value) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int av1_filter_arshift(int value, unsigned int bits) {
    if (value >= 0) return value >> bits;
    return -(int)(((unsigned int)(-value) + ((1U << bits) - 1U)) >> bits);
}

static unsigned int av1_filter_tx_size_for_chroma(uint32_t width4,
                                                   uint32_t height4) {
    unsigned int index;
    unsigned int best = AV1_TX_SIZES_ALL;
    unsigned int best_area = 0U;

    if (width4 == 0U) width4 = 1U;
    if (height4 == 0U) height4 = 1U;
    for (index = 0U; index < AV1_TX_SIZES_ALL; ++index) {
        unsigned int width = av1_tx_size_info[index].width >> 2;
        unsigned int height = av1_tx_size_info[index].height >> 2;
        unsigned int area = width * height;

        if (width <= width4 && height <= height4 && width <= 8U && height <= 8U &&
            area > best_area) {
            best = index;
            best_area = area;
        }
    }
    return best;
}

static void av1_filter_wide(uint16_t *q0,
                            ptrdiff_t step,
                            unsigned int plane,
                            unsigned int log2_size) {
    uint16_t p[7];
    uint16_t q[7];
    unsigned int index;

    for (index = 0U; index < 7U; ++index) {
        p[index] = q0[-(ptrdiff_t)(index + 1U) * step];
        q[index] = q0[(ptrdiff_t)index * step];
    }
    if (plane != 0U) {
        q0[-2 * step] = (uint16_t)((3U * p[2] + 2U * p[1] +
                                    2U * p[0] + q[0] + 4U) >> 3U);
        q0[-step] = (uint16_t)((p[2] + 2U * p[1] + 2U * p[0] +
                               2U * q[0] + q[1] + 4U) >> 3U);
        q0[0] = (uint16_t)((p[1] + 2U * p[0] + 2U * q[0] +
                            2U * q[1] + q[2] + 4U) >> 3U);
        q0[step] = (uint16_t)((p[0] + 2U * q[0] + 2U * q[1] +
                               3U * q[2] + 4U) >> 3U);
    } else if (log2_size == 3U) {
        q0[-3 * step] = (uint16_t)((3U * p[3] + 2U * p[2] + p[1] +
                                    p[0] + q[0] + 4U) >> 3U);
        q0[-2 * step] = (uint16_t)((2U * p[3] + p[2] + 2U * p[1] +
                                    p[0] + q[0] + q[1] + 4U) >> 3U);
        q0[-step] = (uint16_t)((p[3] + p[2] + p[1] + 2U * p[0] +
                               q[0] + q[1] + q[2] + 4U) >> 3U);
        q0[0] = (uint16_t)((p[2] + p[1] + p[0] + 2U * q[0] +
                            q[1] + q[2] + q[3] + 4U) >> 3U);
        q0[step] = (uint16_t)((p[1] + p[0] + q[0] + 2U * q[1] +
                               q[2] + 2U * q[3] + 4U) >> 3U);
        q0[2 * step] = (uint16_t)((p[0] + q[0] + q[1] + 2U * q[2] +
                                   3U * q[3] + 4U) >> 3U);
    } else {
        q0[-6 * step] = (uint16_t)((7U * p[6] + 2U * p[5] + 2U * p[4] +
            p[3] + p[2] + p[1] + p[0] + q[0] + 8U) >> 4U);
        q0[-5 * step] = (uint16_t)((5U * p[6] + 2U * p[5] + 2U * p[4] +
            2U * p[3] + p[2] + p[1] + p[0] + q[0] + q[1] + 8U) >> 4U);
        q0[-4 * step] = (uint16_t)((4U * p[6] + p[5] + 2U * p[4] +
            2U * p[3] + 2U * p[2] + p[1] + p[0] + q[0] + q[1] +
            q[2] + 8U) >> 4U);
        q0[-3 * step] = (uint16_t)((3U * p[6] + p[5] + p[4] +
            2U * p[3] + 2U * p[2] + 2U * p[1] + p[0] + q[0] +
            q[1] + q[2] + q[3] + 8U) >> 4U);
        q0[-2 * step] = (uint16_t)((2U * p[6] + p[5] + p[4] + p[3] +
            2U * p[2] + 2U * p[1] + 2U * p[0] + q[0] + q[1] +
            q[2] + q[3] + q[4] + 8U) >> 4U);
        q0[-step] = (uint16_t)((p[6] + p[5] + p[4] + p[3] + p[2] +
            2U * p[1] + 2U * p[0] + 2U * q[0] + q[1] + q[2] +
            q[3] + q[4] + q[5] + 8U) >> 4U);
        q0[0] = (uint16_t)((p[5] + p[4] + p[3] + p[2] + p[1] +
            2U * p[0] + 2U * q[0] + 2U * q[1] + q[2] + q[3] +
            q[4] + q[5] + q[6] + 8U) >> 4U);
        q0[step] = (uint16_t)((p[4] + p[3] + p[2] + p[1] + p[0] +
            2U * q[0] + 2U * q[1] + 2U * q[2] + q[3] + q[4] +
            q[5] + 2U * q[6] + 8U) >> 4U);
        q0[2 * step] = (uint16_t)((p[3] + p[2] + p[1] + p[0] + q[0] +
            2U * q[1] + 2U * q[2] + 2U * q[3] + q[4] + q[5] +
            3U * q[6] + 8U) >> 4U);
        q0[3 * step] = (uint16_t)((p[2] + p[1] + p[0] + q[0] + q[1] +
            2U * q[2] + 2U * q[3] + 2U * q[4] + q[5] +
            4U * q[6] + 8U) >> 4U);
        q0[4 * step] = (uint16_t)((p[1] + p[0] + q[0] + q[1] + q[2] +
            2U * q[3] + 2U * q[4] + 2U * q[5] + 5U * q[6] +
            8U) >> 4U);
        q0[5 * step] = (uint16_t)((p[0] + q[0] + q[1] + q[2] + q[3] +
            2U * q[4] + 2U * q[5] + 7U * q[6] + 8U) >> 4U);
    }
}

AvifdecStatus av1_loop_filter_sample(uint16_t *q0,
                                     ptrdiff_t step,
                                     unsigned int plane,
                                     unsigned int filter_size,
                                     unsigned int limit,
                                     unsigned int blimit,
                                     unsigned int thresh,
                                     uint8_t bit_depth) {
    uint16_t p[7];
    uint16_t q[7];
    unsigned int filter_length;
    unsigned int shift;
    unsigned int limit_bd;
    unsigned int blimit_bd;
    unsigned int threshold_bd;
    unsigned int index;
    int hev;
    int mask = 0;
    int flat = 1;
    int flat2 = 1;

    if (q0 == 0 || step == 0 || plane > 2U ||
        (filter_size != 4U && filter_size != 8U && filter_size != 16U) ||
        (bit_depth != 8U && bit_depth != 10U && bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    for (index = 0U; index < 7U; ++index) {
        q[index] = q0[(ptrdiff_t)index * step];
        p[index] = q0[-(ptrdiff_t)(index + 1U) * step];
    }
    shift = bit_depth - 8U;
    limit_bd = limit << shift;
    blimit_bd = blimit << shift;
    threshold_bd = 1U << shift;
    hev = av1_filter_abs((int)p[1] - p[0]) > (int)(thresh << shift) ||
          av1_filter_abs((int)q[1] - q[0]) > (int)(thresh << shift);
    filter_length = filter_size == 4U ? 4U : (plane == 0U ? filter_size : 6U);
    mask |= av1_filter_abs((int)p[1] - p[0]) > (int)limit_bd;
    mask |= av1_filter_abs((int)q[1] - q[0]) > (int)limit_bd;
    mask |= 2 * av1_filter_abs((int)p[0] - q[0]) +
            av1_filter_abs((int)p[1] - q[1]) / 2 > (int)blimit_bd;
    if (filter_length >= 6U) {
        mask |= av1_filter_abs((int)p[2] - p[1]) > (int)limit_bd;
        mask |= av1_filter_abs((int)q[2] - q[1]) > (int)limit_bd;
    }
    if (filter_length >= 8U) {
        mask |= av1_filter_abs((int)p[3] - p[2]) > (int)limit_bd;
        mask |= av1_filter_abs((int)q[3] - q[2]) > (int)limit_bd;
    }
    if (mask) return AVIFDEC_OK;
    if (filter_size >= 8U) {
        flat &= av1_filter_abs((int)p[1] - p[0]) <= (int)threshold_bd;
        flat &= av1_filter_abs((int)q[1] - q[0]) <= (int)threshold_bd;
        flat &= av1_filter_abs((int)p[2] - p[0]) <= (int)threshold_bd;
        flat &= av1_filter_abs((int)q[2] - q[0]) <= (int)threshold_bd;
        if (filter_length >= 8U) {
            flat &= av1_filter_abs((int)p[3] - p[0]) <= (int)threshold_bd;
            flat &= av1_filter_abs((int)q[3] - q[0]) <= (int)threshold_bd;
        }
    }
    if (filter_size >= 16U) {
        for (index = 4U; index < 7U; ++index) {
            flat2 &= av1_filter_abs((int)p[index] - p[0]) <=
                     (int)threshold_bd;
            flat2 &= av1_filter_abs((int)q[index] - q[0]) <=
                     (int)threshold_bd;
        }
    }
    if (filter_size != 4U && flat) {
        av1_filter_wide(
            q0, step, plane, filter_size == 16U && flat2 ? 4U : 3U);
    } else {
        int offset = 0x80 << shift;
        int low = -(1 << (bit_depth - 1U));
        int high = (1 << (bit_depth - 1U)) - 1;
        int ps1 = (int)p[1] - offset;
        int ps0 = (int)p[0] - offset;
        int qs0 = (int)q[0] - offset;
        int qs1 = (int)q[1] - offset;
        int filter = hev ? av1_filter_clip(low, high, ps1 - qs1) : 0;
        int filter1;
        int filter2;

        filter = av1_filter_clip(low, high, filter + 3 * (qs0 - ps0));
        filter1 = av1_filter_arshift(av1_filter_clip(low, high, filter + 4), 3U);
        filter2 = av1_filter_arshift(av1_filter_clip(low, high, filter + 3), 3U);
        q0[0] = (uint16_t)(av1_filter_clip(low, high, qs0 - filter1) + offset);
        q0[-step] = (uint16_t)(av1_filter_clip(low, high, ps0 + filter2) + offset);
        if (!hev) {
            filter = av1_filter_arshift(filter1 + 1, 1U);
            q0[step] = (uint16_t)(av1_filter_clip(low, high, qs1 - filter) + offset);
            q0[-2 * step] =
                (uint16_t)(av1_filter_clip(low, high, ps1 + filter) + offset);
        }
    }
    return AVIFDEC_OK;
}

static const Av1BlockCell *av1_filter_cell(const Av1BlockState *blocks,
                                            uint32_t row,
                                            uint32_t column) {
    const Av1BlockCell *cell;

    if (row >= blocks->mi_rows || column >= blocks->mi_columns) {
        return 0;
    }
    cell = av1_block_cell(blocks, row, column);
    return cell != 0 && cell->width != 0U ? cell : 0;
}

static unsigned int av1_filter_level(const Av1LoopFilterParams *params,
                                      const Av1BlockCell *cell,
                                      unsigned int plane,
                                      unsigned int pass) {
    unsigned int level_index = plane == 0U ? pass : plane + 1U;
    int delta_lf = cell->delta_lf[params->delta_lf_multi ? level_index : 0U];
    int level = av1_filter_clip(0, 63, params->level[level_index] + delta_lf);
    unsigned int feature = 1U + level_index;

    if (params->segment_feature_enabled != 0 &&
        params->segment_feature_data != 0 &&
        params->segment_feature_enabled[cell->segment_id * 8U + feature]) {
        level = av1_filter_clip(0, 63,
            level + params->segment_feature_data[cell->segment_id * 8U + feature]);
    }
    if (params->delta_enabled) {
        level = av1_filter_clip(0, 63,
            level + params->ref_deltas[0] * (1 << (level >> 5)));
    }
    return (unsigned int)level;
}

static AvifdecStatus av1_filter_edge(Av1FramePlanes *planes,
                                     const Av1BlockState *blocks,
                                     const Av1LoopFilterParams *params,
                                     unsigned int plane,
                                     unsigned int pass,
                                     uint32_t row,
                                     uint32_t column) {
    unsigned int sub_x = plane == 0U ? 0U : params->subsampling_x;
    unsigned int sub_y = plane == 0U ? 0U : params->subsampling_y;
    unsigned int dx = pass == 0U ? 1U : 0U;
    unsigned int dy = pass == 0U ? 0U : 1U;
    uint32_t x = column * 4U;
    uint32_t y = row * 4U;
    uint32_t current_row = row | sub_y;
    uint32_t current_column = column | sub_x;
    uint32_t previous_row;
    uint32_t previous_column;
    uint32_t x_plane;
    uint32_t y_plane;
    const Av1BlockCell *cell;
    const Av1BlockCell *previous;
    unsigned int tx_size;
    unsigned int previous_tx_size;
    unsigned int block_width;
    unsigned int block_height;
    unsigned int tx_width;
    unsigned int tx_height;
    unsigned int filter_size;
    unsigned int level;
    unsigned int shift;
    unsigned int limit;
    unsigned int blimit;
    unsigned int thresh;
    unsigned int index;
    ptrdiff_t step;

    if (x >= params->frame_width || y >= params->frame_height ||
        (pass == 0U && x == 0U) || (pass == 1U && y == 0U)) {
        return AVIFDEC_OK;
    }
    previous_row = current_row - (dy << sub_y);
    previous_column = current_column - (dx << sub_x);
    cell = av1_filter_cell(blocks, current_row, current_column);
    previous = av1_filter_cell(blocks, previous_row, previous_column);
    if (cell == 0 || previous == 0) return AVIFDEC_INVALID_DATA;
    x_plane = x >> sub_x;
    y_plane = y >> sub_y;
    block_width = ((unsigned int)cell->width * 4U) >> sub_x;
    block_height = ((unsigned int)cell->height * 4U) >> sub_y;
    if (block_width < 4U) block_width = 4U;
    if (block_height < 4U) block_height = 4U;
    if (params->tx_sizes[plane] != 0) {
        size_t current_index = (size_t)current_row * params->mi_columns +
                               current_column;
        size_t previous_index = (size_t)previous_row * params->mi_columns +
                                previous_column;
        if (current_index >= params->tx_size_capacity ||
            previous_index >= params->tx_size_capacity) {
            return AVIFDEC_LIMIT_EXCEEDED;
        }
        tx_size = params->tx_sizes[plane][current_index];
        previous_tx_size = params->tx_sizes[plane][previous_index];
    } else {
        tx_size = plane == 0U ? cell->tx_size : av1_filter_tx_size_for_chroma(
            cell->width >> sub_x, cell->height >> sub_y);
        previous_tx_size = plane == 0U ? previous->tx_size
            : av1_filter_tx_size_for_chroma(
                previous->width >> sub_x, previous->height >> sub_y);
    }
    if (tx_size >= AV1_TX_SIZES_ALL || previous_tx_size >= AV1_TX_SIZES_ALL) {
        return AVIFDEC_INVALID_DATA;
    }
    tx_width = av1_tx_size_info[tx_size].width;
    tx_height = av1_tx_size_info[tx_size].height;
    if ((pass == 0U && x_plane % tx_width != 0U) ||
        (pass == 1U && y_plane % tx_height != 0U)) {
        return AVIFDEC_OK;
    }
            if (!((pass == 0U && x_plane % block_width == 0U) ||
                (pass == 1U && y_plane % block_height == 0U) ||
          !cell->skip || !cell->is_inter)) {
        return AVIFDEC_OK;
    }
    filter_size = pass == 0U
        ? av1_tx_size_info[tx_size].width
        : av1_tx_size_info[tx_size].height;
    {
        unsigned int previous_size = pass == 0U
            ? av1_tx_size_info[previous_tx_size].width
            : av1_tx_size_info[previous_tx_size].height;
        if (previous_size < filter_size) filter_size = previous_size;
    }
    if (filter_size > (plane == 0U ? 16U : 8U)) {
        filter_size = plane == 0U ? 16U : 8U;
    }
    if (filter_size < 8U) filter_size = 4U;
    level = av1_filter_level(params, cell, plane, pass);
    if (level == 0U) level = av1_filter_level(params, previous, plane, pass);
    if (level == 0U) return AVIFDEC_OK;
    shift = params->sharpness > 4U ? 2U : (params->sharpness > 0U ? 1U : 0U);
    limit = level >> shift;
    if (params->sharpness > 0U && limit > 9U - params->sharpness) {
        limit = 9U - params->sharpness;
    }
    if (limit < 1U) limit = 1U;
    blimit = 2U * (level + 2U) + limit;
    thresh = level >> 4;
    step = pass == 0U ? 1 : (ptrdiff_t)planes->stride[plane];
    for (index = 0U; index < 4U; ++index) {
        uint32_t sample_x = x_plane + dy * index;
        uint32_t sample_y = y_plane + dx * index;
        uint16_t *sample;

        if (sample_x >= planes->width[plane] ||
            sample_y >= planes->height[plane]) continue;
        sample = planes->data[plane] +
                 (size_t)sample_y * planes->stride[plane] + sample_x;
        if (av1_loop_filter_sample(sample, step, plane, filter_size,
                                   limit, blimit, thresh,
                                   params->bit_depth) != AVIFDEC_OK) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_loop_filter_frame(Av1FramePlanes *planes,
                                    const Av1BlockState *blocks,
                                    const Av1LoopFilterParams *params) {
    unsigned int plane_count;
    unsigned int plane;

    if (planes == 0 || blocks == 0 || params == 0 ||
        blocks->mi_rows != params->mi_rows ||
        blocks->mi_columns != params->mi_columns ||
        params->monochrome > 1U || params->subsampling_x > 1U ||
        params->subsampling_y > 1U ||
        (params->bit_depth != 8U && params->bit_depth != 10U &&
         params->bit_depth != 12U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    plane_count = params->monochrome ? 1U : 3U;
    for (plane = 0U; plane < plane_count; ++plane) {
        unsigned int sub_x = plane == 0U ? 0U : params->subsampling_x;
        unsigned int sub_y = plane == 0U ? 0U : params->subsampling_y;
        uint32_t unit_row;

        if (planes->data[plane] == 0 || planes->stride[plane] == 0U) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        if ((plane == 0U && params->level[0] == 0U &&
             params->level[1] == 0U) ||
            (plane != 0U && params->level[plane + 1U] == 0U)) {
            continue;
        }
        for (unit_row = 0U; unit_row < params->mi_rows; unit_row += 32U) {
            uint32_t unit_row_end = unit_row + 32U;
            unsigned int pass;
            if (unit_row_end > params->mi_rows) unit_row_end = params->mi_rows;
            for (pass = 0U; pass < 2U; ++pass) {
                uint32_t row;
                for (row = unit_row; row < unit_row_end;
                     row += 1U << sub_y) {
                    uint32_t column;
                    for (column = 0U; column < params->mi_columns;
                         column += 1U << sub_x) {
                        AvifdecStatus status = av1_filter_edge(
                            planes, blocks, params, plane, pass, row, column);
                        if (status != AVIFDEC_OK) return status;
                    }
                }
            }
        }
    }
    return AVIFDEC_OK;
}
