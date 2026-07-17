#include "av1_film_grain.h"

#include "av1_bitstream.h"
#include "base.h"

#include "av1_film_grain_gaussian.inc"

#define AV1_FG_GRAIN_WIDTH 82
#define AV1_FG_GRAIN_HEIGHT 73
#define AV1_FG_GRAIN_SAMPLES (AV1_FG_GRAIN_WIDTH * AV1_FG_GRAIN_HEIGHT)
#define AV1_FG_LUT_ENTRIES 256
#define AV1_FG_STRIPE_HEIGHT 34
#define AV1_FG_STRIPE_EXTRA 64U
#define AV1_FG_FRAME_TYPE_INTER 1U

static void av1_film_grain_reset(Av1FilmGrainParams *params) {
    avifdec_memory_fill(params, 0U, sizeof(*params));
}

static int64_t av1_film_grain_floor_shift(int64_t value, unsigned int bits) {
    uint64_t magnitude;
    uint64_t mask;

    if (bits == 0U) return value;
    if (value >= 0) return (int64_t)((uint64_t)value >> bits);
    magnitude = (uint64_t)(-value);
    mask = ((uint64_t)1U << bits) - 1U;
    return -(int64_t)((magnitude + mask) >> bits);
}

static int32_t av1_film_grain_round2(int64_t value, unsigned int shift) {
    if (shift == 0U) return (int32_t)value;
    return (int32_t)av1_film_grain_floor_shift(
        value + ((int64_t)1 << (shift - 1U)), shift);
}

static int32_t av1_film_grain_clip3(int32_t low, int32_t high, int32_t value) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static uint32_t av1_film_grain_random(uint16_t *reg, unsigned int bits) {
    uint32_t r = *reg;
    uint32_t bit = (r ^ (r >> 1U) ^ (r >> 3U) ^ (r >> 12U)) & 1U;

    r = (r >> 1U) | (bit << 15U);
    *reg = (uint16_t)r;
    return (r >> (16U - bits)) & (((uint32_t)1U << bits) - 1U);
}

AvifdecStatus av1_film_grain_parse(Av1Bits *bits,
                                   const Av1FilmGrainParseConfig *config,
                                   Av1FilmGrainParams *params) {
    unsigned int i;
    unsigned int num_pos_luma;
    unsigned int num_pos_chroma;

    if (bits == 0 || config == 0 || params == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    av1_film_grain_reset(params);
    if (!config->film_grain_params_present ||
        (!config->show_frame && !config->showable_frame)) {
        return AVIFDEC_OK;
    }
    params->apply_grain = (uint8_t)av1_bits_read(bits, 1U);
    if (!params->apply_grain) {
        av1_film_grain_reset(params);
        return bits->status;
    }
    params->grain_seed = (uint16_t)av1_bits_read(bits, 16U);
    if (config->frame_type == AV1_FG_FRAME_TYPE_INTER) {
        params->update_grain = (uint8_t)av1_bits_read(bits, 1U);
    } else {
        params->update_grain = 1U;
    }
    if (!params->update_grain) {
        uint8_t ref_idx = (uint8_t)av1_bits_read(bits, 3U);
        uint16_t saved_seed = params->grain_seed;
        const Av1FilmGrainParams *source;
        unsigned int j;
        int found = 0;

        for (j = 0U; j < AV1_FILM_GRAIN_REFS_PER_FRAME; ++j) {
            if (config->ref_frame_idx != 0 &&
                config->ref_frame_idx[j] == ref_idx) {
                found = 1;
                break;
            }
        }
        if (!found || config->reference_params == 0 ||
            config->reference_params[ref_idx] == 0) {
            return AVIFDEC_INVALID_DATA;
        }
        source = config->reference_params[ref_idx];
        if (!source->apply_grain) return AVIFDEC_INVALID_DATA;
        *params = *source;
        params->grain_seed = saved_seed;
        return bits->status;
    }
    params->num_y_points = (uint8_t)av1_bits_read(bits, 4U);
    if (params->num_y_points > AV1_FILM_GRAIN_MAX_Y_POINTS) {
        return AVIFDEC_INVALID_DATA;
    }
    for (i = 0U; i < params->num_y_points; ++i) {
        params->point_y_value[i] = (uint8_t)av1_bits_read(bits, 8U);
        if (i > 0U &&
            params->point_y_value[i] <= params->point_y_value[i - 1U]) {
            return AVIFDEC_INVALID_DATA;
        }
        params->point_y_scaling[i] = (uint8_t)av1_bits_read(bits, 8U);
    }
    if (!config->mono_chrome) {
        params->chroma_scaling_from_luma = (uint8_t)av1_bits_read(bits, 1U);
    }
    if (config->mono_chrome || params->chroma_scaling_from_luma ||
        (config->subsampling_x == 1U && config->subsampling_y == 1U &&
         params->num_y_points == 0U)) {
        params->num_cb_points = 0U;
        params->num_cr_points = 0U;
    } else {
        params->num_cb_points = (uint8_t)av1_bits_read(bits, 4U);
        if (params->num_cb_points > AV1_FILM_GRAIN_MAX_UV_POINTS) {
            return AVIFDEC_INVALID_DATA;
        }
        for (i = 0U; i < params->num_cb_points; ++i) {
            params->point_cb_value[i] = (uint8_t)av1_bits_read(bits, 8U);
            if (i > 0U &&
                params->point_cb_value[i] <=
                    params->point_cb_value[i - 1U]) {
                return AVIFDEC_INVALID_DATA;
            }
            params->point_cb_scaling[i] = (uint8_t)av1_bits_read(bits, 8U);
        }
        params->num_cr_points = (uint8_t)av1_bits_read(bits, 4U);
        if (params->num_cr_points > AV1_FILM_GRAIN_MAX_UV_POINTS) {
            return AVIFDEC_INVALID_DATA;
        }
        for (i = 0U; i < params->num_cr_points; ++i) {
            params->point_cr_value[i] = (uint8_t)av1_bits_read(bits, 8U);
            if (i > 0U &&
                params->point_cr_value[i] <=
                    params->point_cr_value[i - 1U]) {
                return AVIFDEC_INVALID_DATA;
            }
            params->point_cr_scaling[i] = (uint8_t)av1_bits_read(bits, 8U);
        }
        if (config->subsampling_x == 1U && config->subsampling_y == 1U &&
            ((params->num_cb_points == 0U) !=
             (params->num_cr_points == 0U))) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    params->grain_scaling_minus_8 = (uint8_t)av1_bits_read(bits, 2U);
    params->ar_coeff_lag = (uint8_t)av1_bits_read(bits, 2U);
    num_pos_luma =
        2U * params->ar_coeff_lag * (params->ar_coeff_lag + 1U);
    num_pos_chroma = num_pos_luma;
    if (params->num_y_points > 0U) {
        num_pos_chroma = num_pos_luma + 1U;
        for (i = 0U; i < num_pos_luma; ++i) {
            params->ar_coeffs_y_plus_128[i] =
                (uint8_t)av1_bits_read(bits, 8U);
        }
    }
    if (params->chroma_scaling_from_luma || params->num_cb_points > 0U) {
        for (i = 0U; i < num_pos_chroma; ++i) {
            params->ar_coeffs_cb_plus_128[i] =
                (uint8_t)av1_bits_read(bits, 8U);
        }
    }
    if (params->chroma_scaling_from_luma || params->num_cr_points > 0U) {
        for (i = 0U; i < num_pos_chroma; ++i) {
            params->ar_coeffs_cr_plus_128[i] =
                (uint8_t)av1_bits_read(bits, 8U);
        }
    }
    params->ar_coeff_shift_minus_6 = (uint8_t)av1_bits_read(bits, 2U);
    params->grain_scale_shift = (uint8_t)av1_bits_read(bits, 2U);
    if (params->num_cb_points > 0U) {
        params->cb_mult = (uint8_t)av1_bits_read(bits, 8U);
        params->cb_luma_mult = (uint8_t)av1_bits_read(bits, 8U);
        params->cb_offset = (uint16_t)av1_bits_read(bits, 9U);
    }
    if (params->num_cr_points > 0U) {
        params->cr_mult = (uint8_t)av1_bits_read(bits, 8U);
        params->cr_luma_mult = (uint8_t)av1_bits_read(bits, 8U);
        params->cr_offset = (uint16_t)av1_bits_read(bits, 9U);
    }
    params->overlap_flag = (uint8_t)av1_bits_read(bits, 1U);
    params->clip_to_restricted_range = (uint8_t)av1_bits_read(bits, 1U);
    return bits->status;
}

AvifdecStatus av1_film_grain_scratch_size(uint32_t width, size_t *size) {
    size_t stripe_stride;
    size_t stripe_samples;
    size_t total;

    if (size == 0) return AVIFDEC_INVALID_ARGUMENT;
    if (!avifdec_size_add((size_t)width, AV1_FG_STRIPE_EXTRA, &stripe_stride) ||
        !avifdec_size_multiply(stripe_stride, AV1_FG_STRIPE_HEIGHT,
                               &stripe_samples) ||
        !avifdec_size_multiply(stripe_samples, 2U, &stripe_samples)) {
        return AVIFDEC_OVERFLOW;
    }
    total = (size_t)AV1_FG_GRAIN_SAMPLES * 3U +
            (size_t)AV1_FG_LUT_ENTRIES * 3U;
    if (!avifdec_size_add(total, stripe_samples, &total) ||
        !avifdec_size_multiply(total, sizeof(int16_t), &total)) {
        return AVIFDEC_OVERFLOW;
    }
    *size = total;
    return AVIFDEC_OK;
}

AvifdecStatus av1_film_grain_scratch_size_ex(uint32_t width,
                                             size_t worker_count,
                                             size_t *size) {
    size_t base;
    size_t stripe_stride;
    size_t per_worker_samples;
    size_t per_worker_bytes;
    size_t extra;
    AvifdecStatus status;

    if (size == 0) return AVIFDEC_INVALID_ARGUMENT;
    status = av1_film_grain_scratch_size(width, &base);
    if (status != AVIFDEC_OK) return status;
    if (worker_count == 0U) return AVIFDEC_INVALID_ARGUMENT;
    if (worker_count == 1U) {
        *size = base;
        return AVIFDEC_OK;
    }
    if (worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    /*
     * Each additional worker beyond the first needs its own current+previous
     * stripe pair so concurrent stripe tasks never touch shared mutable
     * scratch; this is the same per-buffer size the serial path already
     * uses, just replicated once per worker instead of ping-ponged in place.
     */
    if (!avifdec_size_add((size_t)width, AV1_FG_STRIPE_EXTRA, &stripe_stride) ||
        !avifdec_size_multiply(stripe_stride, AV1_FG_STRIPE_HEIGHT,
                               &per_worker_samples) ||
        !avifdec_size_multiply(per_worker_samples, 2U, &per_worker_samples) ||
        !avifdec_size_multiply(per_worker_samples, sizeof(int16_t),
                               &per_worker_bytes) ||
        !avifdec_size_multiply(
            per_worker_bytes, worker_count - 1U, &extra) ||
        !avifdec_size_add(base, extra, size)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

typedef struct {
    const Av1FilmGrainParams *params;
    int16_t *luma_grain;
    int16_t *cb_grain;
    int16_t *cr_grain;
    int16_t *scaling_lut[3];
    int16_t *stripe_current;
    int16_t *stripe_previous;
    size_t stripe_stride;
    int grain_min;
    int grain_max;
    unsigned int bit_depth;
} Av1FilmGrainContext;

static void av1_film_grain_generate_luma(Av1FilmGrainContext *ctx) {
    const Av1FilmGrainParams *params = ctx->params;
    int shift = 12 - (int)ctx->bit_depth + (int)params->grain_scale_shift;
    uint16_t reg = params->grain_seed;
    int x;
    int y;

    for (y = 0; y < AV1_FG_GRAIN_HEIGHT; ++y) {
        for (x = 0; x < AV1_FG_GRAIN_WIDTH; ++x) {
            int g = 0;

            if (params->num_y_points > 0U) {
                g = av1_film_grain_gaussian[av1_film_grain_random(&reg, 11U)];
            }
            ctx->luma_grain[y * AV1_FG_GRAIN_WIDTH + x] =
                (int16_t)av1_film_grain_round2(g, (unsigned int)(shift < 0 ? 0 : shift));
        }
    }
    shift = (int)params->ar_coeff_shift_minus_6 + 6;
    for (y = 3; y < AV1_FG_GRAIN_HEIGHT; ++y) {
        for (x = 3; x < AV1_FG_GRAIN_WIDTH - 3; ++x) {
            int lag = (int)params->ar_coeff_lag;
            int delta_row;
            int pos = 0;
            int64_t s = 0;
            int done = 0;

            for (delta_row = -lag; delta_row <= 0 && !done; ++delta_row) {
                int delta_col;

                for (delta_col = -lag; delta_col <= lag; ++delta_col) {
                    int c;

                    if (delta_row == 0 && delta_col == 0) {
                        done = 1;
                        break;
                    }
                    c = (int)params->ar_coeffs_y_plus_128[pos] - 128;
                    s += (int64_t)ctx->luma_grain[(y + delta_row) *
                                                  AV1_FG_GRAIN_WIDTH +
                                                  (x + delta_col)] *
                         c;
                    ++pos;
                }
            }
            ctx->luma_grain[y * AV1_FG_GRAIN_WIDTH + x] = (int16_t)
                av1_film_grain_clip3(
                    ctx->grain_min, ctx->grain_max,
                    ctx->luma_grain[y * AV1_FG_GRAIN_WIDTH + x] +
                        av1_film_grain_round2(s, (unsigned int)shift));
        }
    }
}

static void av1_film_grain_generate_chroma(Av1FilmGrainContext *ctx,
                                           unsigned int subsampling_x,
                                           unsigned int subsampling_y) {
    const Av1FilmGrainParams *params = ctx->params;
    int chroma_w = subsampling_x ? 44 : 82;
    int chroma_h = subsampling_y ? 38 : 73;
    int shift = 12 - (int)ctx->bit_depth + (int)params->grain_scale_shift;
    unsigned int scale_shift = (unsigned int)(shift < 0 ? 0 : shift);
    uint16_t reg;
    int x;
    int y;

    reg = (uint16_t)(params->grain_seed ^ 0xb524U);
    for (y = 0; y < chroma_h; ++y) {
        for (x = 0; x < chroma_w; ++x) {
            int g = 0;

            if (params->num_cb_points > 0U ||
                params->chroma_scaling_from_luma) {
                g = av1_film_grain_gaussian[av1_film_grain_random(&reg, 11U)];
            }
            ctx->cb_grain[y * AV1_FG_GRAIN_WIDTH + x] =
                (int16_t)av1_film_grain_round2(g, scale_shift);
        }
    }
    reg = (uint16_t)(params->grain_seed ^ 0x49d8U);
    for (y = 0; y < chroma_h; ++y) {
        for (x = 0; x < chroma_w; ++x) {
            int g = 0;

            if (params->num_cr_points > 0U ||
                params->chroma_scaling_from_luma) {
                g = av1_film_grain_gaussian[av1_film_grain_random(&reg, 11U)];
            }
            ctx->cr_grain[y * AV1_FG_GRAIN_WIDTH + x] =
                (int16_t)av1_film_grain_round2(g, scale_shift);
        }
    }
    shift = (int)params->ar_coeff_shift_minus_6 + 6;
    for (y = 3; y < chroma_h; ++y) {
        for (x = 3; x < chroma_w - 3; ++x) {
            int lag = (int)params->ar_coeff_lag;
            int delta_row;
            int pos = 0;
            int64_t s0 = 0;
            int64_t s1 = 0;
            int done = 0;

            for (delta_row = -lag; delta_row <= 0 && !done; ++delta_row) {
                int delta_col;

                for (delta_col = -lag; delta_col <= lag; ++delta_col) {
                    int c0 = (int)params->ar_coeffs_cb_plus_128[pos] - 128;
                    int c1 = (int)params->ar_coeffs_cr_plus_128[pos] - 128;

                    if (delta_row == 0 && delta_col == 0) {
                        if (params->num_y_points > 0U) {
                            int luma = 0;
                            int luma_x =
                                ((x - 3) << subsampling_x) + 3;
                            int luma_y =
                                ((y - 3) << subsampling_y) + 3;
                            unsigned int i;
                            unsigned int j;

                            for (i = 0U; i <= subsampling_y; ++i) {
                                for (j = 0U; j <= subsampling_x; ++j) {
                                    luma += ctx->luma_grain[
                                        (luma_y + (int)i) *
                                            AV1_FG_GRAIN_WIDTH +
                                        (luma_x + (int)j)];
                                }
                            }
                            luma = av1_film_grain_round2(
                                luma, subsampling_x + subsampling_y);
                            s0 += (int64_t)luma * c0;
                            s1 += (int64_t)luma * c1;
                        }
                        done = 1;
                        break;
                    }
                    s0 += (int64_t)ctx->cb_grain[(y + delta_row) *
                                                 AV1_FG_GRAIN_WIDTH +
                                                 (x + delta_col)] *
                          c0;
                    s1 += (int64_t)ctx->cr_grain[(y + delta_row) *
                                                 AV1_FG_GRAIN_WIDTH +
                                                 (x + delta_col)] *
                          c1;
                    ++pos;
                }
            }
            ctx->cb_grain[y * AV1_FG_GRAIN_WIDTH + x] = (int16_t)
                av1_film_grain_clip3(
                    ctx->grain_min, ctx->grain_max,
                    ctx->cb_grain[y * AV1_FG_GRAIN_WIDTH + x] +
                        av1_film_grain_round2(s0, (unsigned int)shift));
            ctx->cr_grain[y * AV1_FG_GRAIN_WIDTH + x] = (int16_t)
                av1_film_grain_clip3(
                    ctx->grain_min, ctx->grain_max,
                    ctx->cr_grain[y * AV1_FG_GRAIN_WIDTH + x] +
                        av1_film_grain_round2(s1, (unsigned int)shift));
        }
    }
}

static int av1_film_grain_point_x(const Av1FilmGrainParams *params,
                                  int plane, int i) {
    if (plane == 0 || params->chroma_scaling_from_luma) {
        return params->point_y_value[i];
    }
    if (plane == 1) return params->point_cb_value[i];
    return params->point_cr_value[i];
}

static int av1_film_grain_point_y(const Av1FilmGrainParams *params,
                                  int plane, int i) {
    if (plane == 0 || params->chroma_scaling_from_luma) {
        return params->point_y_scaling[i];
    }
    if (plane == 1) return params->point_cb_scaling[i];
    return params->point_cr_scaling[i];
}

static void av1_film_grain_init_scaling(Av1FilmGrainContext *ctx,
                                        unsigned int plane_count) {
    const Av1FilmGrainParams *params = ctx->params;
    unsigned int plane;

    for (plane = 0U; plane < plane_count; ++plane) {
        int16_t *lut = ctx->scaling_lut[plane];
        int num_points;
        int x;

        if (plane == 0U || params->chroma_scaling_from_luma) {
            num_points = (int)params->num_y_points;
        } else if (plane == 1U) {
            num_points = (int)params->num_cb_points;
        } else {
            num_points = (int)params->num_cr_points;
        }
        if (num_points == 0) {
            for (x = 0; x < AV1_FG_LUT_ENTRIES; ++x) lut[x] = 0;
            continue;
        }
        for (x = 0; x < av1_film_grain_point_x(params, (int)plane, 0); ++x) {
            lut[x] = (int16_t)av1_film_grain_point_y(params, (int)plane, 0);
        }
        for (x = 0; x < num_points - 1; ++x) {
            int delta_y = av1_film_grain_point_y(params, (int)plane, x + 1) -
                          av1_film_grain_point_y(params, (int)plane, x);
            int delta_x = av1_film_grain_point_x(params, (int)plane, x + 1) -
                          av1_film_grain_point_x(params, (int)plane, x);
            int delta = delta_y * ((65536 + (delta_x >> 1)) / delta_x);
            int k;

            for (k = 0; k < delta_x; ++k) {
                int v = av1_film_grain_point_y(params, (int)plane, x) +
                        ((k * delta + 32768) >> 16);

                lut[av1_film_grain_point_x(params, (int)plane, x) + k] =
                    (int16_t)v;
            }
        }
        for (x = av1_film_grain_point_x(params, (int)plane, num_points - 1);
             x < AV1_FG_LUT_ENTRIES; ++x) {
            lut[x] = (int16_t)av1_film_grain_point_y(
                params, (int)plane, num_points - 1);
        }
    }
}

static int av1_film_grain_scale_lut(const Av1FilmGrainContext *ctx,
                                    int plane, int index) {
    const int16_t *lut = ctx->scaling_lut[plane];
    unsigned int shift = ctx->bit_depth - 8U;
    int x = index >> shift;
    int rem = index - (x << shift);

    if (ctx->bit_depth == 8U || x == 255) {
        return lut[x];
    }
    return lut[x] + av1_film_grain_round2(
                        (int64_t)(lut[x + 1] - lut[x]) * rem, shift);
}

/*
 * Per-plane geometry/derived parameters shared by stripe generation and
 * output writing. Depends only on ctx (read-only) and image (read-only), so
 * it may be computed redundantly and concurrently by any number of workers.
 */
typedef struct {
    unsigned int sub_x;
    unsigned int sub_y;
    int plane_w;
    int plane_h;
    int stripe_h;
    int stripe_rows;
    int half_w;
    int scaling_shift;
    const int16_t *grain;
    int luma_multiplier;
    int multiplier;
    int offset;
} Av1FilmGrainPlaneSetup;

static void av1_film_grain_plane_setup(const Av1FilmGrainContext *ctx,
                                       const Av1FilmGrainImage *image,
                                       int plane,
                                       Av1FilmGrainPlaneSetup *setup) {
    const Av1FilmGrainParams *params = ctx->params;
    int w = (int)image->width;
    int h = (int)image->height;

    setup->sub_x = plane > 0 ? image->subsampling_x : 0U;
    setup->sub_y = plane > 0 ? image->subsampling_y : 0U;
    setup->plane_w = (w + (int)setup->sub_x) >> setup->sub_x;
    setup->plane_h = (h + (int)setup->sub_y) >> setup->sub_y;
    setup->stripe_h = AV1_FG_STRIPE_HEIGHT >> setup->sub_y;
    setup->stripe_rows = 32 >> setup->sub_y;
    setup->half_w = (w + 1) / 2;
    setup->scaling_shift = (int)params->grain_scaling_minus_8 + 8;
    setup->grain = plane == 0 ? ctx->luma_grain
                   : (plane == 1 ? ctx->cb_grain : ctx->cr_grain);
    setup->luma_multiplier = 0;
    setup->multiplier = 0;
    setup->offset = 0;
    if (plane == 1) {
        setup->luma_multiplier = (int)params->cb_luma_mult - 128;
        setup->multiplier = (int)params->cb_mult - 128;
        setup->offset = ((int)params->cb_offset - 256) *
                        (1 << (ctx->bit_depth - 8U));
    } else if (plane == 2) {
        setup->luma_multiplier = (int)params->cr_luma_mult - 128;
        setup->multiplier = (int)params->cr_mult - 128;
        setup->offset = ((int)params->cr_offset - 256) *
                        (1 << (ctx->bit_depth - 8U));
    }
}

/*
 * Fills one stripe's raw noise buffer (cur). The result depends only on ctx
 * (immutable grain templates/params), the requested luma_num and the
 * plane's own sub_x/sub_y/half_w/stripe_h -- never on any other stripe's
 * buffer, and never on frame pixel data -- so this may be called
 * redundantly, out of order, and concurrently from any thread to
 * reconstruct any stripe, as long as the caller-provided cur buffer is not
 * shared with another concurrent caller.
 */
static void av1_film_grain_fill_stripe(const Av1FilmGrainContext *ctx,
                                       const int16_t *grain,
                                       unsigned int sub_x,
                                       unsigned int sub_y,
                                       int half_w,
                                       int stripe_h,
                                       int luma_num,
                                       int16_t *cur) {
    const Av1FilmGrainParams *params = ctx->params;
    uint16_t reg = params->grain_seed;
    int block_x;

    reg = (uint16_t)(reg ^ (uint16_t)(((luma_num * 37 + 178) & 255) << 8));
    reg = (uint16_t)(reg ^ (uint16_t)((luma_num * 173 + 105) & 255));
    for (block_x = 0; block_x < half_w; block_x += 16) {
        uint32_t rand = av1_film_grain_random(&reg, 8U);
        int offset_x = (int)(rand >> 4);
        int offset_y = (int)(rand & 15U);
        int plane_offset_x = sub_x ? 6 + offset_x : 9 + offset_x * 2;
        int plane_offset_y = sub_y ? 6 + offset_y : 9 + offset_y * 2;
        int i;

        for (i = 0; i < stripe_h; ++i) {
            int j;

            for (j = 0; j < (AV1_FG_STRIPE_HEIGHT >> sub_x); ++j) {
                int g = grain[(plane_offset_y + i) * AV1_FG_GRAIN_WIDTH +
                              (plane_offset_x + j)];

                if (sub_x == 0U) {
                    int col = block_x * 2 + j;

                    if (j < 2 && params->overlap_flag && block_x > 0) {
                        int old = cur[(size_t)i * ctx->stripe_stride + col];

                        g = j == 0 ? old * 27 + g * 17 : old * 17 + g * 27;
                        g = av1_film_grain_clip3(
                            ctx->grain_min, ctx->grain_max,
                            av1_film_grain_round2(g, 5U));
                    }
                    cur[(size_t)i * ctx->stripe_stride + col] = (int16_t)g;
                } else {
                    int col = block_x + j;

                    if (j == 0 && params->overlap_flag && block_x > 0) {
                        int old = cur[(size_t)i * ctx->stripe_stride + col];

                        g = old * 23 + g * 22;
                        g = av1_film_grain_clip3(
                            ctx->grain_min, ctx->grain_max,
                            av1_film_grain_round2(g, 5U));
                    }
                    cur[(size_t)i * ctx->stripe_stride + col] = (int16_t)g;
                }
            }
        }
    }
}

/*
 * Writes final pixel output for one stripe from its raw noise buffer (cur)
 * and, when needed for the top overlap rows, the preceding stripe's raw
 * noise buffer (prev). Reads/writes only rows
 * [luma_num*stripe_rows, luma_num*stripe_rows+stripe_rows) of plane, which
 * are disjoint across luma_num, and only reads image->plane[0] (never
 * writes it unless plane == 0), so stripes of the same plane, and Cb/Cr
 * stripes of different planes, may be written concurrently; concurrent
 * luma (plane 0) writes must not overlap in time with concurrent chroma
 * reads of plane 0.
 */
static void av1_film_grain_write_stripe(const Av1FilmGrainContext *ctx,
                                        const Av1FilmGrainImage *image,
                                        int plane,
                                        unsigned int sub_x,
                                        unsigned int sub_y,
                                        int plane_w,
                                        int plane_h,
                                        int stripe_rows,
                                        int luma_num,
                                        const int16_t *cur,
                                        const int16_t *prev,
                                        int min_value,
                                        int max_luma,
                                        int max_chroma,
                                        int scaling_shift,
                                        int luma_multiplier,
                                        int multiplier,
                                        int offset) {
    const Av1FilmGrainParams *params = ctx->params;
    int w = (int)image->width;
    uint16_t *out = image->plane[plane];
    size_t out_stride = image->stride[plane];
    const uint16_t *luma_out = image->plane[0];
    size_t luma_stride = image->stride[0];
    int row;

    for (row = 0; row < stripe_rows &&
                  luma_num * stripe_rows + row < plane_h;
         ++row) {
        int out_y = luma_num * stripe_rows + row;
        int x;

        for (x = 0; x < plane_w; ++x) {
            int g = cur[(size_t)row * ctx->stripe_stride + x];
            int noise;

            if (sub_y == 0U) {
                if (row < 2 && luma_num > 0 && params->overlap_flag) {
                    int old = prev[(size_t)(row + 32) *
                                   ctx->stripe_stride + x];

                    g = row == 0 ? old * 27 + g * 17 : old * 17 + g * 27;
                    g = av1_film_grain_clip3(
                        ctx->grain_min, ctx->grain_max,
                        av1_film_grain_round2(g, 5U));
                }
            } else {
                if (row < 1 && luma_num > 0 && params->overlap_flag) {
                    int old = prev[(size_t)(row + 16) *
                                   ctx->stripe_stride + x];

                    g = old * 23 + g * 22;
                    g = av1_film_grain_clip3(
                        ctx->grain_min, ctx->grain_max,
                        av1_film_grain_round2(g, 5U));
                }
            }
            if (plane == 0) {
                int orig = out[(size_t)out_y * out_stride + x];

                noise = av1_film_grain_round2(
                    (int64_t)av1_film_grain_scale_lut(ctx, 0, orig) * g,
                    (unsigned int)scaling_shift);
                if (params->num_y_points > 0U) {
                    out[(size_t)out_y * out_stride + x] = (uint16_t)
                        av1_film_grain_clip3(min_value, max_luma,
                                             orig + noise);
                }
            } else {
                int luma_x = x << sub_x;
                int luma_y = out_y << sub_y;
                int luma_next_x = luma_x + 1 < w ? luma_x + 1 : w - 1;
                int average_luma;
                int orig = out[(size_t)out_y * out_stride + x];
                int merged;

                if (sub_x) {
                    average_luma = av1_film_grain_round2(
                        (int)luma_out[(size_t)luma_y * luma_stride +
                                      luma_x] +
                            (int)luma_out[(size_t)luma_y * luma_stride +
                                          luma_next_x],
                        1U);
                } else {
                    average_luma =
                        luma_out[(size_t)luma_y * luma_stride + luma_x];
                }
                if (params->chroma_scaling_from_luma) {
                    merged = average_luma;
                } else {
                    int combined = average_luma * luma_multiplier +
                                   orig * multiplier;
                    int clip_max = (1 << ctx->bit_depth) - 1;

                    merged = av1_film_grain_clip3(
                        0, clip_max, (combined >> 6) + offset);
                }
                noise = av1_film_grain_round2(
                    (int64_t)av1_film_grain_scale_lut(ctx, plane, merged) *
                        g,
                    (unsigned int)scaling_shift);
                out[(size_t)out_y * out_stride + x] = (uint16_t)
                    av1_film_grain_clip3(min_value, max_chroma,
                                         orig + noise);
            }
        }
    }
}

static void av1_film_grain_add_noise_plane(Av1FilmGrainContext *ctx,
                                           const Av1FilmGrainImage *image,
                                           int plane,
                                           int min_value,
                                           int max_luma,
                                           int max_chroma) {
    Av1FilmGrainPlaneSetup setup;
    int16_t *cur = ctx->stripe_current;
    int16_t *prev = ctx->stripe_previous;
    int luma_num;

    av1_film_grain_plane_setup(ctx, image, plane, &setup);
    for (luma_num = 0; luma_num * setup.stripe_rows < setup.plane_h;
         ++luma_num) {
        av1_film_grain_fill_stripe(ctx, setup.grain, setup.sub_x, setup.sub_y,
                                   setup.half_w, setup.stripe_h, luma_num,
                                   cur);
        av1_film_grain_write_stripe(
            ctx, image, plane, setup.sub_x, setup.sub_y, setup.plane_w,
            setup.plane_h, setup.stripe_rows, luma_num, cur, prev, min_value,
            max_luma, max_chroma, setup.scaling_shift, setup.luma_multiplier,
            setup.multiplier, setup.offset);
        {
            int16_t *swap = prev;

            prev = cur;
            cur = swap;
        }
    }
}

/* Number of AV1_FG_STRIPE_HEIGHT-class stripes needed to cover plane_h. */
static size_t av1_film_grain_stripe_count(int plane_h, int stripe_rows) {
    return (size_t)((plane_h + stripe_rows - 1) / stripe_rows);
}

/*
 * Per-worker parallel context for the chroma (Cb/Cr) stripe application
 * phase. worker_scratch holds worker_count independent
 * (current, previous) stripe buffer pairs; a task only ever touches the
 * slice at its own worker_index, so distinct workers never share mutable
 * scratch. Work indices enumerate (plane slot, luma_num) pairs so Cb and Cr
 * stripes can be load-balanced together across workers.
 */
typedef struct {
    const Av1FilmGrainContext *ctx;
    const Av1FilmGrainImage *image;
    int min_value;
    int max_luma;
    int max_chroma;
    int16_t *worker_scratch;
    size_t per_worker_samples;
    size_t worker_count;
    uint8_t planes[2];
    uint8_t plane_count;
    size_t stripes_per_plane;
} Av1FilmGrainChromaParallel;

static AvifdecStatus av1_film_grain_chroma_task(size_t begin,
                                                size_t end,
                                                size_t worker_index,
                                                void *arg) {
    Av1FilmGrainChromaParallel *parallel = (Av1FilmGrainChromaParallel *)arg;
    size_t work_count;
    size_t index;
    int16_t *cur;
    int16_t *prev;

    if (parallel == 0 || begin > end ||
        worker_index >= parallel->worker_count ||
        parallel->stripes_per_plane == 0U ||
        !avifdec_size_multiply((size_t)parallel->plane_count,
                               parallel->stripes_per_plane, &work_count) ||
        end > work_count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    cur = parallel->worker_scratch +
          worker_index * parallel->per_worker_samples;
    prev = cur + (size_t)AV1_FG_STRIPE_HEIGHT * parallel->ctx->stripe_stride;
    for (index = begin; index < end; ++index) {
        unsigned int slot = (unsigned int)(index / parallel->stripes_per_plane);
        int luma_num = (int)(index % parallel->stripes_per_plane);
        int plane = parallel->planes[slot];
        Av1FilmGrainPlaneSetup setup;

        av1_film_grain_plane_setup(parallel->ctx, parallel->image, plane,
                                   &setup);
        av1_film_grain_fill_stripe(parallel->ctx, setup.grain, setup.sub_x,
                                   setup.sub_y, setup.half_w, setup.stripe_h,
                                   luma_num, cur);
        if (parallel->ctx->params->overlap_flag && luma_num > 0) {
            av1_film_grain_fill_stripe(parallel->ctx, setup.grain,
                                       setup.sub_x, setup.sub_y,
                                       setup.half_w, setup.stripe_h,
                                       luma_num - 1, prev);
        }
        av1_film_grain_write_stripe(
            parallel->ctx, parallel->image, plane, setup.sub_x, setup.sub_y,
            setup.plane_w, setup.plane_h, setup.stripe_rows, luma_num, cur,
            prev, parallel->min_value, parallel->max_luma,
            parallel->max_chroma, setup.scaling_shift,
            setup.luma_multiplier, setup.multiplier, setup.offset);
    }
    return AVIFDEC_OK;
}

/*
 * Per-worker parallel context for the luma (plane 0) stripe application
 * phase. Must only be dispatched after every chroma task has completed
 * (chroma tasks read plane 0 before any luma task may write it).
 */
typedef struct {
    const Av1FilmGrainContext *ctx;
    const Av1FilmGrainImage *image;
    int min_value;
    int max_luma;
    int max_chroma;
    int16_t *worker_scratch;
    size_t per_worker_samples;
    size_t worker_count;
} Av1FilmGrainLumaParallel;

static AvifdecStatus av1_film_grain_luma_task(size_t begin,
                                              size_t end,
                                              size_t worker_index,
                                              void *arg) {
    Av1FilmGrainLumaParallel *parallel = (Av1FilmGrainLumaParallel *)arg;
    Av1FilmGrainPlaneSetup setup;
    int16_t *cur;
    int16_t *prev;
    size_t stripe_count;
    size_t index;

    if (parallel == 0 || begin > end ||
        worker_index >= parallel->worker_count) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    av1_film_grain_plane_setup(parallel->ctx, parallel->image, 0, &setup);
    stripe_count =
        av1_film_grain_stripe_count(setup.plane_h, setup.stripe_rows);
    if (end > stripe_count) return AVIFDEC_INVALID_ARGUMENT;
    cur = parallel->worker_scratch +
          worker_index * parallel->per_worker_samples;
    prev = cur + (size_t)AV1_FG_STRIPE_HEIGHT * parallel->ctx->stripe_stride;
    for (index = begin; index < end; ++index) {
        int luma_num = (int)index;

        av1_film_grain_fill_stripe(parallel->ctx, setup.grain, setup.sub_x,
                                   setup.sub_y, setup.half_w, setup.stripe_h,
                                   luma_num, cur);
        if (parallel->ctx->params->overlap_flag && luma_num > 0) {
            av1_film_grain_fill_stripe(parallel->ctx, setup.grain,
                                       setup.sub_x, setup.sub_y,
                                       setup.half_w, setup.stripe_h,
                                       luma_num - 1, prev);
        }
        av1_film_grain_write_stripe(
            parallel->ctx, parallel->image, 0, setup.sub_x, setup.sub_y,
            setup.plane_w, setup.plane_h, setup.stripe_rows, luma_num, cur,
            prev, parallel->min_value, parallel->max_luma,
            parallel->max_chroma, setup.scaling_shift,
            setup.luma_multiplier, setup.multiplier, setup.offset);
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_film_grain_apply_ex(const Av1FilmGrainParams *params,
                                      const Av1FilmGrainImage *image,
                                      void *scratch,
                                      size_t scratch_size,
                                      const AvifdecExecutor *executor) {
    Av1FilmGrainContext ctx;
    int16_t *base;
    size_t offset = 0;
    size_t required;
    size_t stripe_stride;
    unsigned int plane_count;
    int grain_center;
    int min_value;
    int max_luma;
    int max_chroma;
    size_t worker_count;
    uint8_t chroma_planes[2];
    unsigned int chroma_plane_count = 0U;
    AvifdecStatus status;

    if (params == 0 || image == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!params->apply_grain) return AVIFDEC_OK;
    if (executor != 0 &&
        (executor->worker_count == 0U ||
         executor->worker_count > AVIFDEC_EXECUTOR_MAX_WORKERS ||
         executor->parallel_for == 0)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (scratch == 0 ||
        (image->bit_depth != 8U && image->bit_depth != 10U &&
         image->bit_depth != 12U) ||
        image->width == 0U || image->height == 0U ||
        image->width > 0x7fffffffU || image->height > 0x7fffffffU ||
        image->subsampling_x > 1U || image->subsampling_y > 1U ||
        image->plane[0] == 0 || image->stride[0] < image->width) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (!image->mono_chrome) {
        uint32_t chroma_width =
            (image->width + image->subsampling_x) >>
            image->subsampling_x;

        if (image->plane[1] == 0 || image->plane[2] == 0 ||
            image->stride[1] < chroma_width ||
            image->stride[2] < chroma_width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
    }
    worker_count = executor != 0 ? executor->worker_count : 1U;
    if (worker_count > 1U) {
        status = av1_film_grain_scratch_size_ex(
            image->width, worker_count, &required);
    } else {
        status = av1_film_grain_scratch_size(image->width, &required);
    }
    if (status != AVIFDEC_OK) return status;
    if (scratch_size < required) return AVIFDEC_INVALID_ARGUMENT;

    plane_count = image->mono_chrome ? 1U : 3U;
    base = (int16_t *)scratch;
    ctx.params = params;
    ctx.bit_depth = image->bit_depth;
    ctx.luma_grain = base + offset;
    offset += AV1_FG_GRAIN_SAMPLES;
    ctx.cb_grain = base + offset;
    offset += AV1_FG_GRAIN_SAMPLES;
    ctx.cr_grain = base + offset;
    offset += AV1_FG_GRAIN_SAMPLES;
    ctx.scaling_lut[0] = base + offset;
    offset += AV1_FG_LUT_ENTRIES;
    ctx.scaling_lut[1] = base + offset;
    offset += AV1_FG_LUT_ENTRIES;
    ctx.scaling_lut[2] = base + offset;
    offset += AV1_FG_LUT_ENTRIES;
    stripe_stride = (size_t)image->width + AV1_FG_STRIPE_EXTRA;
    ctx.stripe_stride = stripe_stride;
    ctx.stripe_current = base + offset;
    offset += (size_t)AV1_FG_STRIPE_HEIGHT * stripe_stride;
    ctx.stripe_previous = base + offset;
    offset += (size_t)AV1_FG_STRIPE_HEIGHT * stripe_stride;

    grain_center = 128 << (image->bit_depth - 8U);
    ctx.grain_min = -grain_center;
    ctx.grain_max = (256 << (image->bit_depth - 8U)) - 1 - grain_center;

    /*
     * Grain template/scaling-LUT generation is a short, inherently serial
     * autoregressive recurrence over a small (82x73) buffer; it is kept
     * single-threaded and its outputs are read-only for every worker below.
     */
    av1_film_grain_generate_luma(&ctx);
    if (plane_count > 1U) {
        av1_film_grain_generate_chroma(&ctx, image->subsampling_x,
                                       image->subsampling_y);
    }
    av1_film_grain_init_scaling(&ctx, plane_count);

    if (params->clip_to_restricted_range) {
        min_value = 16 << (image->bit_depth - 8U);
        max_luma = 235 << (image->bit_depth - 8U);
        max_chroma = image->matrix_is_identity
                         ? max_luma
                         : 240 << (image->bit_depth - 8U);
    } else {
        min_value = 0;
        max_luma = (256 << (image->bit_depth - 8U)) - 1;
        max_chroma = max_luma;
    }

    chroma_planes[0] = 0U;
    chroma_planes[1] = 0U;
    if (plane_count > 1U) {
        if ((params->num_cb_points > 0U ||
             params->chroma_scaling_from_luma) &&
            image->plane[1] != 0) {
            chroma_planes[chroma_plane_count++] = 1U;
        }
        if ((params->num_cr_points > 0U ||
             params->chroma_scaling_from_luma) &&
            image->plane[2] != 0) {
            chroma_planes[chroma_plane_count++] = 2U;
        }
    }

    if (worker_count <= 1U) {
        unsigned int i;

        for (i = 0U; i < chroma_plane_count; ++i) {
            av1_film_grain_add_noise_plane(
                &ctx, image, (int)chroma_planes[i], min_value, max_luma,
                max_chroma);
        }
        av1_film_grain_add_noise_plane(&ctx, image, 0, min_value, max_luma,
                                       max_chroma);
        return AVIFDEC_OK;
    }

    /*
     * Parallel path: chroma stripes (disjoint Cb/Cr output rows) are
     * distributed across workers first and fully complete (parallel_for is
     * synchronous) before luma stripes -- which write plane 0 -- are
     * dispatched, so no worker ever reads luma samples that a concurrent
     * worker is writing. Each worker uses only its own scratch slice.
     */
    {
        int16_t *worker_scratch = ctx.stripe_current;
        size_t per_worker_samples =
            2U * (size_t)AV1_FG_STRIPE_HEIGHT * stripe_stride;
        Av1FilmGrainPlaneSetup luma_setup;
        size_t luma_stripes;

        av1_film_grain_plane_setup(&ctx, image, 0, &luma_setup);
        luma_stripes = av1_film_grain_stripe_count(
            luma_setup.plane_h, luma_setup.stripe_rows);

        if (chroma_plane_count > 0U) {
            Av1FilmGrainChromaParallel chroma_parallel;
            Av1FilmGrainPlaneSetup chroma_setup;
            size_t work_count;

            av1_film_grain_plane_setup(&ctx, image, 1, &chroma_setup);
            chroma_parallel.ctx = &ctx;
            chroma_parallel.image = image;
            chroma_parallel.min_value = min_value;
            chroma_parallel.max_luma = max_luma;
            chroma_parallel.max_chroma = max_chroma;
            chroma_parallel.worker_scratch = worker_scratch;
            chroma_parallel.per_worker_samples = per_worker_samples;
            chroma_parallel.worker_count = worker_count;
            chroma_parallel.planes[0] = chroma_planes[0];
            chroma_parallel.planes[1] = chroma_planes[1];
            chroma_parallel.plane_count = (uint8_t)chroma_plane_count;
            chroma_parallel.stripes_per_plane = av1_film_grain_stripe_count(
                chroma_setup.plane_h, chroma_setup.stripe_rows);
            if (!avifdec_size_multiply((size_t)chroma_plane_count,
                                       chroma_parallel.stripes_per_plane,
                                       &work_count)) {
                return AVIFDEC_OVERFLOW;
            }
            if (work_count > 1U) {
                status = executor->parallel_for(
                    executor->user_data, work_count, 1U,
                    av1_film_grain_chroma_task, &chroma_parallel);
            } else if (work_count == 1U) {
                status = av1_film_grain_chroma_task(
                    0U, 1U, 0U, &chroma_parallel);
            } else {
                status = AVIFDEC_OK;
            }
            if (status != AVIFDEC_OK) return status;
        }

        {
            Av1FilmGrainLumaParallel luma_parallel;

            luma_parallel.ctx = &ctx;
            luma_parallel.image = image;
            luma_parallel.min_value = min_value;
            luma_parallel.max_luma = max_luma;
            luma_parallel.max_chroma = max_chroma;
            luma_parallel.worker_scratch = worker_scratch;
            luma_parallel.per_worker_samples = per_worker_samples;
            luma_parallel.worker_count = worker_count;
            if (luma_stripes > 1U) {
                status = executor->parallel_for(
                    executor->user_data, luma_stripes, 1U,
                    av1_film_grain_luma_task, &luma_parallel);
            } else if (luma_stripes == 1U) {
                status = av1_film_grain_luma_task(
                    0U, 1U, 0U, &luma_parallel);
            } else {
                status = AVIFDEC_OK;
            }
            if (status != AVIFDEC_OK) return status;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_film_grain_apply(const Av1FilmGrainParams *params,
                                   const Av1FilmGrainImage *image,
                                   void *scratch,
                                   size_t scratch_size) {
    return av1_film_grain_apply_ex(params, image, scratch, scratch_size, 0);
}
