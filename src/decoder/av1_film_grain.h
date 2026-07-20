#ifndef AVIFDEC_AV1_FILM_GRAIN_H
#define AVIFDEC_AV1_FILM_GRAIN_H

#include "av1_bitstream.h"
#include "avifdec.h"

#define AV1_FILM_GRAIN_NUM_REF_FRAMES 8U
#define AV1_FILM_GRAIN_REFS_PER_FRAME 7U
#define AV1_FILM_GRAIN_MAX_Y_POINTS 14U
#define AV1_FILM_GRAIN_MAX_UV_POINTS 10U
#define AV1_FILM_GRAIN_MAX_LUMA_COEFFS 24U
#define AV1_FILM_GRAIN_MAX_CHROMA_COEFFS 25U

typedef struct {
    uint16_t grain_seed;
    uint16_t cb_offset;
    uint16_t cr_offset;
    uint8_t apply_grain;
    uint8_t update_grain;
    uint8_t chroma_scaling_from_luma;
    uint8_t overlap_flag;
    uint8_t clip_to_restricted_range;
    uint8_t num_y_points;
    uint8_t num_cb_points;
    uint8_t num_cr_points;
    uint8_t grain_scaling_minus_8;
    uint8_t ar_coeff_lag;
    uint8_t ar_coeff_shift_minus_6;
    uint8_t grain_scale_shift;
    uint8_t cb_mult;
    uint8_t cb_luma_mult;
    uint8_t cr_mult;
    uint8_t cr_luma_mult;
    uint8_t point_y_value[AV1_FILM_GRAIN_MAX_Y_POINTS];
    uint8_t point_y_scaling[AV1_FILM_GRAIN_MAX_Y_POINTS];
    uint8_t point_cb_value[AV1_FILM_GRAIN_MAX_UV_POINTS];
    uint8_t point_cb_scaling[AV1_FILM_GRAIN_MAX_UV_POINTS];
    uint8_t point_cr_value[AV1_FILM_GRAIN_MAX_UV_POINTS];
    uint8_t point_cr_scaling[AV1_FILM_GRAIN_MAX_UV_POINTS];
    uint8_t ar_coeffs_y_plus_128[AV1_FILM_GRAIN_MAX_LUMA_COEFFS];
    uint8_t ar_coeffs_cb_plus_128[AV1_FILM_GRAIN_MAX_CHROMA_COEFFS];
    uint8_t ar_coeffs_cr_plus_128[AV1_FILM_GRAIN_MAX_CHROMA_COEFFS];
} Av1FilmGrainParams;

typedef struct {
    uint8_t film_grain_params_present;
    uint8_t show_frame;
    uint8_t showable_frame;
    uint8_t frame_type;
    uint8_t mono_chrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    const uint8_t *ref_frame_idx;
    const Av1FilmGrainParams *const *reference_params;
} Av1FilmGrainParseConfig;

AvifdecStatus av1_film_grain_parse(Av1Bits *bits,
                                   const Av1FilmGrainParseConfig *config,
                                   Av1FilmGrainParams *params);

AvifdecStatus av1_film_grain_scratch_size(uint32_t width, size_t *size);

/*
 * Exact scratch size required by av1_film_grain_apply_ex() when it is given
 * an executor advertising worker_count concurrent workers. worker_count must
 * be in [1, AVIFDEC_EXECUTOR_MAX_WORKERS]. For worker_count == 1 this equals
 * av1_film_grain_scratch_size(width, size); each additional worker adds one
 * bounded, independent stripe scratch pair so concurrent stripe tasks never
 * share mutable scratch.
 */
AvifdecStatus av1_film_grain_scratch_size_ex(uint32_t width,
                                             size_t worker_count,
                                             size_t *size);

typedef struct {
    uint16_t *plane[3];
    size_t stride[3];
    uint32_t width;
    uint32_t height;
    uint8_t bit_depth;
    uint8_t mono_chrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t matrix_is_identity;
} Av1FilmGrainImage;

AvifdecStatus av1_film_grain_apply(const Av1FilmGrainParams *params,
                                   const Av1FilmGrainImage *image,
                                   void *scratch,
                                   size_t scratch_size);

/*
 * Executor-aware variant of av1_film_grain_apply(). When executor is 0 or
 * executor->worker_count <= 1, behavior and scratch requirements are
 * identical to av1_film_grain_apply() (sized by av1_film_grain_scratch_size).
 * When executor->worker_count > 1, scratch must instead be sized by
 * av1_film_grain_scratch_size_ex() with the same worker_count, and output is
 * produced by distributing disjoint per-plane stripe rows across workers:
 * chroma (Cb/Cr) stripes first, then a barrier, then luma stripes -- luma
 * samples are only read by chroma workers before any luma worker writes
 * them. Every worker_index passed to the executor's parallel_for callback
 * that satisfies worker_index < executor->worker_count is handled correctly
 * and only ever touches its own private scratch slice; failures are
 * reported deterministically via the returned AvifdecStatus regardless of
 * how work is chunked across workers.
 */
AvifdecStatus av1_film_grain_apply_ex(const Av1FilmGrainParams *params,
                                      const Av1FilmGrainImage *image,
                                      void *scratch,
                                      size_t scratch_size,
                                      const AvifdecExecutor *executor);

#endif
