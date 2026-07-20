#include "base.h"
#include "av1.h"
#include "av1_coeff.h"
#include "av1_dsp.h"
#include "av1_film_grain.h"
#include "av1_filter.h"
#include "av1_inter.h"
#include "av1_inter_predict.h"
#include "av1_intra.h"
#include "av1_metadata.h"
#include "av1_profile.h"
#include "av1_partition.h"
#include "av1_predict.h"
#include "av1_recon.h"
#include "av1_symbol.h"
#include "av1_tile.h"
#include "av1_tile_internal.h"
#include "av1_warp.h"
#include "avif_sato.h"
#include "bmff.h"
#include "png.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int test_checked_arithmetic(void) {
    size_t result;
    uint64_t capabilities = avifdec_capabilities();
    AvifdecExecutor executor;
    AvifdecImageInfo info;
    AvifdecError error;

    CHECK((capabilities & AVIFDEC_CAP_AV1_ANNEX_B) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AV1_METADATA) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AV1_FILM_GRAIN) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_PRESENTATION) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_ALPHA) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_GRID) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_LAYERED) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_SAMPLE_TRANSFORM) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_RGB_CONVERSION) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AVIF_SEQUENCE) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_PARALLEL_EXECUTOR) != 0U);
    CHECK((capabilities & AVIFDEC_CAP_AV1_TILE_LIST) == 0U);
    CHECK((capabilities & AVIFDEC_CAP_AV1_LARGE_SCALE_TILE) == 0U);
    CHECK(avifdec_memory_compare(
        avifdec_version_string(), "1.3.0", 6U) == 0);
    CHECK(avifdec_size_add(3U, 4U, &result) && result == 7U);
    CHECK(!avifdec_size_add(SIZE_MAX, 1U, &result));
    CHECK(avifdec_size_multiply(7U, 9U, &result) && result == 63U);
    CHECK(!avifdec_size_multiply(SIZE_MAX, 2U, &result));
    CHECK(avifdec_size_align(17U, 16U, &result) && result == 32U);
    CHECK(!avifdec_size_align(17U, 3U, &result));
    avifdec_memory_fill(&executor, 0U, sizeof(executor));
    CHECK(avifdec_query_ex(
        0, 0U, 0, &executor, 0, 0U,
        &info, &error) == AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_memory_and_endian(void) {
    unsigned char source[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    unsigned char destination[8];

    avifdec_memory_fill(destination, 0U, sizeof(destination));
    avifdec_memory_copy(destination, source, sizeof(source));
    CHECK(avifdec_memory_compare(destination, source, sizeof(source)) == 0);
    destination[7] = 9U;
    CHECK(avifdec_memory_compare(destination, source, sizeof(source)) > 0);
    CHECK(avifdec_load_u16be(source) == 0x0102U);
    CHECK(avifdec_load_u32be(source) == 0x01020304U);
    CHECK(avifdec_load_u64be(source) == 0x0102030405060708ULL);
    return 0;
}

static int test_av1_reference_state(void) {
    Av1ReferenceSlot slots[8];
    Av1ReferenceSlot shown;
    uint8_t refs[7];
    unsigned int index;

    CHECK(av1_relative_distance(0U, 0U, 1U, 15U) == 0);
    CHECK(av1_relative_distance(1U, 4U, 1U, 15U) == 2);
    CHECK(av1_relative_distance(1U, 4U, 15U, 1U) == -2);
    avifdec_memory_fill(slots, 0U, sizeof(slots));
    for (index = 0U; index < 8U; ++index) {
        slots[index].valid = 1U;
        slots[index].frame_id = index;
    }
    CHECK(av1_mark_reference_frames(slots, 4U, 3U, 2U) == AVIFDEC_OK);
        CHECK(slots[2].valid == 1U && slots[3].valid == 0U &&
            slots[7].valid == 0U);
    slots[0].order_hint = 14U;
    slots[1].order_hint = 15U;
    slots[2].order_hint = 1U;
    slots[3].order_hint = 3U;
    slots[4].order_hint = 4U;
    slots[5].order_hint = 12U;
    slots[6].order_hint = 7U;
    slots[7].order_hint = 8U;
    CHECK(av1_set_frame_refs(slots, 4U, 2U, 2U, 1U, refs) == AVIFDEC_OK);
    CHECK(refs[0] == 2U);
    CHECK(refs[3] == 1U);
    CHECK(refs[4] == 3U && refs[5] == 4U && refs[6] == 7U);
    CHECK(refs[1] == 0U && refs[2] == 5U);
    CHECK(av1_set_frame_refs(slots, 4U, 2U, 3U, 1U, refs) ==
            AVIFDEC_INVALID_DATA);
    slots[4].valid = 1U;
    slots[4].showable_frame = 1U;
    slots[4].frame_id = 9U;
    slots[4].order_hint = 15U;
    slots[4].upscaled_width = 64U;
    slots[4].frame_height = 32U;
    CHECK(av1_reference_show_existing(
        slots, 4U, 1, 9U, &shown) == AVIFDEC_OK);
    CHECK(shown.order_hint == 15U && shown.upscaled_width == 64U &&
          shown.frame_height == 32U);
    CHECK(av1_reference_show_existing(
        slots, 4U, 1, 8U, &shown) == AVIFDEC_INVALID_DATA);
    slots[4].showable_frame = 0U;
    CHECK(av1_reference_show_existing(
        slots, 4U, 0, 0U, &shown) == AVIFDEC_INVALID_DATA);
    CHECK(av1_reference_show_existing(
        slots, 8U, 0, 0U, &shown) == AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_av1_motion_vectors(void) {
    Av1MotionVector mv;
    Av1MotionVector projected;
    Av1MvStack stack;
    int32_t global_params[6] = { 65536, -131072, 65536, 0, 0, 65536 };
    unsigned int index;

    mv.row = 96;
    mv.column = -64;
    CHECK(av1_mv_project(mv, -2, 4U, &projected) == AVIFDEC_OK);
    CHECK(projected.row == -48 && projected.column == 32);
    CHECK(av1_mv_project(mv, 1, 0U, &projected) ==
          AVIFDEC_INVALID_ARGUMENT);
        CHECK(av1_mv_global(1U, global_params, 0U, 0U, 8U, 8U,
                    0, 1, &projected) == AVIFDEC_OK);
        CHECK(projected.row == 8 && projected.column == -16);
        global_params[0] = 0;
        global_params[1] = 0;
        CHECK(av1_mv_global(2U, global_params, 5U, 7U, 16U, 8U,
                    0, 1, &projected) == AVIFDEC_OK);
        CHECK(projected.row == 0 && projected.column == 0);
    mv.row = 13;
    mv.column = -13;
    av1_mv_lower_precision(&mv, 0, 0);
    CHECK(mv.row == 12 && mv.column == -12);
    mv.row = 13;
    mv.column = -13;
    av1_mv_lower_precision(&mv, 1, 0);
    CHECK(mv.row == 16 && mv.column == -16);
    mv.row = -1000;
    mv.column = 1000;
    av1_mv_clamp(&mv, 1U, 2U, 2U, 3U, 10U, 12U, 128U);
    CHECK(mv.row == -256 && mv.column == 448);
    avifdec_memory_fill(&stack, 0U, sizeof(stack));
    mv.row = 8;
    mv.column = 16;
    projected.row = -8;
    projected.column = 24;
    CHECK(av1_mv_stack_add(&stack, mv, projected, 1, 4U) == AVIFDEC_OK);
    CHECK(av1_mv_stack_add(&stack, projected, mv, 1, 9U) == AVIFDEC_OK);
    CHECK(av1_mv_stack_add(&stack, mv, projected, 1, 7U) == AVIFDEC_OK);
    CHECK(stack.count == 2U && stack.candidates[0].weight == 11U);
    av1_mv_stack_sort(&stack, 0U);
    CHECK(stack.candidates[0].weight == 11U &&
          stack.candidates[0].mv[0].row == 8);
    avifdec_memory_fill(&stack, 0U, sizeof(stack));
    for (index = 0U; index < AV1_MAX_MV_STACK_SIZE; ++index) {
        mv.row = (int32_t)index;
        mv.column = 0;
        CHECK(av1_mv_stack_add(&stack, mv, projected, 0, 1U) == AVIFDEC_OK);
    }
    mv.row = (int32_t)AV1_MAX_MV_STACK_SIZE;
    CHECK(av1_mv_stack_add(&stack, mv, projected, 0, 1U) == AVIFDEC_OK);
    CHECK(stack.count == AV1_MAX_MV_STACK_SIZE);
    return 0;
}

static int test_av1_inter_prediction(void) {
    uint16_t source[16U * 16U];
    uint16_t destination[8U * 8U];
    uint16_t pred0[8U * 8U];
    uint16_t pred1[8U * 8U];
    uint16_t intra[8U * 8U];
    uint8_t mask[8U * 8U];
    uint8_t inverse_mask[8U * 8U];
    Av1InterPredictParams prediction;
    Av1CompoundParams compound;
    unsigned int depth_index;
    unsigned int filter;
    unsigned int phase;
    unsigned int index;

    avifdec_memory_fill(&prediction, 0U, sizeof(prediction));
    prediction.src = source;
    prediction.src_stride = 16U;
    prediction.dst = destination;
    prediction.dst_stride = 8U;
    prediction.src_width = 16U;
    prediction.src_height = 16U;
    prediction.frame_width = 16U;
    prediction.frame_height = 16U;
    prediction.block_x = 4U;
    prediction.block_y = 4U;
    prediction.block_width = 8U;
    prediction.block_height = 8U;
    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        const uint8_t bit_depth = (uint8_t)(8U + 2U * depth_index);
        const uint16_t value = (uint16_t)(173U << (bit_depth - 8U));
        prediction.bit_depth = bit_depth;
        for (index = 0U; index < 16U * 16U; ++index) source[index] = value;
        for (filter = 0U; filter < 4U; ++filter) {
            prediction.filter_x = (uint8_t)filter;
            prediction.filter_y = (uint8_t)filter;
            prediction.subsampling_x = 1U;
            prediction.subsampling_y = 1U;
            for (phase = 0U; phase < 16U; ++phase) {
                prediction.mv_col = (int32_t)phase;
                prediction.mv_row = (int32_t)(15U - phase);
                CHECK(av1_inter_predict_single(&prediction) == AVIFDEC_OK);
                for (index = 0U; index < 8U * 8U; ++index) {
                    CHECK(destination[index] == value);
                }

            }
        }
        prediction.src_width = 8U;
        prediction.src_height = 8U;
        prediction.mv_col = -127;
        prediction.mv_row = 127;
        CHECK(av1_inter_predict_single(&prediction) == AVIFDEC_OK);
        for (index = 0U; index < 8U * 8U; ++index) {
            CHECK(destination[index] == value);
        }
        prediction.src_width = 16U;
        prediction.src_height = 16U;
    }

    prediction.bit_depth = 8U;
    prediction.filter_x = AV1_INTERP_BILINEAR;
    prediction.filter_y = AV1_INTERP_BILINEAR;
    prediction.subsampling_x = 0U;
    prediction.subsampling_y = 0U;
    prediction.block_x = 2U;
    prediction.block_y = 2U;
    prediction.block_width = 4U;
    prediction.block_height = 4U;
    prediction.mv_col = 4;
    prediction.mv_row = 0;
    for (index = 0U; index < 16U * 16U; ++index) {
        source[index] = (uint16_t)(index % 16U);
    }
    CHECK(av1_inter_predict_single(&prediction) == AVIFDEC_OK);
    for (index = 0U; index < 4U * 4U; ++index) {
        CHECK(destination[(index / 4U) * 8U + index % 4U] ==
              3U + index % 4U);
    }

    prediction.block_x = 0U;
    prediction.block_y = 0U;
    prediction.block_width = 8U;
    prediction.block_height = 8U;
    prediction.mv_col = 0;
    prediction.mv_row = 0;
    for (index = 0U; index < 16U * 16U; ++index) source[index] = 50U;
    CHECK(av1_inter_predict_compound(&prediction, pred0, 8U) == AVIFDEC_OK);
    for (index = 0U; index < 16U * 16U; ++index) source[index] = 200U;
    CHECK(av1_inter_predict_compound(&prediction, pred1, 8U) == AVIFDEC_OK);
    avifdec_memory_fill(&compound, 0U, sizeof(compound));
    compound.dst = destination;
    compound.dst_stride = 8U;
    compound.width = 8U;
    compound.height = 8U;
    compound.bit_depth = 8U;
    compound.weight0 = 8U;
    compound.weight1 = 8U;
    CHECK(av1_inter_blend_average(&compound, pred0, 8U, pred1, 8U) ==
          AVIFDEC_OK);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(destination[index] == 125U);
    }
    compound.weight0 = 5U;
    compound.weight1 = 11U;
    CHECK(av1_inter_blend_average(&compound, pred0, 8U, pred1, 8U) ==
          AVIFDEC_OK);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(destination[index] == 153U);
    }
    CHECK(av1_inter_build_diff_mask(mask, 8U, pred0, 8U, pred1, 8U,
                                    8U, 8U, 8U, 0U) == AVIFDEC_OK);
    CHECK(av1_inter_build_diff_mask(inverse_mask, 8U, pred0, 8U, pred1, 8U,
                                    8U, 8U, 8U, 1U) == AVIFDEC_OK);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(mask[index] + inverse_mask[index] == 64U);
    }
    compound.mask = mask;
    compound.mask_stride = 8U;
    CHECK(av1_inter_blend_masked(&compound, pred0, 8U, pred1, 8U) ==
          AVIFDEC_OK);

    CHECK(av1_inter_build_interintra_mask(mask, 8U, 8U, 8U,
                                          AV1_INTERINTRA_DC) == AVIFDEC_OK);
    for (index = 0U; index < 8U * 8U; ++index) CHECK(mask[index] == 32U);
    CHECK(av1_inter_build_interintra_mask(
              mask, 8U, 8U, 8U, AV1_INTERINTRA_VERTICAL) == AVIFDEC_OK);
    CHECK(mask[0] == 60U && mask[7U * 8U] == 1U);
    CHECK(av1_inter_build_interintra_mask(
              mask, 8U, 8U, 8U, AV1_INTERINTRA_HORIZONTAL) == AVIFDEC_OK);
    CHECK(mask[0] == 60U && mask[7] == 1U);
    CHECK(av1_inter_build_interintra_mask(
              mask, 8U, 8U, 8U, AV1_INTERINTRA_SMOOTH) == AVIFDEC_OK);
    CHECK(mask[7] == 60U && mask[7U * 8U + 7U] == 1U);
    for (index = 0U; index < 8U * 8U; ++index) {
        destination[index] = 0U;
        pred0[index] = 40U;
        intra[index] = 200U;
        mask[index] = 32U;
    }
    av1_inter_blend_interintra(destination, 8U, pred0, 8U, intra, 8U,
                               mask, 8U, 8U, 8U);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(destination[index] == 120U);
    }

    CHECK(av1_inter_build_wedge_mask(mask, 8U, 8U, 8U, 0U, 0U) ==
          AVIFDEC_OK);
    CHECK(av1_inter_build_wedge_mask(inverse_mask, 8U, 8U, 8U, 0U, 1U) ==
          AVIFDEC_OK);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(mask[index] + inverse_mask[index] == 64U);
    }
    CHECK(av1_inter_build_wedge_mask(mask, 8U, 4U, 4U, 0U, 0U) ==
          AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_av1_warp_and_obmc(void) {
    Av1WarpModel model = { { 0, 0, 65536, 0, 0, 65536 } };
    Av1WarpModel projected;
    Av1WarpShear shear;
    Av1WarpSample samples[4] = {
        { 0, 0, 0, 0 }, { 64, 0, 64, 0 },
        { 0, 64, 0, 64 }, { 64, 64, 64, 64 }
    };
    Av1WarpPlaneParams params;
    uint16_t source[16 * 16];
    uint16_t destination[8 * 8];
    uint16_t neighbor[4 * 4];
    uint16_t blended[4 * 4];
    uint8_t shift;
    int32_t factor;
    unsigned int index;

    CHECK(av1_warp_resolve_divisor(65536, &shift, &factor) == AV1_WARP_OK);
    CHECK(shift == 30U && factor == 16384);
    CHECK(av1_warp_derive_shear(&model, &shear) == AV1_WARP_OK);
    CHECK(shear.alpha == 0 && shear.beta == 0 &&
          shear.gamma == 0 && shear.delta == 0);
    CHECK(av1_warp_model_is_valid(&model));
    model.matrix[2] = 0;
    CHECK(!av1_warp_model_is_valid(&model));
    model.matrix[2] = 65536;
    CHECK(av1_warp_project_samples(
              &projected, samples, 4U, 4, 4, 0, 0) == AV1_WARP_OK);
    CHECK(projected.matrix[0] == -176 && projected.matrix[1] == -176 &&
          projected.matrix[2] == 65580 && projected.matrix[3] == 0 &&
          projected.matrix[4] == 0 && projected.matrix[5] == 65580);

    for (index = 0U; index < 16U * 16U; ++index) {
        source[index] =
            (uint16_t)(index % 16U + 2U * (index / 16U));
    }
    avifdec_memory_fill(&params, 0U, sizeof(params));
    params.source = source;
    params.source_stride = 16U;
    params.source_width = 16U;
    params.source_height = 16U;
    params.block_x = 4U;
    params.block_y = 4U;
    params.block_width = 8U;
    params.block_height = 8U;
    params.bit_depth = 8U;
    params.model = &model;
    CHECK(av1_warp_predict_single(&params, destination, 8U) == AV1_WARP_OK);
    for (index = 0U; index < 8U * 8U; ++index) {
        CHECK(destination[index] ==
              source[(4U + index / 8U) * 16U + 4U + index % 8U]);
    }

    for (index = 0U; index < 4U * 4U; ++index) {
        blended[index] = 100U;
        neighbor[index] = 200U;
    }
    CHECK(av1_inter_blend_obmc(
              blended, 4U, neighbor, 4U, 4U, 4U, 1) == AVIFDEC_OK);
    CHECK(blended[0] == 139U && blended[1] == 122U &&
          blended[2] == 108U && blended[3] == 100U);
    return 0;
}

static int test_av1_nondirectional_predictors(void) {
    const uint16_t above[8] = { 10U, 20U, 30U, 40U, 1000U, 900U, 800U, 700U };
    const uint16_t left[4] = { 50U, 60U, 70U, 80U };
    Av1IntraReferences references;
    uint16_t destination[32];

    references.above = above;
    references.left = left;
    references.top_left = 25U;
    references.have_above = 1U;
    references.have_left = 1U;
    CHECK(av1_predict_dc(destination, 8U, 4U, 4U, 8U, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 45U && destination[3U * 8U + 3U] == 45U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_VERTICAL, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 10U && destination[3] == 40U &&
        destination[3U * 8U + 2U] == 30U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_HORIZONTAL, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 50U && destination[3U * 8U + 3U] == 80U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_PAETH, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 25U && destination[1] == 50U &&
        destination[8U] == 60U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_SMOOTH_VERTICAL, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 10U && destination[8U] == 39U &&
        destination[8U + 3U] == 57U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_SMOOTH_HORIZONTAL, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 50U && destination[1] == 46U &&
        destination[2] == 43U && destination[3] == 43U);
    CHECK(av1_predict_nondirectional(destination, 8U, 4U, 4U, 8U,
          AV1_PREDICT_SMOOTH, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 30U && destination[3U * 8U + 3U] == 60U);

    references.have_above = 0U;
    CHECK(av1_predict_dc(destination, 8U, 4U, 4U, 12U, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 65U);
    references.have_left = 0U;
    CHECK(av1_predict_dc(destination, 8U, 4U, 4U, 12U, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 2048U);
    references.have_above = 1U;
    CHECK(av1_predict_dc(destination, 8U, 8U, 4U, 10U, &references) == AVIFDEC_OK);
    CHECK(destination[0] == 438U);
    CHECK(av1_predict_dc(destination, 3U, 4U, 4U, 8U, &references) ==
        AVIFDEC_INVALID_ARGUMENT);
    CHECK(av1_predict_dc(destination, 8U, 5U, 4U, 8U, &references) ==
        AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_av1_directional_predictors(void) {
    const uint16_t above[8] = { 10U, 20U, 30U, 40U, 50U, 60U, 70U, 80U };
    const uint16_t left[8] = { 90U, 100U, 110U, 120U, 130U, 140U, 150U, 160U };
    Av1IntraReferences references;
    uint16_t destination[16];

    references.above = above;
    references.left = left;
    references.top_left = 25U;
    references.have_above = 1U;
    references.have_left = 1U;
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 45U,
              &references) == AVIFDEC_OK);
    CHECK(destination[0] == 20U && destination[3] == 50U &&
          destination[3U * 4U] == 50U && destination[15] == 80U);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 90U,
              &references) == AVIFDEC_OK);
    CHECK(destination[0] == 10U && destination[15] == 40U);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 99U,
              &references) == AVIFDEC_OK);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 135U,
              &references) == AVIFDEC_OK);
    CHECK(destination[0] == 25U && destination[1] == 10U &&
          destination[4U] == 90U && destination[15] == 25U);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 180U,
              &references) == AVIFDEC_OK);
    CHECK(destination[0] == 90U && destination[15] == 120U);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 189U,
              &references) == AVIFDEC_OK);
    CHECK(av1_predict_directional(destination, 4U, 4U, 4U, 8U, 46U,
              &references) == AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_av1_cfl_predictor(void) {
    uint16_t luma[64];
    uint16_t destination[16];
    uint16_t edge_prediction[16];
    uint32_t index;

    for (index = 0U; index < 64U; ++index) luma[index] = (uint16_t)index;
    for (index = 0U; index < 16U; ++index) destination[index] = 100U;
    CHECK(av1_predict_cfl(destination, 4U, luma, 4U, 4U, 4U, 0U, 0U, 4U, 4U,
              0U, 0U, 8, 8U) == AVIFDEC_OK);
    CHECK(destination[0] == 92U && destination[7] == 99U &&
          destination[8] == 101U && destination[15] == 108U);

    for (index = 0U; index < 16U; ++index) destination[index] = 100U;
    CHECK(av1_predict_cfl(destination, 4U, luma, 8U, 8U, 8U, 0U, 0U, 4U, 4U,
              1U, 1U, -8, 10U) == AVIFDEC_OK);
    CHECK(destination[0] == 127U && destination[15] == 73U);
    for (index = 0U; index < 16U; ++index) destination[index] = 2U;
    CHECK(av1_predict_cfl(destination, 4U, luma, 4U, 4U, 4U, 0U, 0U, 4U, 4U,
              0U, 0U, 16, 8U) == AVIFDEC_OK);
    CHECK(destination[0] == 0U && destination[15] == 17U);
    CHECK(av1_predict_cfl(destination, 4U, luma, 3U, 4U, 4U, 0U, 0U, 4U, 4U,
              0U, 0U, 1, 8U) == AVIFDEC_INVALID_ARGUMENT);
    for (index = 0U; index < 16U; ++index) destination[index] = 100U;
    CHECK(av1_predict_cfl(destination, 4U, luma, 8U, 6U, 8U, 4U, 0U, 4U, 4U,
              1U, 1U, 8, 8U) == AVIFDEC_OK);
    avifdec_memory_copy(edge_prediction, destination, sizeof(destination));
    for (index = 0U; index < 8U; ++index) {
        luma[index * 8U + 6U] = 255U;
        luma[index * 8U + 7U] = 255U;
    }
    for (index = 0U; index < 16U; ++index) destination[index] = 100U;
    CHECK(av1_predict_cfl(destination, 4U, luma, 8U, 6U, 8U, 4U, 0U, 4U, 4U,
              1U, 1U, 8, 8U) == AVIFDEC_OK);
    CHECK(avifdec_memory_compare(
        destination, edge_prediction, sizeof(destination)) == 0);
    return 0;
}

    static int test_av1_prediction_edges_and_tools(void) {
        uint16_t plane[16U * 16U];
        uint16_t destination[8U * 8U];
        uint16_t above[8] = { 10U, 20U, 30U, 40U, 50U, 60U, 70U, 80U };
        uint16_t left[8] = { 50U, 60U, 70U, 80U, 90U, 100U, 110U, 120U };
        uint16_t palette[2] = { 10U, 200U };
        uint8_t color_map[16] = {
          0U, 1U, 0U, 1U,
          1U, 0U, 1U, 0U,
          0U, 1U, 0U, 1U,
          1U, 0U, 1U, 0U
        };
        Av1PreparedReferences prepared;
        Av1IntraReferences references;
        uint64_t checksum0;
        uint64_t checksum1;
        uint32_t index;
        uint8_t mode;

        for (index = 0U; index < 16U * 16U; ++index) plane[index] = (uint16_t)index;
        CHECK(av1_predict_prepare_references(plane, 16U, 16U, 16U, 16U, 16U,
            0U, 0U, 4U, 4U, 8U, 0U, 0U, 0U, 0U,
            &prepared) == AVIFDEC_OK);
        CHECK(prepared.references.top_left == 128U &&
            prepared.references.above[0] == 127U &&
            prepared.references.above[7] == 127U &&
            prepared.references.left[0] == 129U &&
            prepared.references.left[7] == 129U);
        CHECK(av1_predict_prepare_references(plane, 16U, 16U, 16U, 16U, 16U,
            4U, 4U, 4U, 4U, 8U, 1U, 1U, 0U, 0U,
            &prepared) == AVIFDEC_OK);
        CHECK(prepared.references.top_left == 51U &&
            prepared.references.above[0] == 52U &&
            prepared.references.above[3] == 55U &&
            prepared.references.above[7] == 55U &&
            prepared.references.left[0] == 67U &&
            prepared.references.left[3] == 115U &&
            prepared.references.left[7] == 115U);
        CHECK(av1_predict_prepare_references(plane, 16U, 16U, 16U, 8U, 8U,
            4U, 4U, 4U, 4U, 8U, 1U, 1U, 1U, 1U,
            &prepared) == AVIFDEC_OK);
        CHECK(prepared.references.above[0] == 52U &&
            prepared.references.above[3] == 55U &&
            prepared.references.above[4] == 55U &&
            prepared.references.above[7] == 55U &&
            prepared.references.left[0] == 67U &&
            prepared.references.left[3] == 115U &&
            prepared.references.left[4] == 115U &&
            prepared.references.left[7] == 115U);
        CHECK(av1_predict_prepare_references(plane, 16U, 16U, 16U, 16U, 16U,
            0U, 0U, 4U, 4U, 8U, 1U, 0U, 0U, 0U,
            &prepared) == AVIFDEC_INVALID_ARGUMENT);
        CHECK(av1_predict_edge_filter_strength(16U, 8U, 0U, 8) == 1U &&
            av1_predict_edge_filter_strength(16U, 8U, 0U, 16) == 2U &&
            av1_predict_edge_filter_strength(16U, 8U, 0U, 32) == 3U &&
            av1_predict_edge_filter_strength(8U, 8U, 1U, 48) == 2U);
        CHECK(av1_predict_edge_upsample_selected(4U, 4U, 0U, 23) == 1 &&
            av1_predict_edge_upsample_selected(8U, 8U, 1U, 23) == 0 &&
            av1_predict_edge_upsample_selected(4U, 4U, 0U, 40) == 0);

        for (index = 0U; index < 16U * 16U; ++index) plane[index] = 512U;
        CHECK(av1_predict_prepare_references(plane, 16U, 16U, 16U, 16U, 16U,
            4U, 4U, 4U, 4U, 10U, 1U, 1U, 1U, 1U,
            &prepared) == AVIFDEC_OK);
        CHECK(av1_predict_directional_edges(destination, 8U, 4U, 4U, 10U,
            67U, 0U, &prepared) == AVIFDEC_OK);
        for (index = 0U; index < 4U; ++index) {
          CHECK(destination[index] == 512U && destination[3U * 8U + index] == 512U);
        }

        references.above = above;
        references.left = left;
        references.top_left = 25U;
        references.have_above = 1U;
        references.have_left = 1U;
        CHECK(av1_predict_filter_intra(destination, 8U, 4U, 4U, 8U,
            0U, &references) == AVIFDEC_OK);
        CHECK(destination[0] == 34U);
        for (index = 0U; index < 8U; ++index) {
          above[index] = 100U;
          left[index] = 100U;
        }
        references.top_left = 100U;
        for (mode = 0U; mode < 5U; ++mode) {
          CHECK(av1_predict_filter_intra(destination, 8U, 4U, 4U, 12U,
              mode, &references) == AVIFDEC_OK);
          CHECK(destination[0] == 100U && destination[3U * 8U + 3U] == 100U);
        }
        CHECK(av1_predict_filter_intra(destination, 8U, 4U, 4U, 8U,
            5U, &references) == AVIFDEC_INVALID_ARGUMENT);

        CHECK(av1_predict_palette(destination, 8U, 4U, 4U, 8U,
            palette, 2U, color_map, 4U) == AVIFDEC_OK);
        CHECK(destination[0] == 10U && destination[1] == 200U &&
            destination[8U] == 200U && destination[3U * 8U + 3U] == 10U);
        color_map[15] = 2U;
        CHECK(av1_predict_palette(destination, 8U, 4U, 4U, 8U,
            palette, 2U, color_map, 4U) == AVIFDEC_INVALID_DATA);
        color_map[15] = 0U;
        CHECK(av1_predict_checksum(destination, 8U, 4U, 4U, 0U,
            &checksum0) == AVIFDEC_OK);
        CHECK(av1_predict_checksum(destination, 8U, 4U, 4U, 1U,
            &checksum1) == AVIFDEC_OK && checksum0 != checksum1);
        destination[0] ^= 1U;
        CHECK(av1_predict_checksum(destination, 8U, 4U, 4U, 0U,
            &checksum1) == AVIFDEC_OK && checksum0 != checksum1);
        return 0;
    }

static int test_byte_reader(void) {
    const unsigned char data[12] = {
        0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U,
        0x77U, 0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU
    };
    AvifdecByteReader reader;
    AvifdecByteReader child;

    avifdec_byte_reader_init(&reader, data, sizeof(data), 100U);
    CHECK(avifdec_byte_reader_u8(&reader) == 0x11U);
    CHECK(avifdec_byte_reader_u16be(&reader) == 0x2233U);
    CHECK(avifdec_byte_reader_offset(&reader) == 103U);
    CHECK(avifdec_byte_reader_subreader(&reader, 4U, &child) == AVIFDEC_OK);
    CHECK(avifdec_byte_reader_u32be(&child) == 0x44556677U);
    CHECK(avifdec_byte_reader_remaining(&reader) == 5U);
    CHECK(avifdec_byte_reader_u64be(&reader) == 0U);
    CHECK(reader.status == AVIFDEC_TRUNCATED);
    CHECK(avifdec_byte_reader_u8(&reader) == 0U);
    return 0;
}

static int test_av1_predictor_matrix(void) {
    static uint16_t plane[128U * 128U];
    static uint16_t destination[64U * 64U];
    static uint16_t above[128];
    static uint16_t left[128];
    static const uint32_t sizes[5] = { 4U, 8U, 16U, 32U, 64U };
    static const uint16_t angles[8] = {
        90U, 180U, 45U, 135U, 113U, 157U, 203U, 67U
    };
    Av1IntraReferences references;
    uint32_t depth_index;
    uint32_t width_index;
    uint32_t height_index;
    uint32_t index;

    references.above = above;
    references.left = left;
    references.have_above = 1U;
    references.have_left = 1U;
    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        uint8_t bit_depth = (uint8_t)(8U + 2U * depth_index);
        uint16_t constant = (uint16_t)(1U << (bit_depth - 2U));

        references.top_left = constant;
        for (index = 0U; index < 128U; ++index) {
            above[index] = constant;
            left[index] = constant;
        }
        for (width_index = 0U; width_index < 5U; ++width_index) {
            for (height_index = 0U; height_index < 5U; ++height_index) {
                uint32_t width = sizes[width_index];
                uint32_t height = sizes[height_index];
                uint8_t mode;

                CHECK(av1_predict_dc(destination, width, width, height,
                      bit_depth, &references) == AVIFDEC_OK);
                CHECK(destination[0] == constant &&
                      destination[(size_t)(height - 1U) * width + width - 1U] == constant);
                for (mode = AV1_PREDICT_VERTICAL;
                     mode <= AV1_PREDICT_SMOOTH_HORIZONTAL; ++mode) {
                    CHECK(av1_predict_nondirectional(destination, width, width,
                          height, bit_depth, (Av1PredictMode)mode,
                          &references) == AVIFDEC_OK);
                    CHECK(destination[0] == constant &&
                          destination[(size_t)(height - 1U) * width +
                                      width - 1U] == constant);
                }
                for (index = 0U; index < 8U; ++index) {
                    CHECK(av1_predict_directional(destination, width, width,
                          height, bit_depth, angles[index],
                          &references) == AVIFDEC_OK);
                    CHECK(destination[0] == constant &&
                          destination[(size_t)(height - 1U) * width +
                                      width - 1U] == constant);
                }
                if (width <= 32U && height <= 32U) {
                    for (mode = 0U; mode < 5U; ++mode) {
                        CHECK(av1_predict_filter_intra(destination, width, width,
                              height, bit_depth, mode, &references) == AVIFDEC_OK);
                        CHECK(destination[0] == constant &&
                              destination[(size_t)(height - 1U) * width +
                                          width - 1U] == constant);
                    }
                }
            }
        }
        for (index = 0U; index < 128U * 128U; ++index) plane[index] = constant;
        for (width_index = 0U; width_index < 2U; ++width_index) {
            for (height_index = 0U; height_index < 2U; ++height_index) {
                uint8_t subsampling_x = (uint8_t)width_index;
                uint8_t subsampling_y = (uint8_t)height_index;
                uint32_t row;
                uint32_t column;

                for (row = 0U; row < 8U; ++row) {
                    for (column = 0U; column < 8U; ++column) {
                        destination[(size_t)row * 8U + column] = constant;
                    }
                }
                    CHECK(av1_predict_cfl(destination, 8U, plane, 128U,
                        128U, 128U, 0U, 0U, 8U, 8U,
                        subsampling_x, subsampling_y, 16,
                      bit_depth) == AVIFDEC_OK);
                CHECK(destination[0] == constant && destination[63] == constant);
            }
        }
    }
    return 0;
}

static int test_bit_reader(void) {
    const unsigned char data[2] = { 0xb2U, 0x61U };
    AvifdecBitReader reader;

    avifdec_bit_reader_init(&reader, data, sizeof(data), 20U);
    CHECK(avifdec_bit_reader_read(&reader, 3U) == 5U);
    CHECK(avifdec_bit_reader_read(&reader, 5U) == 18U);
    CHECK(avifdec_bit_reader_read(&reader, 4U) == 6U);
    CHECK(avifdec_bit_reader_align(&reader) == AVIFDEC_OK);
    CHECK(avifdec_bit_reader_offset(&reader) == 22U);
    CHECK(avifdec_bit_reader_read(&reader, 1U) == 0U);
    CHECK(reader.status == AVIFDEC_TRUNCATED);

    avifdec_bit_reader_init(&reader, data, sizeof(data), 0U);
    CHECK(avifdec_bit_reader_read(&reader, 33U) == 0U);
    CHECK(reader.status == AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static AvifdecStatus test_parse_metadata(
    const unsigned char *data,
    size_t size,
    uint8_t extension_flag,
    const Av1MetadataConfig *config,
    AvifdecImageInfo *info) {
    AvifdecSpan span;
    Av1Stream stream;
    Av1Bits bits;

    span.data = data;
    span.size = size;
    span.file_offset = 0U;
    stream.spans = &span;
    stream.span_count = 1U;
    stream.size = size;
    stream.position = 0U;
    stream.status = AVIFDEC_OK;
    av1_bits_init(&bits, &stream, 0U, size);
    return av1_metadata_parse(
        &bits, config, extension_flag, info);
}

static int test_av1_metadata(void) {
    static const unsigned char hdr_cll[] = {
        0x01U, 0x01U, 0x23U, 0x04U, 0x56U, 0x80U
    };
    static const unsigned char hdr_mdcv[] = {
        0x02U,
        0x00U, 0x01U, 0x00U, 0x02U,
        0x00U, 0x03U, 0x00U, 0x04U,
        0x00U, 0x05U, 0x00U, 0x06U,
        0x00U, 0x07U, 0x00U, 0x08U,
        0x00U, 0x00U, 0x00U, 0x09U,
        0x00U, 0x00U, 0x00U, 0x0aU,
        0x80U
    };
    static const unsigned char scalability[] = {
        0x03U, 0x0eU, 0x78U,
        0x00U, 0x40U, 0x00U, 0x20U,
        0x00U, 0x80U, 0x00U, 0x40U,
        0x05U, 0x06U,
        0x01U, 0x11U, 0x00U, 0x80U
    };
    static const unsigned char reserved_scalability[] = {
        0x03U, 0xffU, 0x80U
    };
    static const unsigned char itut_t35[] = {
        0x04U, 0xffU, 0x35U, 0x01U, 0x02U, 0x03U,
        0x80U, 0x00U
    };
    static const unsigned char timecode_full[] = {
        0x05U, 0x14U, 0x06U, 0x45U, 0xc1U, 0xc1U
    };
    static const unsigned char timecode_partial[] = {
        0x05U, 0x10U, 0x06U, 0x81U
    };
    static const unsigned char bad_trailing[] = {
        0x01U, 0x01U, 0x23U, 0x04U, 0x56U, 0x00U
    };
    Av1MetadataConfig config;
    AvifdecImageInfo info;

    avifdec_memory_fill(&config, 0U, sizeof(config));
    config.max_width = 256U;
    config.max_height = 128U;
    config.timing_info_present = 1U;
    config.num_units_in_display_tick = 1U;
    config.time_scale = 60U;
    avifdec_memory_fill(&info, 0U, sizeof(info));
    CHECK(test_parse_metadata(
        hdr_cll, sizeof(hdr_cll), 0U, &config, &info) ==
        AVIFDEC_OK);
    CHECK(info.hdr_cll.max_cll == 0x0123U &&
          info.hdr_cll.max_fall == 0x0456U);
    CHECK(test_parse_metadata(
        hdr_mdcv, sizeof(hdr_mdcv), 0U, &config, &info) ==
        AVIFDEC_OK);
    CHECK(info.hdr_mdcv.primary_x[2] == 5U &&
          info.hdr_mdcv.primary_y[2] == 6U &&
          info.hdr_mdcv.white_point_y == 8U &&
          info.hdr_mdcv.luminance_max == 9U &&
          info.hdr_mdcv.luminance_min == 10U);
    CHECK(test_parse_metadata(
        scalability, sizeof(scalability), 0U, &config, &info) ==
        AVIFDEC_OK);
    CHECK(info.scalability_mode_idc == 14U &&
          info.spatial_layer_count == 2U &&
          info.spatial_layer_width[1] == 128U &&
          info.spatial_layer_height[1] == 64U &&
          info.spatial_layer_ref_id[0] == 5U &&
          info.temporal_group_size == 1U &&
          info.scalability_checksum != 0U);
    CHECK(test_parse_metadata(
        reserved_scalability, sizeof(reserved_scalability), 0U,
        &config, &info) == AVIFDEC_OK);
    CHECK(info.scalability_mode_idc == 0xffU);
    CHECK(test_parse_metadata(
        itut_t35, sizeof(itut_t35), 1U, &config, &info) ==
        AVIFDEC_OK);
    CHECK(info.itu_t35_country_code == 0xffU &&
          info.itu_t35_country_code_extension == 0x35U &&
          info.itu_t35_payload_size == 3U &&
          info.itu_t35_payload_checksum != 0U);
    CHECK(test_parse_metadata(
        timecode_full, sizeof(timecode_full), 0U, &config, &info) ==
        AVIFDEC_OK);
    CHECK(info.timecode.counting_type == 2U &&
          info.timecode.n_frames == 12U &&
          info.timecode.seconds == 34U &&
          info.timecode.minutes == 56U &&
          info.timecode.hours == 7U);
    CHECK(test_parse_metadata(
        timecode_partial, sizeof(timecode_partial), 0U, &config,
        &info) == AVIFDEC_OK);
    CHECK(info.timecode.n_frames == 13U &&
          info.timecode.seconds == 34U &&
          info.timecode.minutes == 56U &&
          info.timecode.hours == 7U);
    CHECK(test_parse_metadata(
        hdr_cll, sizeof(hdr_cll), 1U, &config, &info) ==
        AVIFDEC_INVALID_DATA);
    CHECK(test_parse_metadata(
        bad_trailing, sizeof(bad_trailing), 0U, &config, &info) ==
        AVIFDEC_INVALID_DATA);
    return 0;
}

static int test_av1_profile_and_level(void) {
    CHECK(av1_profile_validate(0U, 8U, 0U, 1U, 1U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(0U, 10U, 1U, 1U, 1U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(0U, 12U, 0U, 1U, 1U) ==
          AVIFDEC_INVALID_DATA);
    CHECK(av1_profile_validate(1U, 10U, 0U, 0U, 0U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(1U, 8U, 1U, 1U, 1U) ==
          AVIFDEC_INVALID_DATA);
    CHECK(av1_profile_validate(2U, 8U, 0U, 1U, 0U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(2U, 12U, 0U, 1U, 1U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(2U, 12U, 0U, 0U, 0U) ==
          AVIFDEC_OK);
    CHECK(av1_profile_validate(2U, 10U, 0U, 1U, 1U) ==
          AVIFDEC_INVALID_DATA);
    CHECK(av1_level_validate_dimensions(
        0U, 2048U, 72U, 1) == AVIFDEC_OK);
    CHECK(av1_level_validate_dimensions(
        0U, 2049U, 72U, 0) == AVIFDEC_INVALID_DATA);
    CHECK(av1_level_validate_dimensions(
        0U, 2048U, 1152U, 1) == AVIFDEC_INVALID_DATA);
    CHECK(av1_level_validate_dimensions(
        9U, 1920U, 1080U, 1) == AVIFDEC_OK);
    CHECK(av1_level_validate_dimensions(
        2U, 640U, 360U, 1) == AVIFDEC_INVALID_DATA);
    CHECK(av1_level_validate_dimensions(
        31U, 65535U, 65535U, 1) == AVIFDEC_OK);
    return 0;
}

typedef struct {
    unsigned char data[256];
    size_t bit_count;
} TestBitWriter;

static void test_bit_writer_put(TestBitWriter *writer,
                                uint32_t value,
                                unsigned int count) {
    unsigned int index;

    for (index = 0U; index < count; ++index) {
        size_t position = writer->bit_count++;
        unsigned int shift = count - index - 1U;

        if ((position & 7U) == 0U) {
            writer->data[position >> 3U] = 0U;
        }
        writer->data[position >> 3U] |=
            (unsigned char)(((value >> shift) & 1U) <<
                            (7U - (position & 7U)));
    }
}

static AvifdecStatus test_parse_film_grain(
    const TestBitWriter *writer,
    const Av1FilmGrainParseConfig *config,
    Av1FilmGrainParams *params) {
    AvifdecSpan span;
    Av1Stream stream;
    Av1Bits bits;
    size_t size = (writer->bit_count + 7U) >> 3U;

    span.data = writer->data;
    span.size = size;
    span.file_offset = 0U;
    stream.spans = &span;
    stream.span_count = 1U;
    stream.size = size;
    stream.position = 0U;
    stream.status = AVIFDEC_OK;
    av1_bits_init(&bits, &stream, 0U, size);
    return av1_film_grain_parse(&bits, config, params);
}

typedef struct {
    size_t calls;
    size_t worker_count;
} TestReverseExecutorState;

static AvifdecStatus test_reverse_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg) {
    TestReverseExecutorState *state =
        (TestReverseExecutorState *)user_data;
    size_t index = count;

    (void)min_chunk;
    ++state->calls;
    while (index != 0U) {
        AvifdecStatus status;

        --index;
        status = body(
            index, index + 1U,
            index % state->worker_count, arg);
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static int test_av1_film_grain(void) {
    TestBitWriter writer;
    Av1FilmGrainParseConfig config;
    Av1FilmGrainParams params;
    Av1FilmGrainParams reference;
    const Av1FilmGrainParams *reference_params[8];
    uint8_t ref_frame_idx[7] = { 3U, 0U, 0U, 0U, 0U, 0U, 0U };
    static uint16_t y420[65U * 35U];
    static uint16_t u420[33U * 18U];
    static uint16_t v420[33U * 18U];
    static uint16_t parallel_y420[65U * 35U];
    static uint16_t parallel_u420[33U * 18U];
    static uint16_t parallel_v420[33U * 18U];
    static uint16_t y444[33U * 37U];
    static uint16_t u444[33U * 37U];
    static uint16_t v444[33U * 37U];
    static int16_t scratch[300000U];
    Av1FilmGrainImage image;
    Av1FilmGrainImage parallel_image;
    TestReverseExecutorState executor_state = { 0U, 4U };
    AvifdecExecutor executor = {
        &executor_state, 4U, test_reverse_parallel_for
    };
    size_t scratch_size;
    size_t parallel_scratch_size;
    uint64_t checksum_y;
    uint64_t checksum_u;
    uint64_t checksum_v;
    uint32_t row;
    uint32_t column;

    avifdec_memory_fill(&writer, 0U, sizeof(writer));
    test_bit_writer_put(&writer, 1U, 1U);
    test_bit_writer_put(&writer, 0x1234U, 16U);
    test_bit_writer_put(&writer, 2U, 4U);
    test_bit_writer_put(&writer, 0U, 8U);
    test_bit_writer_put(&writer, 16U, 8U);
    test_bit_writer_put(&writer, 255U, 8U);
    test_bit_writer_put(&writer, 32U, 8U);
    test_bit_writer_put(&writer, 0U, 2U);
    test_bit_writer_put(&writer, 0U, 2U);
    test_bit_writer_put(&writer, 0U, 2U);
    test_bit_writer_put(&writer, 0U, 2U);
    test_bit_writer_put(&writer, 1U, 1U);
    test_bit_writer_put(&writer, 1U, 1U);
    avifdec_memory_fill(&config, 0U, sizeof(config));
    config.film_grain_params_present = 1U;
    config.show_frame = 1U;
    config.mono_chrome = 1U;
    CHECK(test_parse_film_grain(
        &writer, &config, &params) == AVIFDEC_OK);
    CHECK(params.apply_grain == 1U &&
          params.grain_seed == 0x1234U &&
          params.update_grain == 1U &&
          params.num_y_points == 2U &&
          params.point_y_value[1] == 255U &&
          params.overlap_flag == 1U &&
          params.clip_to_restricted_range == 1U);

    reference = params;
    avifdec_memory_fill(
        reference_params, 0U, sizeof(reference_params));
    reference_params[3] = &reference;
    avifdec_memory_fill(&writer, 0U, sizeof(writer));
    test_bit_writer_put(&writer, 1U, 1U);
    test_bit_writer_put(&writer, 0x5678U, 16U);
    test_bit_writer_put(&writer, 0U, 1U);
    test_bit_writer_put(&writer, 3U, 3U);
    config.frame_type = 1U;
    config.ref_frame_idx = ref_frame_idx;
    config.reference_params = reference_params;
    CHECK(test_parse_film_grain(
        &writer, &config, &params) == AVIFDEC_OK);
    CHECK(params.grain_seed == 0x5678U &&
          params.num_y_points == reference.num_y_points &&
          params.point_y_scaling[1] ==
              reference.point_y_scaling[1]);

    avifdec_memory_fill(&params, 0U, sizeof(params));
    params.apply_grain = 1U;
    params.grain_seed = 0x2345U;
    params.num_y_points = 2U;
    params.point_y_value[0] = 0U;
    params.point_y_value[1] = 255U;
    params.point_y_scaling[0] = 24U;
    params.point_y_scaling[1] = 72U;
    params.chroma_scaling_from_luma = 1U;
    params.grain_scaling_minus_8 = 1U;
    params.ar_coeff_lag = 2U;
    params.ar_coeff_shift_minus_6 = 2U;
    params.grain_scale_shift = 1U;
    params.overlap_flag = 1U;
    params.clip_to_restricted_range = 1U;
    avifdec_memory_fill(
        params.ar_coeffs_y_plus_128, 128U,
        sizeof(params.ar_coeffs_y_plus_128));
    avifdec_memory_fill(
        params.ar_coeffs_cb_plus_128, 128U,
        sizeof(params.ar_coeffs_cb_plus_128));
    avifdec_memory_fill(
        params.ar_coeffs_cr_plus_128, 128U,
        sizeof(params.ar_coeffs_cr_plus_128));
    params.ar_coeffs_y_plus_128[0] = 132U;
    params.ar_coeffs_y_plus_128[5] = 125U;
    params.ar_coeffs_cb_plus_128[12] = 132U;
    params.ar_coeffs_cr_plus_128[12] = 124U;
    for (row = 0U; row < 35U; ++row) {
        for (column = 0U; column < 65U; ++column) {
            y420[(size_t)row * 65U + column] =
                (uint16_t)((row * 5U + column * 3U) & 255U);
        }
    }
    for (row = 0U; row < 18U; ++row) {
        for (column = 0U; column < 33U; ++column) {
            u420[(size_t)row * 33U + column] =
                (uint16_t)((64U + row * 2U + column * 7U) & 255U);
            v420[(size_t)row * 33U + column] =
                (uint16_t)((192U + row * 3U + column * 5U) & 255U);
        }
    }
    avifdec_memory_fill(&image, 0U, sizeof(image));
    image.plane[0] = y420;
    image.plane[1] = u420;
    image.plane[2] = v420;
    image.stride[0] = 65U;
    image.stride[1] = 33U;
    image.stride[2] = 33U;
    image.width = 65U;
    image.height = 35U;
    image.bit_depth = 8U;
    image.subsampling_x = 1U;
    image.subsampling_y = 1U;
    CHECK(av1_film_grain_scratch_size(
        image.width, &scratch_size) == AVIFDEC_OK);
    CHECK(av1_film_grain_scratch_size_ex(
        image.width, executor.worker_count,
        &parallel_scratch_size) == AVIFDEC_OK);
    CHECK(scratch_size <= sizeof(scratch));
    CHECK(parallel_scratch_size <= sizeof(scratch));
    avifdec_memory_copy(parallel_y420, y420, sizeof(y420));
    avifdec_memory_copy(parallel_u420, u420, sizeof(u420));
    avifdec_memory_copy(parallel_v420, v420, sizeof(v420));
    parallel_image = image;
    parallel_image.plane[0] = parallel_y420;
    parallel_image.plane[1] = parallel_u420;
    parallel_image.plane[2] = parallel_v420;
    CHECK(av1_film_grain_apply(
        &params, &image, scratch, sizeof(scratch)) == AVIFDEC_OK);
    CHECK(av1_film_grain_apply_ex(
        &params, &parallel_image, scratch,
        parallel_scratch_size, &executor) == AVIFDEC_OK);
    CHECK(executor_state.calls != 0U);
    CHECK(avifdec_memory_compare(
        y420, parallel_y420, sizeof(y420)) == 0);
    CHECK(avifdec_memory_compare(
        u420, parallel_u420, sizeof(u420)) == 0);
    CHECK(avifdec_memory_compare(
        v420, parallel_v420, sizeof(v420)) == 0);
    CHECK(av1_film_grain_apply_ex(
        &params, &parallel_image, scratch,
        parallel_scratch_size - 1U, &executor) ==
        AVIFDEC_INVALID_ARGUMENT);
    CHECK(av1_predict_checksum(
        y420, 65U, 65U, 35U, 0U, &checksum_y) == AVIFDEC_OK);
    CHECK(av1_predict_checksum(
        u420, 33U, 33U, 18U, 1U, &checksum_u) == AVIFDEC_OK);
    CHECK(av1_predict_checksum(
        v420, 33U, 33U, 18U, 2U, &checksum_v) == AVIFDEC_OK);
    CHECK(checksum_y == 0xe690248dbbd442c8ULL &&
          checksum_u == 0xd474cbd8e97fd441ULL &&
          checksum_v == 0x8ea84945af4c8589ULL);

    avifdec_memory_fill(&params, 0U, sizeof(params));
    params.apply_grain = 1U;
    params.grain_seed = 0xabcdU;
    params.num_y_points = 2U;
    params.num_cb_points = 2U;
    params.num_cr_points = 2U;
    params.point_y_value[1] = 255U;
    params.point_cb_value[1] = 255U;
    params.point_cr_value[1] = 255U;
    params.point_y_scaling[0] = 16U;
    params.point_y_scaling[1] = 48U;
    params.point_cb_scaling[0] = 20U;
    params.point_cb_scaling[1] = 56U;
    params.point_cr_scaling[0] = 28U;
    params.point_cr_scaling[1] = 64U;
    params.ar_coeff_lag = 1U;
    params.ar_coeff_shift_minus_6 = 1U;
    params.grain_scale_shift = 1U;
    params.cb_mult = 160U;
    params.cb_luma_mult = 112U;
    params.cb_offset = 240U;
    params.cr_mult = 144U;
    params.cr_luma_mult = 136U;
    params.cr_offset = 272U;
    params.overlap_flag = 1U;
    params.clip_to_restricted_range = 1U;
    avifdec_memory_fill(
        params.ar_coeffs_y_plus_128, 128U,
        sizeof(params.ar_coeffs_y_plus_128));
    avifdec_memory_fill(
        params.ar_coeffs_cb_plus_128, 128U,
        sizeof(params.ar_coeffs_cb_plus_128));
    avifdec_memory_fill(
        params.ar_coeffs_cr_plus_128, 128U,
        sizeof(params.ar_coeffs_cr_plus_128));
    params.ar_coeffs_y_plus_128[0] = 130U;
    params.ar_coeffs_cb_plus_128[4] = 131U;
    params.ar_coeffs_cr_plus_128[4] = 125U;
    for (row = 0U; row < 37U; ++row) {
        for (column = 0U; column < 33U; ++column) {
            size_t position = (size_t)row * 33U + column;

            y444[position] =
                (uint16_t)((128U + row * 13U + column * 17U) &
                           1023U);
            u444[position] =
                (uint16_t)((512U + row * 11U + column * 7U) &
                           1023U);
            v444[position] =
                (uint16_t)((768U + row * 5U + column * 19U) &
                           1023U);
        }
    }
    image.plane[0] = y444;
    image.plane[1] = u444;
    image.plane[2] = v444;
    image.stride[0] = 33U;
    image.stride[1] = 33U;
    image.stride[2] = 33U;
    image.width = 33U;
    image.height = 37U;
    image.bit_depth = 10U;
    image.subsampling_x = 0U;
    image.subsampling_y = 0U;
    CHECK(av1_film_grain_apply(
        &params, &image, scratch, sizeof(scratch)) == AVIFDEC_OK);
    CHECK(av1_predict_checksum(
        y444, 33U, 33U, 37U, 0U, &checksum_y) == AVIFDEC_OK);
    CHECK(av1_predict_checksum(
        u444, 33U, 33U, 37U, 1U, &checksum_u) == AVIFDEC_OK);
    CHECK(av1_predict_checksum(
        v444, 33U, 33U, 37U, 2U, &checksum_v) == AVIFDEC_OK);
    CHECK(checksum_y == 0x48d108c79d85f90aULL &&
          checksum_u == 0x60a5bf55bf855a35ULL &&
          checksum_v == 0x294849a2318bb2a7ULL);
    return 0;
}

static int test_arena(void) {
    unsigned char memory[64];
    AvifdecArena arena;
    AvifdecArena sizing;
    void *first;
    void *second;

    avifdec_arena_init_sizing(&sizing);
    CHECK(avifdec_arena_allocate(&sizing, 3U, 1U) == 0);
    CHECK(avifdec_arena_allocate(&sizing, 8U, 8U) == 0);
    CHECK(sizing.status == AVIFDEC_OK);
    CHECK(avifdec_arena_required(&sizing) == 16U);

    avifdec_arena_init(&arena, memory, sizeof(memory));
    first = avifdec_arena_allocate(&arena, 3U, 1U);
    second = avifdec_arena_allocate(&arena, 8U, 8U);
    CHECK(first == memory);
    CHECK(second == memory + 8U);
    CHECK(avifdec_arena_required(&arena) == avifdec_arena_required(&sizing));
    CHECK(avifdec_arena_allocate(&arena, 64U, 1U) == 0);
    CHECK(arena.status == AVIFDEC_OUT_OF_MEMORY);
    avifdec_arena_init(&arena, memory + 1U, sizeof(memory) - 1U);
    second = avifdec_arena_allocate(&arena, 8U, 8U);
    CHECK(second != 0 && ((uintptr_t)second & 7U) == 0U);
    return 0;
}

static int test_avif_presentation_helpers(void) {
    AvifdecCleanAperture clap;
    AvifdecCropRect crop;

    avifdec_memory_fill(&clap, 0U, sizeof(clap));
    clap.width_n = 120U;
    clap.width_d = 1U;
    clap.height_n = 110U;
    clap.height_d = 1U;
    clap.horizontal_offset_n = -3;
    clap.horizontal_offset_d = 1U;
    clap.vertical_offset_n = -7;
    clap.vertical_offset_d = 1U;
    CHECK(avifdec_clap_to_crop_rect(
        &clap, 128U, 128U, &crop));
    CHECK(crop.x == 1U && crop.y == 2U &&
          crop.width == 120U && crop.height == 110U);
    clap.horizontal_offset_d = 0U;
    CHECK(!avifdec_clap_to_crop_rect(
        &clap, 128U, 128U, &crop));
    return 0;
}

static int test_avif_sample_transform(void) {
    static const unsigned char expression[] = {
        0x02U, 0x05U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U,
        0x01U, 0x82U, 0x02U, 0x80U
    };
    static const unsigned char underflow[] = {
        0x00U, 0x01U, 0x80U
    };
    static const unsigned char reserved[] = {
        0x00U, 0x01U, 0x21U
    };
    static const unsigned char trailing[] = {
        0x00U, 0x01U, 0x01U, 0x00U
    };
    AvifdecSpan spans[2];
    AvifSatoProgram program;
    AvifdecError error;
    int64_t samples[2] = { 12, 34 };
    uint16_t first[12];
    uint16_t second[12];
    uint16_t output[12];
    AvifdecImage inputs[2];
    AvifdecImage output_image;
    size_t pixel;
    int64_t result;

    spans[0].data = expression;
    spans[0].size = 6U;
    spans[0].file_offset = 100U;
    spans[1].data = expression + 6U;
    spans[1].size = sizeof(expression) - 6U;
    spans[1].file_offset = 106U;
    CHECK(avif_sato_parse(
        &program, spans, 2U, 2U, &error) == AVIFDEC_OK);
    CHECK(program.intermediate_bits == 32U &&
          program.max_stack_depth == 2U);
    CHECK(avif_sato_evaluate(
        &program, samples, 2U, 0, 65535, &result) ==
        AVIFDEC_OK);
    CHECK(result == 3106);
    avifdec_memory_fill(inputs, 0U, sizeof(inputs));
    avifdec_memory_fill(
        &output_image, 0U, sizeof(output_image));
    for (pixel = 0U; pixel < 12U; ++pixel) {
        first[pixel] = (uint16_t)(pixel + 1U);
        second[pixel] = (uint16_t)(20U + pixel);
        output[pixel] = 0xffffU;
    }
    inputs[0].planes[0] = first;
    inputs[0].strides[0] = 4U;
    inputs[0].widths[0] = 4U;
    inputs[0].heights[0] = 3U;
    inputs[1].planes[0] = second;
    inputs[1].strides[0] = 4U;
    inputs[1].widths[0] = 4U;
    inputs[1].heights[0] = 3U;
    output_image.planes[0] = output;
    output_image.strides[0] = 4U;
    CHECK(avif_sato_apply_rows(
        &program, inputs, 2U, 0U, 4U, 2U, 3U,
        0, 65535, &output_image) == AVIFDEC_OK);
    CHECK(avif_sato_apply_rows(
        &program, inputs, 2U, 0U, 4U, 0U, 2U,
        0, 65535, &output_image) == AVIFDEC_OK);
    for (pixel = 0U; pixel < 12U; ++pixel) {
        samples[0] = first[pixel];
        samples[1] = second[pixel];
        CHECK(avif_sato_evaluate(
            &program, samples, 2U, 0, 65535,
            &result) == AVIFDEC_OK);
        CHECK(output[pixel] == (uint16_t)result);
    }
    CHECK(avif_sato_apply_rows(
        &program, inputs, 2U, 0U, 4U, 3U, 2U,
        0, 65535, &output_image) ==
        AVIFDEC_INVALID_ARGUMENT);

    spans[0].data = underflow;
    spans[0].size = sizeof(underflow);
    CHECK(avif_sato_parse(
        &program, spans, 1U, 2U, &error) ==
        AVIFDEC_INVALID_DATA);
    spans[0].data = reserved;
    spans[0].size = sizeof(reserved);
    CHECK(avif_sato_parse(
        &program, spans, 1U, 2U, &error) ==
        AVIFDEC_UNSUPPORTED);
    spans[0].data = trailing;
    spans[0].size = sizeof(trailing);
    CHECK(avif_sato_parse(
        &program, spans, 1U, 1U, &error) ==
        AVIFDEC_INVALID_DATA);
    return 0;
}

static int test_avif_rgb_conversion(void) {
    uint16_t y[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
    unsigned char rgb_pixels[18];
    unsigned char rgb_row[6];
    unsigned char unspecified_pixels[3];
    unsigned char bt601_pixels[3];
    AvifdecImage image;
    AvifdecImageInfo info;
    AvifdecRgbImage rgb;
    AvifdecError error;
    size_t pixel;

    avifdec_memory_fill(&image, 0U, sizeof(image));
    avifdec_memory_fill(&info, 0U, sizeof(info));
    avifdec_memory_fill(&rgb, 0U, sizeof(rgb));
    image.planes[0] = y;
    image.strides[0] = 3U;
    image.widths[0] = 3U;
    image.heights[0] = 2U;
    image.bit_depth = 8U;
    image.monochrome = 1U;
    info.width = 3U;
    info.height = 2U;
    info.bit_depth = 8U;
    info.monochrome = 1U;
    info.channel_count = 1U;
    info.color_range = 1U;
    info.matrix_coefficients = 6U;
    info.crop.width = 3U;
    info.crop.height = 2U;
    info.presentation_width = 2U;
    info.presentation_height = 3U;
    info.transform_flags =
        AVIFDEC_TRANSFORM_IROT | AVIFDEC_TRANSFORM_IMIR;
    info.irot_angle = 1U;
    info.imir_axis = 1U;
    rgb.pixels = rgb_pixels;
    rgb.stride = 6U;
    rgb.width = 2U;
    rgb.height = 3U;
    rgb.format = AVIFDEC_RGB8;
    rgb.alpha_mode = AVIFDEC_ALPHA_STRAIGHT;
    CHECK(avifdec_image_to_rgb(
        &image, &info, &rgb, &error) == AVIFDEC_OK);
    {
        static const unsigned char expected[6] = {
            6U, 3U, 5U, 2U, 4U, 1U
        };

        for (pixel = 0U; pixel < 6U; ++pixel) {
            CHECK(rgb_pixels[3U * pixel] == expected[pixel]);
            CHECK(rgb_pixels[3U * pixel + 1U] == expected[pixel]);
            CHECK(rgb_pixels[3U * pixel + 2U] == expected[pixel]);
        }
        rgb.pixels = rgb_row;
        for (pixel = 0U; pixel < 3U; ++pixel) {
            CHECK(avifdec_image_to_rgb_row(
                &image, &info, &rgb, (uint32_t)pixel,
                &error) == AVIFDEC_OK);
            CHECK(avifdec_memory_compare(
                rgb_row, rgb_pixels + pixel * rgb.stride,
                rgb.stride) == 0);
        }
        rgb.pixels = rgb_pixels;
    }
    {
        uint16_t color_y[1] = { 128U };
        uint16_t color_u[1] = { 64U };
        uint16_t color_v[1] = { 192U };

        avifdec_memory_fill(&image, 0U, sizeof(image));
        avifdec_memory_fill(&info, 0U, sizeof(info));
        avifdec_memory_fill(&rgb, 0U, sizeof(rgb));
        image.planes[0] = color_y;
        image.planes[1] = color_u;
        image.planes[2] = color_v;
        image.strides[0] = 1U;
        image.strides[1] = 1U;
        image.strides[2] = 1U;
        image.widths[0] = 1U;
        image.widths[1] = 1U;
        image.widths[2] = 1U;
        image.heights[0] = 1U;
        image.heights[1] = 1U;
        image.heights[2] = 1U;
        image.bit_depth = 8U;
        info.width = 1U;
        info.height = 1U;
        info.bit_depth = 8U;
        info.channel_count = 3U;
        info.color_range = 1U;
        info.crop.width = 1U;
        info.crop.height = 1U;
        info.presentation_width = 1U;
        info.presentation_height = 1U;
        rgb.pixels = unspecified_pixels;
        rgb.stride = 3U;
        rgb.width = 1U;
        rgb.height = 1U;
        rgb.format = AVIFDEC_RGB8;
        info.matrix_coefficients = 2U;
        CHECK(avifdec_image_to_rgb(
            &image, &info, &rgb, &error) == AVIFDEC_OK);
        rgb.pixels = bt601_pixels;
        info.matrix_coefficients = 6U;
        CHECK(avifdec_image_to_rgb(
            &image, &info, &rgb, &error) == AVIFDEC_OK);
        CHECK(avifdec_memory_compare(
            unspecified_pixels, bt601_pixels,
            sizeof(unspecified_pixels)) == 0);
    }
    return 0;
}

typedef struct {
    unsigned char *data;
    size_t capacity;
    size_t size;
} PngTestOutput;

typedef struct {
    const unsigned char *pixels;
    size_t stride;
} PngTestRows;

static int png_test_write(void *user_data, const void *data, size_t size) {
    PngTestOutput *output = (PngTestOutput *)user_data;

    if (size > output->capacity - output->size) return -1;
    avifdec_memory_copy(output->data + output->size, data, size);
    output->size += size;
    return 0;
}

static AvifdecStatus png_test_read_row(
    void *user_data, uint32_t row, void *pixels, size_t size) {
    const PngTestRows *rows = (const PngTestRows *)user_data;

    avifdec_memory_copy(
        pixels, rows->pixels + (size_t)row * rows->stride, size);
    return AVIFDEC_OK;
}

static int test_png_writer(void) {
    static const uint16_t rgba16[4] = {
        0x0123U, 0x4567U, 0x89abU, 0xcdefU
    };
    static unsigned char wide_pixels[65535];
    static unsigned char wide_png[100000];
    static unsigned char workspace[700000];
    unsigned char png[128];
    PngTestOutput output;
    PngTestRows rows;
    size_t workspace_required;
    size_t index;

    output.data = png;
    output.capacity = sizeof(png);
    output.size = 0U;
    CHECK(avifdec_png_write(
        png_test_write, &output, rgba16, sizeof(rgba16),
        1U, 1U, 4U, 16U, 0) == AVIFDEC_OK);
    CHECK(output.size == 74U);
    CHECK(png[0] == 0x89U && png[1] == 'P' &&
          png[2] == 'N' && png[3] == 'G');
    CHECK(avifdec_load_u32be(png + 8U) == 13U);
    CHECK(png[24] == 16U && png[25] == 6U);
    CHECK(avifdec_load_u32be(png + 33U) == 17U);
    CHECK(png[41] == 0x78U && png[42] == 0x01U);
    CHECK((png[43] & 7U) == 3U);
    CHECK(png[output.size - 8U] == 'I' &&
          png[output.size - 7U] == 'E' &&
          png[output.size - 6U] == 'N' &&
          png[output.size - 5U] == 'D');
    CHECK(avifdec_png_write(
        png_test_write, &output, rgba16, 7U,
        1U, 1U, 4U, 16U, 0) == AVIFDEC_OVERFLOW);

    for (index = 0U; index < sizeof(wide_pixels); ++index) {
        wide_pixels[index] = (unsigned char)index;
    }
    output.data = wide_png;
    output.capacity = sizeof(wide_png);
    output.size = 0U;
    rows.pixels = wide_pixels;
    rows.stride = sizeof(wide_pixels);
    CHECK(avifdec_png_workspace_requirement(
        21845U, 3U, 8U, &workspace_required) == AVIFDEC_OK);
    CHECK(workspace_required <= sizeof(workspace));
    CHECK(avifdec_png_write_rows(
        png_test_write, &output, png_test_read_row, &rows,
        workspace, workspace_required - 1U,
        21845U, 1U, 3U, 8U, 0) == AVIFDEC_OUT_OF_MEMORY);
    CHECK(avifdec_png_write_rows(
        png_test_write, &output, png_test_read_row, &rows,
        workspace, workspace_required,
        21845U, 1U, 3U, 8U, 0) == AVIFDEC_OK);
    CHECK(output.size < sizeof(wide_pixels) / 4U);
    CHECK(wide_png[41] == 0x78U && wide_png[42] == 0x01U);
    CHECK((wide_png[43] & 7U) == 3U);
    {
        uint32_t random = 1U;

        for (index = 0U; index < sizeof(wide_pixels); ++index) {
            random = random * 1664525U + 1013904223U;
            wide_pixels[index] = (unsigned char)(random >> 24U);
        }
    }
    output.size = 0U;
    CHECK(avifdec_png_write_rows(
        png_test_write, &output, png_test_read_row, &rows,
        workspace, workspace_required,
        21845U, 1U, 3U, 8U, 0) == AVIFDEC_OK);
    CHECK(avifdec_load_u32be(wide_png + 33U) == 32768U);
    CHECK(wide_png[32817U] == 'I' &&
          wide_png[32818U] == 'D' &&
          wide_png[32819U] == 'A' &&
          wide_png[32820U] == 'T');
    return 0;
}

typedef struct {
    size_t count;
    size_t deepest;
} BoxTrace;

static void trace_box(const AvifdecBmffBox *box, void *user_data) {
    BoxTrace *trace = (BoxTrace *)user_data;
    ++trace->count;
    if (box->depth > trace->deepest) trace->deepest = box->depth;
}

static int test_bmff_valid(void) {
    static const unsigned char file[] = {
        0x00U, 0x00U, 0x00U, 0x14U, 'f', 't', 'y', 'p',
        'a', 'v', 'i', 'f', 0x00U, 0x00U, 0x00U, 0x00U, 'm', 'i', 'a', 'f',
        0x00U, 0x00U, 0x00U, 0x1cU, 'm', 'e', 't', 'a',
        0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x10U, 'i', 'p', 'r', 'p',
        0x00U, 0x00U, 0x00U, 0x08U, 'i', 'p', 'c', 'o',
        0x00U, 0x00U, 0x00U, 0x00U, 'm', 'd', 'a', 't'
    };
    AvifdecBmffLimits limits = { 4U, 16U };
    AvifdecBmffInfo info;
    AvifdecError error;
    BoxTrace trace = { 0U, 0U };

    CHECK(avifdec_bmff_inspect(file, sizeof(file), &limits, trace_box, &trace, &info, &error) == AVIFDEC_OK);
    CHECK(error.status == AVIFDEC_OK);
    CHECK(info.major_brand == AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    CHECK(info.compatible_brand_count == 1U);
    CHECK(info.compatible_brands[0] == AVIFDEC_FOURCC('m', 'i', 'a', 'f'));
    CHECK(info.has_avif_brand);
    CHECK(info.box_count == 5U && trace.count == 5U);
    CHECK(info.maximum_depth == 2U && trace.deepest == 2U);
    CHECK(info.meta_count == 1U && info.media_data_count == 1U);
    return 0;
}

static int test_bmff_large_uuid(void) {
    static const unsigned char file[] = {
        0x00U, 0x00U, 0x00U, 0x14U, 'f', 't', 'y', 'p',
        'm', 'i', 'f', '1', 0x00U, 0x00U, 0x00U, 0x00U, 'a', 'v', 'i', 'f',
        0x00U, 0x00U, 0x00U, 0x01U, 'u', 'u', 'i', 'd',
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U,
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU
    };
    AvifdecBmffInfo info;
    AvifdecError error;

    CHECK(avifdec_bmff_inspect(file, sizeof(file), 0, 0, 0, &info, &error) == AVIFDEC_OK);
    CHECK(info.box_count == 2U && info.has_avif_brand);
    return 0;
}

static int test_bmff_invalid(void) {
    static const unsigned char wrong_first[] = {
        0x00U, 0x00U, 0x00U, 0x08U, 'f', 'r', 'e', 'e'
    };
    static const unsigned char truncated[] = {
        0x00U, 0x00U, 0x00U, 0x20U, 'f', 't', 'y', 'p',
        'a', 'v', 'i', 'f', 0x00U, 0x00U, 0x00U, 0x00U
    };
    static const unsigned char nested[] = {
        0x00U, 0x00U, 0x00U, 0x10U, 'f', 't', 'y', 'p',
        'a', 'v', 'i', 'f', 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x1cU, 'm', 'e', 't', 'a',
        0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x10U, 'i', 'p', 'r', 'p',
        0x00U, 0x00U, 0x00U, 0x08U, 'i', 'p', 'c', 'o'
    };
    AvifdecBmffInfo info;
    AvifdecError error;
    AvifdecBmffLimits depth_limit = { 1U, 16U };
    AvifdecBmffLimits box_limit = { 8U, 2U };

    CHECK(avifdec_bmff_inspect(wrong_first, sizeof(wrong_first), 0, 0, 0, &info, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.offset == 0U);
    CHECK(avifdec_bmff_inspect(truncated, sizeof(truncated), 0, 0, 0, &info, &error) == AVIFDEC_TRUNCATED);
    CHECK(avifdec_bmff_inspect(nested, sizeof(nested), &depth_limit, 0, 0, &info, &error) == AVIFDEC_LIMIT_EXCEEDED);
    CHECK(avifdec_bmff_inspect(nested, sizeof(nested), &box_limit, 0, 0, &info, &error) == AVIFDEC_LIMIT_EXCEEDED);
    return 0;
}

static int test_bmff_mutation_sweep(void) {
    static const unsigned char valid[] = {
        0x00U, 0x00U, 0x00U, 0x14U, 'f', 't', 'y', 'p',
        'a', 'v', 'i', 'f', 0x00U, 0x00U, 0x00U, 0x00U, 'm', 'i', 'a', 'f',
        0x00U, 0x00U, 0x00U, 0x14U, 'm', 'e', 't', 'a',
        0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x08U, 'i', 'd', 'a', 't'
    };
    static const unsigned char mutations[] = { 0x00U, 0x01U, 0x7fU, 0x80U, 0xffU };
    unsigned char input[sizeof(valid)];
    AvifdecBmffLimits limits = { 8U, 64U };
    AvifdecBmffInfo info;
    AvifdecError error;
    size_t length;
    size_t offset;
    size_t mutation;

    CHECK(avifdec_bmff_inspect(valid, sizeof(valid), &limits, 0, 0, &info, &error) == AVIFDEC_OK);
    for (length = 0U; length < sizeof(valid); ++length) {
        (void)avifdec_bmff_inspect(valid, length, &limits, 0, 0, &info, &error);
    }
    for (offset = 0U; offset < sizeof(valid); ++offset) {
        for (mutation = 0U; mutation < sizeof(mutations); ++mutation) {
            avifdec_memory_copy(input, valid, sizeof(valid));
            input[offset] = mutations[mutation];
            (void)avifdec_bmff_inspect(input, sizeof(input), &limits, 0, 0, &info, &error);
        }
    }
    return 0;
}

typedef struct {
    unsigned char data[1024];
    size_t size;
    size_t iloc_extent_offset;
    size_t iloc_extent_length;
    size_t iloc_extent_offset2;
    size_t ispe_width;
    size_t av1c_profile_level;
    size_t ipma_entry_count;
    size_t ipma_property;
    size_t pixi_depth;
    size_t mdat_payload;
} QueryFixture;

static void fixture_u16(QueryFixture *fixture, uint16_t value) {
    fixture->data[fixture->size++] = (unsigned char)(value >> 8);
    fixture->data[fixture->size++] = (unsigned char)value;
}

static void fixture_u32(QueryFixture *fixture, uint32_t value) {
    fixture->data[fixture->size++] = (unsigned char)(value >> 24);
    fixture->data[fixture->size++] = (unsigned char)(value >> 16);
    fixture->data[fixture->size++] = (unsigned char)(value >> 8);
    fixture->data[fixture->size++] = (unsigned char)value;
}

static void fixture_fourcc(QueryFixture *fixture, uint32_t value) {
    fixture_u32(fixture, value);
}

static size_t fixture_box_begin(QueryFixture *fixture, uint32_t type) {
    size_t start = fixture->size;

    fixture_u32(fixture, 0U);
    fixture_fourcc(fixture, type);
    return start;
}

static void fixture_box_end(QueryFixture *fixture, size_t start) {
    uint32_t size = (uint32_t)(fixture->size - start);

    fixture->data[start] = (unsigned char)(size >> 24);
    fixture->data[start + 1U] = (unsigned char)(size >> 16);
    fixture->data[start + 2U] = (unsigned char)(size >> 8);
    fixture->data[start + 3U] = (unsigned char)size;
}

static void fixture_full_box(QueryFixture *fixture, uint8_t version, uint32_t flags) {
    fixture->data[fixture->size++] = version;
    fixture->data[fixture->size++] = (unsigned char)(flags >> 16);
    fixture->data[fixture->size++] = (unsigned char)(flags >> 8);
    fixture->data[fixture->size++] = (unsigned char)flags;
}

static void make_query_fixture(QueryFixture *fixture, int use_idat, int multiple_extents) {
    static const unsigned char av1_payload[29] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x32U, 0x13U, 0x10U, 0x00U, 0x00U, 0x00U, 0x0fU, 0xfaU,
        0x3fU, 0x5aU, 0x74U, 0x0cU, 0x7aU, 0x91U, 0x83U, 0xddU,
        0xcaU, 0x7bU, 0x36U, 0x50U, 0xb0U
    };
    size_t box;
    size_t meta;
    size_t iinf;
    size_t iprp;
    size_t ipco;

    avifdec_memory_fill(fixture, 0U, sizeof(*fixture));
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    fixture_fourcc(fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_u32(fixture, 0U);
    fixture_fourcc(fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_box_end(fixture, box);

    meta = fixture_box_begin(fixture, AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    fixture_full_box(fixture, 0U, 0U);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('h', 'd', 'l', 'r'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u32(fixture, 0U);
    fixture_fourcc(fixture, AVIFDEC_FOURCC('p', 'i', 'c', 't'));
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture->data[fixture->size++] = 0U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('p', 'i', 't', 'm'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 1U);
    fixture_box_end(fixture, box);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    fixture_full_box(fixture, use_idat ? 1U : 0U, 0U);
    fixture->data[fixture->size++] = 0x44U;
    fixture->data[fixture->size++] = 0x00U;
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 1U);
    if (use_idat) fixture_u16(fixture, 1U);
    fixture_u16(fixture, 0U);
    fixture_u16(fixture, multiple_extents ? 2U : 1U);
    fixture->iloc_extent_offset = fixture->size;
    fixture_u32(fixture, 0U);
    fixture->iloc_extent_length = fixture->size;
    fixture_u32(fixture, multiple_extents ? 10U : sizeof(av1_payload));
    if (multiple_extents) {
        fixture->iloc_extent_offset2 = fixture->size;
        fixture_u32(fixture, use_idat ? 10U : 0U);
        fixture_u32(fixture, sizeof(av1_payload) - 10U);
    }

    fixture_box_end(fixture, box);
    iinf = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'i', 'n', 'f'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 1U);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    fixture_full_box(fixture, 2U, 0U);
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 0U);
    fixture_fourcc(fixture, AVIFDEC_FOURCC('a', 'v', '0', '1'));
    fixture->data[fixture->size++] = 0U;
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, iinf);
    iprp = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'p', 'r', 'p'));
    ipco = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'p', 'c', 'o'));
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    fixture_full_box(fixture, 0U, 0U);
    fixture->ispe_width = fixture->size;
    fixture_u32(fixture, 1U);
    fixture_u32(fixture, 1U);
    fixture_box_end(fixture, box);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    fixture_full_box(fixture, 0U, 0U);
    fixture->data[fixture->size++] = 3U;
    fixture->pixi_depth = fixture->size;
    fixture->data[fixture->size++] = 8U;
    fixture->data[fixture->size++] = 8U;
    fixture->data[fixture->size++] = 8U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('a', 'v', '1', 'C'));
    fixture->data[fixture->size++] = 0x81U;
    fixture->av1c_profile_level = fixture->size;
    fixture->data[fixture->size++] = 0x20U;
    fixture->data[fixture->size++] = 0x00U;
    fixture->data[fixture->size++] = 0x00U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('t', 'e', 's', 't'));
    fixture->data[fixture->size++] = 0U;
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, ipco);
    box = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'p', 'm', 'a'));
    fixture_full_box(fixture, 0U, 0U);
    fixture->ipma_entry_count = fixture->size;
    fixture_u32(fixture, 1U);
    fixture_u16(fixture, 1U);
    fixture->data[fixture->size++] = 3U;
    fixture->data[fixture->size++] = 1U;
    fixture->data[fixture->size++] = 2U;
    fixture->ipma_property = fixture->size;
    fixture->data[fixture->size++] = 0x83U;
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, iprp);
    if (use_idat) {
        box = fixture_box_begin(fixture, AVIFDEC_FOURCC('i', 'd', 'a', 't'));
        fixture->mdat_payload = fixture->size;
        avifdec_memory_copy(fixture->data + fixture->size, av1_payload, sizeof(av1_payload));
        fixture->size += sizeof(av1_payload);
        fixture_box_end(fixture, box);
    }
    fixture_box_end(fixture, meta);
    if (!use_idat) {
        box = fixture_box_begin(fixture, AVIFDEC_FOURCC('m', 'd', 'a', 't'));
        fixture->mdat_payload = fixture->size;
        avifdec_memory_copy(fixture->data + fixture->size, av1_payload, sizeof(av1_payload));
        fixture->size += sizeof(av1_payload);
        fixture_box_end(fixture, box);
        fixture->data[fixture->iloc_extent_offset] = (unsigned char)(fixture->mdat_payload >> 24);
        fixture->data[fixture->iloc_extent_offset + 1U] = (unsigned char)(fixture->mdat_payload >> 16);
        fixture->data[fixture->iloc_extent_offset + 2U] = (unsigned char)(fixture->mdat_payload >> 8);
        fixture->data[fixture->iloc_extent_offset + 3U] = (unsigned char)fixture->mdat_payload;
        if (multiple_extents) {
            size_t second_offset = fixture->mdat_payload + 10U;

            fixture->data[fixture->iloc_extent_offset2] = (unsigned char)(second_offset >> 24);
            fixture->data[fixture->iloc_extent_offset2 + 1U] = (unsigned char)(second_offset >> 16);
            fixture->data[fixture->iloc_extent_offset2 + 2U] = (unsigned char)(second_offset >> 8);
            fixture->data[fixture->iloc_extent_offset2 + 3U] = (unsigned char)second_offset;
        }
    }
}

static void make_sato_fixture(QueryFixture *fixture) {
    static const unsigned char expression[] = {
        0x02U, 0x05U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U,
        0x01U, 0x82U, 0x02U, 0x80U
    };
    static const unsigned char av1_payload[] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x32U, 0x0eU, 0x1eU, 0x40U, 0x3fU, 0xffU, 0xffU, 0xc4U,
        0x00U, 0x00U, 0xafU, 0x28U, 0xc4U, 0x04U, 0x06U, 0x40U
    };
    size_t box;
    size_t meta;
    size_t iinf;
    size_t iref;
    size_t iprp;
    size_t ipco;
    uint16_t item_id;

    avifdec_memory_fill(fixture, 0U, sizeof(*fixture));
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_u32(fixture, 0U);
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_box_end(fixture, box);
    meta = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    fixture_full_box(fixture, 0U, 0U);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('h', 'd', 'l', 'r'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u32(fixture, 0U);
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('p', 'i', 'c', 't'));
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture->data[fixture->size++] = 0U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('p', 'i', 't', 'm'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 1U);
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'l', 'o', 'c'));
    fixture_full_box(fixture, 1U, 0U);
    fixture->data[fixture->size++] = 0x44U;
    fixture->data[fixture->size++] = 0x00U;
    fixture_u16(fixture, 3U);
    for (item_id = 1U; item_id <= 3U; ++item_id) {
        fixture_u16(fixture, item_id);
        fixture_u16(fixture, 1U);
        fixture_u16(fixture, 0U);
        fixture_u16(fixture, 1U);
        if (item_id == 1U) {
            fixture_u32(fixture, 0U);
            fixture_u32(
                fixture, (uint32_t)sizeof(expression));
        } else {
            fixture_u32(
                fixture,
                (uint32_t)(
                    sizeof(expression) +
                    (item_id - 2U) *
                        sizeof(av1_payload)));
            fixture_u32(
                fixture, (uint32_t)sizeof(av1_payload));
        }
    }
    fixture_box_end(fixture, box);
    iinf = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'i', 'n', 'f'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 3U);
    for (item_id = 1U; item_id <= 3U; ++item_id) {
        box = fixture_box_begin(
            fixture, AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
        fixture_full_box(fixture, 2U, 0U);
        fixture_u16(fixture, item_id);
        fixture_u16(fixture, 0U);
        fixture_fourcc(
            fixture, item_id == 1U
                ? AVIFDEC_FOURCC('s', 'a', 't', 'o')
                : AVIFDEC_FOURCC('a', 'v', '0', '1'));
        fixture->data[fixture->size++] = 0U;
        fixture_box_end(fixture, box);
    }
    fixture_box_end(fixture, iinf);
    iref = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'r', 'e', 'f'));
    fixture_full_box(fixture, 0U, 0U);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 2U);
    fixture_u16(fixture, 2U);
    fixture_u16(fixture, 3U);
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, iref);
    iprp = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'p', 'r', 'p'));
    ipco = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'p', 'c', 'o'));
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 's', 'p', 'e'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u32(fixture, 1U);
    fixture_u32(fixture, 1U);
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    fixture_full_box(fixture, 0U, 0U);
    fixture->data[fixture->size++] = 3U;
    fixture->data[fixture->size++] = 8U;
    fixture->data[fixture->size++] = 8U;
    fixture->data[fixture->size++] = 8U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('a', 'v', '1', 'C'));
    fixture->data[fixture->size++] = 0x81U;
    fixture->data[fixture->size++] = 0x20U;
    fixture->data[fixture->size++] = 0x00U;
    fixture->data[fixture->size++] = 0x00U;
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, ipco);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'p', 'm', 'a'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u32(fixture, 3U);
    for (item_id = 1U; item_id <= 3U; ++item_id) {
        fixture_u16(fixture, item_id);
        fixture->data[fixture->size++] =
            item_id == 1U ? 2U : 3U;
        fixture->data[fixture->size++] = 1U;
        fixture->data[fixture->size++] = 2U;
        if (item_id != 1U) {
            fixture->data[fixture->size++] = 0x83U;
        }
    }
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, iprp);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'd', 'a', 't'));
    avifdec_memory_copy(
        fixture->data + fixture->size,
        expression, sizeof(expression));
    fixture->size += sizeof(expression);
    avifdec_memory_copy(
        fixture->data + fixture->size,
        av1_payload, sizeof(av1_payload));
    fixture->size += sizeof(av1_payload);
    avifdec_memory_copy(
        fixture->data + fixture->size,
        av1_payload, sizeof(av1_payload));
    fixture->size += sizeof(av1_payload);
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, meta);
}

static void make_cycle_fixture(QueryFixture *fixture) {
    size_t box;
    size_t meta;
    size_t iinf;
    size_t iref;
    uint16_t item_id;

    avifdec_memory_fill(fixture, 0U, sizeof(*fixture));
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_u32(fixture, 0U);
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('a', 'v', 'i', 'f'));
    fixture_box_end(fixture, box);
    meta = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    fixture_full_box(fixture, 0U, 0U);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('h', 'd', 'l', 'r'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u32(fixture, 0U);
    fixture_fourcc(
        fixture, AVIFDEC_FOURCC('p', 'i', 'c', 't'));
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture_u32(fixture, 0U);
    fixture->data[fixture->size++] = 0U;
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('p', 'i', 't', 'm'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 1U);
    fixture_box_end(fixture, box);
    iinf = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'i', 'n', 'f'));
    fixture_full_box(fixture, 0U, 0U);
    fixture_u16(fixture, 2U);
    for (item_id = 1U; item_id <= 2U; ++item_id) {
        box = fixture_box_begin(
            fixture, AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
        fixture_full_box(fixture, 2U, 0U);
        fixture_u16(fixture, item_id);
        fixture_u16(fixture, 0U);
        fixture_fourcc(
            fixture, AVIFDEC_FOURCC('g', 'r', 'i', 'd'));
        fixture->data[fixture->size++] = 0U;
        fixture_box_end(fixture, box);
    }
    fixture_box_end(fixture, iinf);
    iref = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('i', 'r', 'e', 'f'));
    fixture_full_box(fixture, 0U, 0U);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 2U);
    fixture_box_end(fixture, box);
    box = fixture_box_begin(
        fixture, AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    fixture_u16(fixture, 2U);
    fixture_u16(fixture, 1U);
    fixture_u16(fixture, 1U);
    fixture_box_end(fixture, box);
    fixture_box_end(fixture, iref);
    fixture_box_end(fixture, meta);
}

static int test_avif_item_graph(void) {
    QueryFixture fixture;
    AvifdecImageInfo info;
    AvifdecError error;

    make_cycle_fixture(&fixture);
    CHECK(avifdec_query(
        fixture.data, fixture.size, 0, 0, 0U,
        &info, &error) == AVIFDEC_INVALID_DATA);
    CHECK(error.context == AVIFDEC_FOURCC('d', 'i', 'm', 'g'));
    return 0;
}

typedef struct {
    size_t calls;
    size_t count;
    size_t min_chunk;
} OutOfOrderExecutorState;

static AvifdecStatus out_of_order_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg) {
    OutOfOrderExecutorState *state =
        (OutOfOrderExecutorState *)user_data;
    AvifdecStatus tail_status;
    AvifdecStatus head_status;

    state->count = count;
    state->min_chunk = min_chunk;
    ++state->calls;
    if (count <= 1U) return body(0U, count, 0U, arg);
    tail_status = body(count - 1U, count, 3U, arg);
    head_status = body(0U, count - 1U, 0U, arg);
    return tail_status != AVIFDEC_OK
        ? tail_status : head_status;
}

static int test_avif_sample_transform_executor(void) {
    QueryFixture fixture;
    static unsigned char workspace[700000U];
    AvifdecImageInfo info;
    AvifdecImageInfo parallel_info;
    AvifdecImage serial_image;
    AvifdecImage parallel_image;
    uint16_t serial_pixels[3];
    uint16_t parallel_pixels[3];
    OutOfOrderExecutorState executor_state = { 0U, 0U, 0U };
    AvifdecExecutor executor = {
        &executor_state, 4U, out_of_order_parallel_for
    };
    AvifdecError error;
    unsigned int plane;

    make_sato_fixture(&fixture);
    CHECK(avifdec_query(
        fixture.data, fixture.size, 0, 0, 0U,
        &info, &error) == AVIFDEC_OK);
    CHECK(info.sample_transform_present &&
          info.sample_transform_input_count == 2U &&
          info.width == 1U && info.height == 1U &&
          info.workspace_required <= sizeof(workspace));
    CHECK(avifdec_query_ex(
        fixture.data, fixture.size, 0, &executor,
        0, 0U, &parallel_info, &error) == AVIFDEC_OK);
    CHECK(parallel_info.workspace_required ==
          info.workspace_required);
    avifdec_memory_fill(
        &serial_image, 0U, sizeof(serial_image));
    avifdec_memory_fill(
        &parallel_image, 0U, sizeof(parallel_image));
    for (plane = 0U; plane < 3U; ++plane) {
        serial_image.planes[plane] =
            &serial_pixels[plane];
        serial_image.strides[plane] = 1U;
        parallel_image.planes[plane] =
            &parallel_pixels[plane];
        parallel_image.strides[plane] = 1U;
    }
    CHECK(avifdec_decode(
        fixture.data, fixture.size, 0,
        workspace, sizeof(workspace),
        &serial_image, 0, &error) == AVIFDEC_OK);
    CHECK(avifdec_decode_ex(
        fixture.data, fixture.size, 0, &executor,
        workspace, sizeof(workspace),
        &parallel_image, 0, &error) == AVIFDEC_OK);
    CHECK(executor_state.calls == 1U &&
          executor_state.count == 3U &&
          executor_state.min_chunk == 1U);
    for (plane = 0U; plane < 3U; ++plane) {
        CHECK(serial_pixels[plane] ==
              parallel_pixels[plane]);
    }
    return 0;
}

static int test_avif_query_extents(void) {
    QueryFixture fixture;
    AvifdecImageInfo info;
    AvifdecSpan spans[2];
    AvifdecError error;

    make_query_fixture(&fixture, 0, 0);
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_OK);
    CHECK(error.status == AVIFDEC_OK);
    CHECK(info.primary_item_id == 1U && info.width == 1U && info.height == 1U);
    CHECK(info.profile == 1U && info.bit_depth == 8U && info.channel_count == 3U);
    CHECK(info.extent_count == 1U && info.payload_size == 29U);
    CHECK(info.workspace_required >= 2U * sizeof(Av1TileCdfs));
    CHECK(spans[0].data == fixture.data + fixture.mdat_payload && spans[0].file_offset == fixture.mdat_payload);

    make_query_fixture(&fixture, 1, 0);
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_OK);
    CHECK(info.extent_count == 1U && spans[0].data == fixture.data + fixture.mdat_payload);

    make_query_fixture(&fixture, 0, 1);
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_OK);
    CHECK(info.extent_count == 2U && info.payload_size == 29U);
    CHECK(spans[0].size == 10U && spans[1].size == 19U);
    CHECK(spans[0].data == fixture.data + fixture.mdat_payload);
    CHECK(spans[1].data == fixture.data + fixture.mdat_payload + 10U);

    make_query_fixture(&fixture, 1, 1);
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_OK);
    CHECK(info.extent_count == 2U && spans[0].size == 10U && spans[1].size == 19U);

    fixture.data[fixture.pixi_depth] = 10U;
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_INVALID_DATA);
    fixture.data[fixture.pixi_depth] = 8U;
    fixture.data[fixture.av1c_profile_level] = 0U;
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_INVALID_DATA);
    fixture.data[fixture.av1c_profile_level] = 0x20U;
    fixture.data[fixture.ispe_width + 3U] = 2U;
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_INVALID_DATA);
    fixture.data[fixture.ispe_width + 3U] = 1U;
    fixture.data[fixture.ipma_entry_count] = 0xf7U;
    fixture.data[fixture.ipma_entry_count + 1U] = 0U;
    fixture.data[fixture.ipma_entry_count + 2U] = 0U;
    fixture.data[fixture.ipma_entry_count + 3U] = 1U;
    CHECK(avifdec_query(
              fixture.data, fixture.size, 0, spans, 2U,
              &info, &error) == AVIFDEC_TRUNCATED);
    fixture.data[fixture.ipma_entry_count] = 0U;
    fixture.data[fixture.ipma_entry_count + 3U] = 1U;
    fixture.data[fixture.ipma_property] = 0x84U;
    CHECK(avifdec_query(fixture.data, fixture.size, 0, spans, 2U, &info, &error) == AVIFDEC_UNSUPPORTED);
    return 0;
}

static int test_av1_obu_errors(void) {
    static const unsigned char valid[29] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x32U, 0x13U, 0x10U, 0x00U, 0x00U, 0x00U, 0x0fU, 0xfaU,
        0x3fU, 0x5aU, 0x74U, 0x0cU, 0x7aU, 0x91U, 0x83U, 0xddU,
        0xcaU, 0x7bU, 0x36U, 0x50U, 0xb0U
    };
    static const unsigned char truncated_leb[] = { 0x12U, 0x80U };
    static const unsigned char duplicate_sequence[] = {
        0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U
    };
    static const unsigned char annex_b[31] = {
        0x1eU, 0x1dU,
        0x01U, 0x10U,
        0x05U, 0x08U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x14U, 0x30U, 0x10U, 0x00U, 0x00U, 0x00U, 0x0fU,
        0xfaU, 0x3fU, 0x5aU, 0x74U, 0x0cU, 0x7aU, 0x91U,
        0x83U, 0xddU, 0xcaU, 0x7bU, 0x36U, 0x50U, 0xb0U
    };
    static const unsigned char annex_b_bad_delimiter[31] = {
        0x1eU, 0x1dU,
        0x01U, 0x08U,
        0x05U, 0x08U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x14U, 0x30U, 0x10U, 0x00U, 0x00U, 0x00U, 0x0fU,
        0xfaU, 0x3fU, 0x5aU, 0x74U, 0x0cU, 0x7aU, 0x91U,
        0x83U, 0xddU, 0xcaU, 0x7bU, 0x36U, 0x50U, 0xb0U
    };
    static const unsigned char with_metadata[37] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x2aU, 0x06U, 0x01U, 0x00U, 0x64U, 0x00U, 0x32U, 0x80U,
        0x32U, 0x13U, 0x10U, 0x00U, 0x00U, 0x00U, 0x0fU, 0xfaU,
        0x3fU, 0x5aU, 0x74U, 0x0cU, 0x7aU, 0x91U, 0x83U, 0xddU,
        0xcaU, 0x7bU, 0x36U, 0x50U, 0xb0U
    };
    static const unsigned char truncated_metadata[] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U, 0x06U, 0x09U,
        0x2aU, 0x03U, 0x02U, 0x00U, 0x64U
    };
    static const unsigned char no_size[] = { 0x10U };
    AvifdecLimits limits;
    AvifdecSpan span;
    AvifdecImageInfo info;
    AvifdecError error;
    AvifdecEntropyTrace low_overhead_trace;
    AvifdecEntropyTrace annex_b_trace;
    static unsigned char framing_workspace[700000U];
    unsigned char mutation[sizeof(valid)];
    unsigned char padded[sizeof(valid) + 3U];
    unsigned char delimiter_payload[sizeof(valid) + 1U];
    size_t length;
    size_t offset;

    avifdec_memory_fill(&info, 0U, sizeof(info));
    info.width = 1U;
    info.height = 1U;
    info.profile = 1U;
    info.bit_depth = 8U;
    info.channel_count = 3U;
    span.file_offset = 1000U;
    span.data = valid;
    span.size = sizeof(valid);
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
    CHECK(info.obu_count == 3U && info.render_width == 1U && info.tile_columns == 1U);
    CHECK(info.tile_count == 1U && info.tile_data_size != 0U);
    CHECK(avifdec_av1_trace(
        &span, 1U, 0, &info, framing_workspace,
        sizeof(framing_workspace), &low_overhead_trace,
        &error) == AVIFDEC_OK);
    avifdec_memory_copy(padded, valid, sizeof(valid));
    padded[sizeof(valid)] = 0x7aU;
    padded[sizeof(valid) + 1U] = 0x01U;
    padded[sizeof(valid) + 2U] = 0x00U;
    span.data = padded;
    span.size = sizeof(padded);
    CHECK(avifdec_av1_query(
        &span, 1U, 0, &info, &error) == AVIFDEC_OK);
    padded[sizeof(valid) + 2U] = 0x01U;
    CHECK(avifdec_av1_query(
        &span, 1U, 0, &info, &error) ==
        AVIFDEC_INVALID_DATA);
    delimiter_payload[0] = 0x12U;
    delimiter_payload[1] = 0x01U;
    delimiter_payload[2] = 0x00U;
    avifdec_memory_copy(
        delimiter_payload + 3U, valid + 2U,
        sizeof(valid) - 2U);
    span.data = delimiter_payload;
    span.size = sizeof(delimiter_payload);
    CHECK(avifdec_av1_query(
        &span, 1U, 0, &info, &error) ==
        AVIFDEC_INVALID_DATA);

    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    limits.av1_framing = AVIFDEC_AV1_ANNEX_B;
    span.data = annex_b;
    span.size = sizeof(annex_b);
    info.obu_count = 0U;
    info.tile_count = 0U;
    info.tile_data_size = 0U;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) == AVIFDEC_OK);
    CHECK(info.obu_count == 3U && info.tile_count == 1U);
    CHECK(avifdec_av1_trace(
        &span, 1U, &limits, &info, framing_workspace,
        sizeof(framing_workspace), &annex_b_trace,
        &error) == AVIFDEC_OK);
    CHECK(annex_b_trace.checksum == low_overhead_trace.checksum &&
          annex_b_trace.restoration_checksum ==
              low_overhead_trace.restoration_checksum &&
          annex_b_trace.reference_state_checksum ==
              low_overhead_trace.reference_state_checksum);
    span.data = annex_b_bad_delimiter;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) ==
        AVIFDEC_INVALID_DATA);
    limits.av1_framing = 2U;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) ==
        AVIFDEC_INVALID_ARGUMENT);

    span.data = with_metadata;
    span.size = sizeof(with_metadata);
    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    info.obu_count = 0U;
    info.metadata_obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
    CHECK(info.obu_count == 4U && info.metadata_obu_count == 1U);
    CHECK(info.hdr_cll.max_cll == 100U &&
          info.hdr_cll.max_fall == 50U);
    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    limits.max_obus = 2U;
    info.obu_count = 0U;
    info.metadata_obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, &limits, &info, &error) == AVIFDEC_LIMIT_EXCEEDED);

    span.data = truncated_metadata;
    span.size = sizeof(truncated_metadata);
    info.obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_TRUNCATED);
    span.data = valid;
    span.size = sizeof(valid);
    info.obu_count = 0U;
    info.has_nclx = 1U;
    info.color_primaries = 9U;
        info.transfer_characteristics = 16U;
        info.matrix_coefficients = 9U;
        info.color_range = 1U;
        CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
        CHECK(info.color_primaries == 9U && info.transfer_characteristics == 16U &&
            info.matrix_coefficients == 9U && info.color_range == 1U);
    info.has_nclx = 0U;

    span.data = truncated_leb;
    span.size = sizeof(truncated_leb);
    info.obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_TRUNCATED);
    span.data = duplicate_sequence;
    span.size = sizeof(duplicate_sequence);
    info.obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_INVALID_DATA);
    span.data = no_size;
    span.size = sizeof(no_size);
    info.obu_count = 0U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_INVALID_DATA);

    for (length = 0U; length < sizeof(valid); ++length) {
        span.data = valid;
        span.size = length;
        info.obu_count = 0U;
        (void)avifdec_av1_query(&span, 1U, 0, &info, &error);
    }
    for (offset = 0U; offset < sizeof(valid); ++offset) {
        avifdec_memory_copy(mutation, valid, sizeof(valid));
        mutation[offset] ^= 0xffU;
        span.data = mutation;
        span.size = sizeof(mutation);
        info.obu_count = 0U;
        (void)avifdec_av1_query(&span, 1U, 0, &info, &error);
    }
    return 0;
}

static int test_av1_regular_header(void) {
    static const unsigned char regular[48] = {
        0x12U, 0x00U, 0x0aU, 0x08U, 0x30U, 0x00U, 0x00U, 0x00U,
        0x06U, 0x6dU, 0x7cU, 0xc1U, 0x32U, 0x22U, 0x10U, 0x00U,
        0xa8U, 0x00U, 0x00U, 0x02U, 0x4bU, 0x80U, 0x02U, 0x7dU,
        0x54U, 0x6fU, 0x30U, 0x70U, 0xf2U, 0x89U, 0x6bU, 0x10U,
        0x55U, 0x94U, 0x29U, 0x13U, 0xf6U, 0x46U, 0x34U, 0xf2U,
        0x6cU, 0x82U, 0x25U, 0x5cU, 0xbbU, 0x7dU, 0x65U, 0x18U
    };
    static const unsigned char layered[52] = {
        0x12U, 0x00U, 0x0aU, 0x08U, 0x30U, 0x01U, 0x01U, 0x00U,
        0x06U, 0x6dU, 0x7cU, 0xc1U,
        0x2eU, 0x20U, 0x01U, 0x04U,
        0x32U, 0x22U, 0x10U, 0x00U, 0xa8U, 0x00U, 0x00U, 0x02U,
        0x4bU, 0x80U, 0x02U, 0x7dU, 0x54U, 0x6fU, 0x30U, 0x70U,
        0xf2U, 0x89U, 0x6bU, 0x10U, 0x55U, 0x94U, 0x29U, 0x13U,
        0xf6U, 0x46U, 0x34U, 0xf2U, 0x6cU, 0x82U, 0x25U, 0x5cU,
        0xbbU, 0x7dU, 0x65U, 0x18U
    };
    static const unsigned char timed[57] = {
        0x12U, 0x00U,
        0x0aU, 0x11U, 0x34U, 0x00U, 0x00U, 0x00U, 0x04U,
        0x00U, 0x00U, 0x00U, 0xf3U, 0x00U, 0x00U, 0x00U, 0x00U,
        0xcdU, 0xafU, 0x98U, 0x20U,
        0x32U, 0x22U, 0x10U, 0x00U, 0xa8U, 0x00U, 0x00U, 0x02U,
        0x4bU, 0x80U, 0x02U, 0x7dU, 0x54U, 0x6fU, 0x30U, 0x70U,
        0xf2U, 0x89U, 0x6bU, 0x10U, 0x55U, 0x94U, 0x29U, 0x13U,
        0xf6U, 0x46U, 0x34U, 0xf2U, 0x6cU, 0x82U, 0x25U, 0x5cU,
        0xbbU, 0x7dU, 0x65U, 0x18U
    };
    static unsigned char selected_layer[sizeof(layered)];
    static unsigned char workspace[700000U];
    AvifdecSpan span = { regular, sizeof(regular), 2000U };
    AvifdecLimits limits;
    AvifdecEntropyTrace trace;
    AvifdecImageInfo info;
    AvifdecError error;

    avifdec_memory_fill(&info, 0U, sizeof(info));
    info.width = 2U;
    info.height = 2U;
    info.profile = 1U;
    info.bit_depth = 8U;
    info.channel_count = 3U;
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
    CHECK(info.reduced_still_picture_header == 0U);
    CHECK(info.workspace_plane_buffer_count == 13U &&
          info.workspace_required <= sizeof(workspace));
    CHECK(info.frame_type == 0U && info.render_width == 2U && info.render_height == 2U);
    CHECK(info.tile_columns == 1U && info.tile_rows == 1U && info.obu_count == 3U);
    CHECK(avifdec_av1_trace(&span, 1U, 0, &info, workspace,
                            info.workspace_required,
                            &trace, &error) == AVIFDEC_OK);
    CHECK(trace.tile_count == 1U && trace.partition_nodes == 4U &&
          trace.block_count == 1U && trace.transform_count == 6U &&
          trace.nonzero_transform_count == 3U &&
          trace.coefficient_count == 33U &&
          trace.checksum == 0x07108a861b12bc01ULL);
    CHECK(trace.deblocked_checksum != trace.cdef_checksum &&
          trace.cdef_checksum == trace.superres_checksum &&
          trace.superres_checksum == trace.restoration_checksum);
    span.data = layered;
    span.size = sizeof(layered);
    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    avifdec_memory_fill(&info, 0U, sizeof(info));
    info.width = 2U;
    info.height = 2U;
    info.profile = 1U;
    info.bit_depth = 8U;
    info.channel_count = 3U;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) == AVIFDEC_OK);
    CHECK(info.operating_point == 0U &&
          info.operating_point_count == 1U &&
          info.operating_point_idc == 0x101U &&
          info.metadata_obu_count == 0U);
    avifdec_memory_copy(
        selected_layer, layered, sizeof(selected_layer));
    selected_layer[13] = 0x00U;
    span.data = selected_layer;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) ==
        AVIFDEC_TRUNCATED);
    limits.operating_point = 1U;
    span.data = layered;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) ==
        AVIFDEC_INVALID_ARGUMENT);
    span.data = timed;
    span.size = sizeof(timed);
    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    avifdec_memory_fill(&info, 0U, sizeof(info));
    info.width = 2U;
    info.height = 2U;
    info.profile = 1U;
    info.bit_depth = 8U;
    info.channel_count = 3U;
    CHECK(avifdec_av1_query(
        &span, 1U, &limits, &info, &error) == AVIFDEC_OK);
    CHECK(info.timing_info_present == 1U &&
          info.equal_picture_interval == 1U &&
          info.num_units_in_display_tick == 1U &&
          info.time_scale == 60U &&
          info.num_ticks_per_picture_minus_1 == 0U);
    return 0;
}

static void init_av1_info(AvifdecImageInfo *info,
                          uint8_t profile,
                          uint8_t bit_depth,
                          uint8_t monochrome,
                          uint8_t subsampling_x,
                          uint8_t subsampling_y) {
    avifdec_memory_fill(info, 0U, sizeof(*info));
    info->width = 2U;
    info->height = 2U;
    info->profile = profile;
    info->bit_depth = bit_depth;
    info->monochrome = monochrome;
    info->subsampling_x = subsampling_x;
    info->subsampling_y = subsampling_y;
    info->channel_count = monochrome ? 1U : 3U;
}

static int test_av1_format_matrix(void) {
    static const unsigned char yuv420_10[44] = {
        0x12U, 0x00U, 0x0aU, 0x05U, 0x18U, 0x00U, 0x36U, 0xe0U,
        0x20U, 0x32U, 0x21U, 0x15U, 0x00U, 0x00U, 0x00U, 0x49U,
        0x70U, 0x00U, 0x40U, 0x7dU, 0x55U, 0x7bU, 0x06U, 0x27U,
        0x49U, 0x0fU, 0xdeU, 0x3aU, 0x72U, 0x88U, 0xadU, 0x25U,
        0x4fU, 0xd9U, 0xf1U, 0x0dU, 0x63U, 0xf0U, 0xf8U, 0xf9U,
        0x4eU, 0xd1U, 0xdeU, 0xa8U
    };
    static const unsigned char monochrome[41] = {
        0x12U, 0x00U, 0x0aU, 0x04U, 0x18U, 0x00U, 0x36U, 0xd1U,
        0x32U, 0x1fU, 0x15U, 0x00U, 0x00U, 0x01U, 0x25U, 0xc4U,
        0x7fU, 0x3aU, 0x4cU, 0xf5U, 0x64U, 0x14U, 0x99U, 0x12U,
        0xf6U, 0xa1U, 0x49U, 0x2cU, 0xb6U, 0x77U, 0x3bU, 0x4cU,
        0x4bU, 0x51U, 0xb0U, 0x3aU, 0xc7U, 0x66U, 0xabU, 0x15U,
        0x60U
    };
    static const unsigned char yuv420_12[46] = {
        0x12U, 0x00U, 0x0aU, 0x05U, 0x58U, 0x00U, 0x36U, 0xf1U,
        0x84U, 0x32U, 0x23U, 0x15U, 0x00U, 0x00U, 0x00U, 0x49U,
        0x70U, 0x00U, 0x40U, 0x7dU, 0x56U, 0x2fU, 0x5aU, 0xdbU,
        0x94U, 0xbcU, 0xb4U, 0xfeU, 0x3eU, 0x19U, 0x04U, 0x10U,
        0x3eU, 0x4fU, 0x6eU, 0x00U, 0xffU, 0xe0U, 0x1aU, 0x9eU,
        0xddU, 0x8cU, 0x08U, 0xa7U, 0x3dU, 0x40U
    };
    static const unsigned char yuv422[39] = {
        0x12U, 0x00U, 0x0aU, 0x05U, 0x58U, 0x00U, 0x36U, 0xc0U,
        0x80U, 0x32U, 0x1cU, 0x15U, 0x00U, 0x00U, 0x00U, 0x49U,
        0x70U, 0x00U, 0x40U, 0x12U, 0x81U, 0x84U, 0xe9U, 0x12U,
        0x17U, 0x88U, 0x26U, 0x70U, 0xd4U, 0x6aU, 0x21U, 0x63U,
        0xd3U, 0xc4U, 0xb3U, 0x47U, 0x04U, 0x2aU, 0x40U
    };
    AvifdecSpan span;
    AvifdecImageInfo info;
    AvifdecError error;

    span.file_offset = 3000U;
    span.data = yuv420_10;
    span.size = sizeof(yuv420_10);
    init_av1_info(&info, 0U, 10U, 0U, 1U, 1U);
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);

    span.data = monochrome;
    span.size = sizeof(monochrome);
    init_av1_info(&info, 0U, 8U, 1U, 1U, 1U);
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);

    span.data = yuv420_12;
    span.size = sizeof(yuv420_12);
    init_av1_info(&info, 2U, 12U, 0U, 1U, 1U);
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);

    span.data = yuv422;
    span.size = sizeof(yuv422);
    init_av1_info(&info, 2U, 8U, 0U, 1U, 0U);
    CHECK(avifdec_av1_query(&span, 1U, 0, &info, &error) == AVIFDEC_OK);
    return 0;
}

static int test_av1_symbol_decoder(void) {
    static const unsigned char one_data[2] = { 0xc0U, 0x00U };
    static const unsigned char zero_data[2] = { 0x20U, 0x00U };
    static const unsigned char bad_tail[2] = { 0xc0U, 0x01U };
    static const unsigned char equivalence_data[16] = {
        0x6dU, 0xa3U, 0x17U, 0xc8U, 0x52U, 0x99U, 0x04U, 0xeeU,
        0x31U, 0x7bU, 0xd0U, 0x48U, 0xa5U, 0x16U, 0x80U, 0x00U
    };
    AvifdecSpan spans[3];
    Av1SymbolDecoder decoder;
    Av1SymbolDecoder split_decoder;
    uint16_t cdf[3];

    spans[0].data = one_data;
    spans[0].size = 1U;
    spans[0].file_offset = 0U;
    spans[1].data = one_data + 1U;
    spans[1].size = 1U;
    spans[1].file_offset = 1U;
    cdf[0] = 16384U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    CHECK(av1_symbol_init(&decoder, spans, 2U, 0U, 2U, 0) == AVIFDEC_OK);
    CHECK(av1_symbol_read(&decoder, cdf, 2U) == 1U);
    CHECK(cdf[0] == 15360U && cdf[1] == 32768U && cdf[2] == 1U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_OK);

    spans[0].data = zero_data;
    spans[0].size = sizeof(zero_data);
    cdf[0] = 16384U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    CHECK(av1_symbol_init(&decoder, spans, 1U, 0U, 2U, 0) == AVIFDEC_OK);
    CHECK(av1_symbol_read(&decoder, cdf, 2U) == 0U);
    CHECK(cdf[0] == 17408U && cdf[1] == 32768U && cdf[2] == 1U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_OK);

    spans[0].data = bad_tail;
    CHECK(av1_symbol_init(&decoder, spans, 1U, 0U, 2U, 1) == AVIFDEC_OK);
    CHECK(av1_symbol_read_literal(&decoder, 1U) == 1U);
    CHECK(av1_symbol_exit(&decoder) == AVIFDEC_INVALID_DATA);

    spans[0].data = equivalence_data;
    spans[0].size = sizeof(equivalence_data);
    spans[0].file_offset = 0U;
    CHECK(av1_symbol_init(
              &decoder, spans, 1U, 0U, sizeof(equivalence_data), 0) ==
          AVIFDEC_OK);
    spans[0].size = 3U;
    spans[1].data = equivalence_data + 3U;
    spans[1].size = 4U;
    spans[1].file_offset = 3U;
    spans[2].data = equivalence_data + 7U;
    spans[2].size = sizeof(equivalence_data) - 7U;
    spans[2].file_offset = 7U;
    CHECK(av1_symbol_init(
              &split_decoder, spans, 3U, 0U,
              sizeof(equivalence_data), 0) == AVIFDEC_OK);
    CHECK(decoder.contiguous_data != 0 &&
          split_decoder.contiguous_data == 0);
    cdf[0] = 9000U;
    cdf[1] = 32768U;
    cdf[2] = 0U;
    {
        uint16_t split_cdf[3] = { 9000U, 32768U, 0U };
        unsigned int index;

        for (index = 0U; index < 8U; ++index) {
            unsigned int bits = index % 5U + 1U;

            CHECK(av1_symbol_read_literal(&decoder, bits) ==
                  av1_symbol_read_literal(&split_decoder, bits));
            CHECK(av1_symbol_read(&decoder, cdf, 2U) ==
                  av1_symbol_read(&split_decoder, split_cdf, 2U));
            CHECK(cdf[0] == split_cdf[0] &&
                  cdf[1] == split_cdf[1] &&
                  cdf[2] == split_cdf[2]);
        }
    }
    CHECK(av1_symbol_exit(&decoder) ==
          av1_symbol_exit(&split_decoder));
    return 0;
}

static void force_cdf_symbol(uint16_t *cdf,
                             size_t symbols,
                             size_t forced_symbol) {
    size_t index;

    for (index = 0U; index < symbols; ++index) {
        cdf[index] = index < forced_symbol ? 0U : 32768U;
    }
    cdf[symbols] = 0U;
}

static int test_av1_coeff_cdfs(void) {
    Av1CoeffCdfs cdfs;
    Av1CoeffCdfs repeated;
    uint64_t checksum;

    CHECK(av1_coeff_q_context(0U) == 0U);
    CHECK(av1_coeff_q_context(20U) == 0U);
    CHECK(av1_coeff_q_context(21U) == 1U);
    CHECK(av1_coeff_q_context(60U) == 1U);
    CHECK(av1_coeff_q_context(61U) == 2U);
    CHECK(av1_coeff_q_context(120U) == 2U);
    CHECK(av1_coeff_q_context(121U) == 3U);
    CHECK(av1_coeff_q_context(255U) == 3U);

    av1_coeff_cdfs_init(&cdfs, 0U);
    CHECK(cdfs.txb_skip[0][0][0] == 31849U);
    CHECK(cdfs.eob_pt_16[0][0][0] == 840U);
    CHECK(cdfs.dc_sign[1][2][0] == 128U * 135U);
    CHECK(cdfs.coeff_base_eob[0][0][0][0] == 17837U);
    CHECK(cdfs.coeff_base[0][0][0][0] == 4034U);
    CHECK(cdfs.coeff_br[0][0][0][0] == 14298U);
    checksum = av1_coeff_cdfs_checksum(&cdfs);
    av1_coeff_cdfs_init(&repeated, 20U);
    CHECK(checksum == av1_coeff_cdfs_checksum(&repeated));
    repeated.coeff_base[0][0][0][0] = 0U;
    CHECK(checksum != av1_coeff_cdfs_checksum(&repeated));
    av1_coeff_cdfs_init(&repeated, 121U);
    CHECK(checksum != av1_coeff_cdfs_checksum(&repeated));
    CHECK(repeated.eob_pt_16[0][0][0] == 6708U);
    return 0;
}

    static int test_av1_coeff_parser(void) {
        static const unsigned char data[64] = {
          0x40U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
        };
                static const unsigned char golomb_limit_data[64] = {
                    0x00U, 0xe4U, 0U, 0U, 0U, 0U, 0U, 0U
                };
        static Av1CoeffCdfs cdfs;
        static uint8_t above_level[4];
        static uint8_t left_level[4];
        static uint8_t above_dc[4];
        static uint8_t left_dc[4];
        Av1CoeffPlaneContext planes[3];
        Av1CoeffContextState contexts;
        Av1CoeffBlockResult result;
        int32_t coefficients[16];
        Av1SymbolDecoder decoder;
        AvifdecSpan span;

        CHECK(av1_tx_size_info[AV1_TX_4X4].width == 4U &&
            av1_tx_size_info[AV1_TX_4X4].height == 4U &&
            av1_tx_size_info[AV1_TX_64X16].width == 64U &&
            av1_tx_size_info[AV1_TX_64X16].height == 16U);
        avifdec_memory_fill(planes, 0U, sizeof(planes));
        planes[0].above_level_context = above_level;
        planes[0].left_level_context = left_level;
        planes[0].above_dc_context = above_dc;
        planes[0].left_dc_context = left_dc;
        planes[0].above_capacity = sizeof(above_level);
        planes[0].left_capacity = sizeof(left_level);
        planes[0].width4 = 4U;
        planes[0].height4 = 4U;
        span.data = data;
        span.size = sizeof(data);
        span.file_offset = 0U;

        planes[0].above_capacity = 3U;
        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_INVALID_ARGUMENT);
        planes[0].above_capacity = sizeof(above_level);

        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        above_level[0] = 9U;
        left_level[0] = 9U;
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[0][0], 2U, 1U);
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_4X4, AV1_TX_DCT_DCT, 4U, 4U,
                          0U, 0U, &result) == AVIFDEC_OK);
        CHECK(result.eob == 0U && result.cul_level == 0U &&
            result.dc_category == 0U);
        CHECK(above_level[0] == 0U && left_level[0] == 0U &&
            above_dc[0] == 0U && left_dc[0] == 0U);
        CHECK(cdfs.txb_skip[0][0][2] == 1U);

        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[0][0], 2U, 0U);
        force_cdf_symbol(cdfs.eob_pt_16[0][0], 5U, 0U);
        force_cdf_symbol(cdfs.coeff_base_eob[0][0][0], 3U, 0U);
        force_cdf_symbol(cdfs.dc_sign[0][0], 2U, 1U);
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block_values(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_4X4, AV1_TX_DCT_DCT, 4U, 4U,
                          0U, 0U, coefficients, 16U, &result) == AVIFDEC_OK);
        CHECK(result.eob == 1U && result.cul_level == 1U &&
            result.dc_category == 1U && coefficients[0] == -1 &&
            coefficients[1] == 0);
        CHECK(above_level[0] == 1U && left_level[0] == 1U &&
            above_dc[0] == 1U && left_dc[0] == 1U);
        CHECK(cdfs.txb_skip[0][0][2] == 1U &&
            cdfs.eob_pt_16[0][0][5] == 1U &&
            cdfs.coeff_base_eob[0][0][0][3] == 1U &&
            cdfs.dc_sign[0][0][2] == 1U);

        planes[0].width4 = 3U;
        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[1][0], 2U, 0U);
        force_cdf_symbol(cdfs.eob_pt_64[0][0], 7U, 0U);
        force_cdf_symbol(cdfs.coeff_base_eob[1][0][0], 3U, 0U);
        force_cdf_symbol(cdfs.dc_sign[0][0], 2U, 0U);
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_8X8, AV1_TX_DCT_DCT, 8U, 8U,
                          2U, 0U, &result) == AVIFDEC_OK);
        CHECK(result.eob == 1U && result.dc_category == 2U &&
            above_level[2] == 1U && above_dc[2] == 2U &&
            above_level[3] == 0U && above_dc[3] == 0U);
        planes[0].width4 = 4U;

        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        left_level[0] = 4U;
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[0][3], 2U, 1U);
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_4X4, AV1_TX_DCT_DCT, 8U, 4U,
                          0U, 0U, &result) == AVIFDEC_OK);
        CHECK(cdfs.txb_skip[0][3][2] == 1U &&
            cdfs.txb_skip[0][2][2] == 0U);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_8X8, AV1_TX_DCT_DCT, 8U, 8U,
                          3U, 3U, &result) == AVIFDEC_INVALID_ARGUMENT);

        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[0][0], 2U, 0U);
        force_cdf_symbol(cdfs.eob_pt_16[0][0], 5U, 0U);
        force_cdf_symbol(cdfs.coeff_base_eob[0][0][0], 3U, 2U);
        force_cdf_symbol(cdfs.coeff_br[0][0][0], 4U, 3U);
        force_cdf_symbol(cdfs.dc_sign[0][0], 2U, 0U);
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_4X4, AV1_TX_DCT_DCT, 4U, 4U,
                          0U, 0U, &result) == AVIFDEC_OK);
        CHECK(result.eob == 1U && result.cul_level == 21U &&
            result.dc_category == 2U);
        CHECK(above_level[0] == 21U && left_level[0] == 21U &&
            above_dc[0] == 2U && left_dc[0] == 2U);
        CHECK(cdfs.coeff_br[0][0][0][4] == 4U);

        CHECK(av1_coeff_context_init(&contexts, planes) == AVIFDEC_OK);
        av1_coeff_cdfs_init(&cdfs, 0U);
        force_cdf_symbol(cdfs.txb_skip[0][0], 2U, 0U);
        force_cdf_symbol(cdfs.eob_pt_16[0][0], 5U, 0U);
        force_cdf_symbol(cdfs.coeff_base_eob[0][0][0], 3U, 2U);
        force_cdf_symbol(cdfs.coeff_br[0][0][0], 4U, 3U);
        force_cdf_symbol(cdfs.dc_sign[0][0], 2U, 0U);
        span.data = golomb_limit_data;
        CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
        CHECK(av1_coeff_parse_block(&decoder, &cdfs, &contexts, 0U,
                          AV1_TX_4X4, AV1_TX_DCT_DCT, 4U, 4U,
                          0U, 0U, &result) == AVIFDEC_INVALID_DATA);
        CHECK(decoder.status == AVIFDEC_INVALID_DATA && result.eob == 0U &&
            result.cul_level == 0U && result.dc_category == 0U);
        CHECK(cdfs.coeff_br[0][0][0][4] == 4U &&
            cdfs.dc_sign[0][0][2] == 1U && decoder.bit_position == 35U);
        CHECK(above_level[0] == 0U && left_level[0] == 0U &&
            above_dc[0] == 0U && left_dc[0] == 0U);
        return 0;
    }

static int test_av1_intra_cdfs(void) {
    Av1IntraCdfs cdfs;
    Av1IntraCdfs repeated;
    uint64_t checksum;

    av1_intra_cdfs_init(&cdfs);
    CHECK(cdfs.y_mode[0][0][0] == 15588U);
    CHECK(cdfs.uv_mode_cfl_not_allowed[0][0] == 22631U);
    CHECK(cdfs.uv_mode_cfl_allowed[0][0] == 10407U);
    CHECK(cdfs.angle_delta[0][0] == 2180U);
    CHECK(cdfs.filter_intra_mode[0] == 8949U);
    CHECK(cdfs.filter_intra[0][0] == 4621U);
    CHECK(cdfs.tx_type_set1[0][0][0] == 1535U);
    CHECK(cdfs.tx_type_set2[0][0][0] == 6554U);
    CHECK(cdfs.cfl_sign[0] == 1418U);
    CHECK(cdfs.cfl_alpha[0][0] == 7637U);
    checksum = av1_intra_cdfs_checksum(&cdfs);
    av1_intra_cdfs_init(&repeated);
    CHECK(checksum == av1_intra_cdfs_checksum(&repeated));
    repeated.y_mode[0][0][0] = 0U;
    CHECK(checksum != av1_intra_cdfs_checksum(&repeated));
    return 0;
}

static int test_av1_dequantization(void) {
    static const uint64_t qmatrix_checksums[15][2] = {
        { 0x730ce42d6021974eULL, 0x8b1321bf21de68b6ULL },
        { 0x2e74e857158f7fa0ULL, 0x1bf5ed1bb3daec24ULL },
        { 0x050ffea88c149c63ULL, 0xdd7b20fb50aa2ba4ULL },
        { 0x3a7dcfdff3882339ULL, 0x376322fcf954a6d4ULL },
        { 0xd77b200356a6101eULL, 0x589518744f4a6421ULL },
        { 0xe3d47fd034e7ee85ULL, 0x8125c27b2684167fULL },
        { 0xd78c8a89f53f66fcULL, 0x8c9cc51490f6c6aaULL },
        { 0xf705f88c63149b0fULL, 0x9b72d1a736e534a2ULL },
        { 0xb256559b099e6bd3ULL, 0x079bbb9e6e3b9f19ULL },
        { 0x4e4a67a768129189ULL, 0x0dca801762ec8ba8ULL },
        { 0x04a7f4484b475b49ULL, 0x94b4a5a72744369bULL },
        { 0x00e5ef24f850d295ULL, 0x3b98324b71fd71e4ULL },
        { 0x01fab794ac27ed71ULL, 0x8af5fb952a0d00e1ULL },
        { 0x368160674f59dedfULL, 0x01097afa4634cd66ULL },
        { 0x27fbe657f0026b32ULL, 0xf4c16d6b8619ed06ULL }
    };
    int32_t quantized[1024];
    int32_t dequantized[1024];
    int32_t reference[16];
    uint8_t qmatrix[AV1_QM_TOTAL_SIZE];
    Av1DequantParams params;
    unsigned int level;
    unsigned int chroma;

    CHECK(av1_recon_dc_quant(8U, 0) == 4U);
    CHECK(av1_recon_dc_quant(10U, 255) == 5347U);
    CHECK(av1_recon_dc_quant(12U, 255) == 21387U);
    CHECK(av1_recon_ac_quant(8U, 255) == 1828U);
    CHECK(av1_recon_ac_quant(10U, 255) == 7312U);
    CHECK(av1_recon_ac_quant(12U, 255) == 29247U);
    for (level = 0U; level < 15U; ++level) {
        for (chroma = 0U; chroma < 2U; ++chroma) {
            uint64_t checksum = (uint64_t)1469598103934665603ULL;
            size_t index;

            CHECK(av1_recon_qmatrix_decode(
                      (uint8_t)level, (uint8_t)chroma,
                      qmatrix, sizeof(qmatrix)) == AVIFDEC_OK);
            for (index = 0U; index < sizeof(qmatrix); ++index) {
                checksum ^= qmatrix[index];
                checksum *= (uint64_t)1099511628211ULL;
            }
            CHECK(checksum == qmatrix_checksums[level][chroma]);
        }
    }
    CHECK(av1_recon_qmatrix_decode(
              15U, 0U, qmatrix, sizeof(qmatrix)) ==
          AVIFDEC_INVALID_ARGUMENT);
    avifdec_memory_fill(&params, 0U, sizeof(params));
    params.bit_depth = 8U;
    quantized[0] = -1;
    quantized[1] = 2;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_OK);
    CHECK(dequantized[0] == -4 && dequantized[1] == 8);

    avifdec_memory_fill(quantized, 0U, sizeof(quantized));
    params.bit_depth = 12U;
    params.q_index = 255U;
    quantized[0] = 1048575;
    CHECK(av1_recon_dequantize(quantized, 1024U, AV1_TX_64X64,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 1024U) == AVIFDEC_OK);
    CHECK(dequantized[0] <= 524287 && dequantized[0] >= -524288);
    CHECK(av1_recon_dequantize(quantized, 15U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_INVALID_ARGUMENT);
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 15U) == AVIFDEC_INVALID_ARGUMENT);

    params.bit_depth = 10U;
    params.q_index = 73U;
    params.qm_level = 0U;
    for (level = 0U; level < 16U; ++level) {
        quantized[level] = (int32_t)level - 8;
    }
    params.using_qmatrix = 0U;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               reference, 16U) == AVIFDEC_OK);
    avifdec_memory_fill(qmatrix, 32U, sizeof(qmatrix));
    params.using_qmatrix = 1U;
    params.qmatrix = qmatrix;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_OK);
    CHECK(avifdec_memory_compare(
              reference, dequantized, sizeof(reference)) == 0);

    CHECK(av1_recon_qmatrix_decode(
              6U, 0U, qmatrix, sizeof(qmatrix)) == AVIFDEC_OK);
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_OK);
    for (level = 0U; level < 16U; ++level) {
        uint32_t step;

        CHECK(av1_recon_quant_step(
                  &params, AV1_TX_4X4, AV1_TX_DCT_DCT,
                  level, &step) == AVIFDEC_OK);
        CHECK(dequantized[level] == quantized[level] * (int32_t)step);
    }
    params.qmatrix = 0;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_INVALID_DATA);
    params.qm_level = 15U;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_DCT_DCT, &params,
                               dequantized, 16U) == AVIFDEC_OK);
    params.qm_level = 0U;
    CHECK(av1_recon_dequantize(quantized, 16U, AV1_TX_4X4,
                               AV1_TX_IDTX, &params,
                               dequantized, 16U) == AVIFDEC_OK);
    return 0;
}

static void test_hash_i32(uint64_t *checksum, int32_t value) {
    unsigned int byte;
    for (byte = 0U; byte < 4U; ++byte) {
        *checksum ^= (uint8_t)((uint32_t)value >> (byte * 8U));
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

static int test_tx_type_has_long_adst(Av1TxType tx_type,
                                      unsigned int width_log2,
                                      unsigned int height_log2) {
    int row_adst = tx_type == AV1_TX_DCT_ADST ||
        tx_type == AV1_TX_ADST_ADST || tx_type == AV1_TX_DCT_FLIPADST ||
        tx_type == AV1_TX_FLIPADST_FLIPADST ||
        tx_type == AV1_TX_ADST_FLIPADST ||
        tx_type == AV1_TX_FLIPADST_ADST || tx_type == AV1_TX_H_ADST ||
        tx_type == AV1_TX_H_FLIPADST;
    int column_adst = tx_type == AV1_TX_ADST_DCT ||
        tx_type == AV1_TX_ADST_ADST || tx_type == AV1_TX_FLIPADST_DCT ||
        tx_type == AV1_TX_FLIPADST_FLIPADST ||
        tx_type == AV1_TX_ADST_FLIPADST ||
        tx_type == AV1_TX_FLIPADST_ADST || tx_type == AV1_TX_V_ADST ||
        tx_type == AV1_TX_V_FLIPADST;
    int row_identity = tx_type == AV1_TX_IDTX || tx_type == AV1_TX_V_DCT ||
        tx_type == AV1_TX_V_ADST || tx_type == AV1_TX_V_FLIPADST;
    int column_identity = tx_type == AV1_TX_IDTX || tx_type == AV1_TX_H_DCT ||
        tx_type == AV1_TX_H_ADST || tx_type == AV1_TX_H_FLIPADST;
    return (row_adst && width_log2 > 4U) ||
           (column_adst && height_log2 > 4U) ||
           (row_identity && width_log2 > 5U) ||
           (column_identity && height_log2 > 5U);
}

static int test_av1_inverse_transforms(void) {
    int32_t values[64];
    int32_t dequantized[1024];
    int32_t residual[4096];
    uint16_t pixels[16];
    unsigned int index;
    unsigned int tx_size;
    uint64_t one_dimensional_checksum = (uint64_t)1469598103934665603ULL;
    uint64_t two_dimensional_checksum = (uint64_t)1469598103934665603ULL;

    avifdec_memory_fill(values, 0U, sizeof(values));
    values[0] = 1024;
    CHECK(av1_recon_inverse_1d(values, 2U, AV1_INVERSE_DCT,
                               16U, 0U) == AVIFDEC_OK);
    CHECK(values[0] == 724 && values[1] == 724 &&
          values[2] == 724 && values[3] == 724);
    avifdec_memory_fill(dequantized, 0U, sizeof(dequantized));
    dequantized[0] = 1024;
    CHECK(av1_recon_inverse_transform(dequantized, 16U, AV1_TX_4X4,
                                      AV1_TX_DCT_DCT, 8U, 0U,
                                      residual, 16U) == AVIFDEC_OK);
    for (index = 0U; index < 16U; ++index) CHECK(residual[index] == 32);
    for (index = 0U; index < 16U; ++index) pixels[index] = 240U;
    CHECK(av1_recon_add_residual(pixels, 4U, 4U, 4U, residual, 16U,
                                 8U, 1U, 1U) == AVIFDEC_OK);
    for (index = 0U; index < 16U; ++index) CHECK(pixels[index] == 255U);

    for (index = AV1_INVERSE_DCT; index <= AV1_INVERSE_IDENTITY; ++index) {
        unsigned int length_log2;
        for (length_log2 = 2U; length_log2 <= 6U; ++length_log2) {
            unsigned int length = 1U << length_log2;
            AvifdecStatus status;
            unsigned int value_index;
            for (value_index = 0U; value_index < length; ++value_index) {
                int32_t magnitude = (int32_t)(value_index * 73U +
                                              length_log2 * 19U + index * 11U);
                values[value_index] = (value_index & 1U) != 0U
                                      ? -magnitude : magnitude;
            }
            status = av1_recon_inverse_1d(values, length_log2,
                (Av1Inverse1dType)index, 20U, 0U);
            if ((index == AV1_INVERSE_ADST && length_log2 > 4U) ||
                (index == AV1_INVERSE_IDENTITY && length_log2 > 5U)) {
                CHECK(status == AVIFDEC_INVALID_ARGUMENT);
            } else {
                CHECK(status == AVIFDEC_OK);
                test_hash_i32(&one_dimensional_checksum, (int32_t)index);
                test_hash_i32(&one_dimensional_checksum, (int32_t)length_log2);
                for (value_index = 0U; value_index < length; ++value_index) {
                    test_hash_i32(&one_dimensional_checksum, values[value_index]);
                }
            }
        }
    }
    for (index = 0U; index <= 2U; ++index) {
        unsigned int value_index;
        for (value_index = 0U; value_index < 4U; ++value_index) {
            values[value_index] = (int32_t)(value_index * 131U) - 197;
        }
        CHECK(av1_recon_inverse_1d(values, 2U, AV1_INVERSE_WHT,
                                   20U, index) == AVIFDEC_OK);
        test_hash_i32(&one_dimensional_checksum, (int32_t)index);
        for (value_index = 0U; value_index < 4U; ++value_index) {
            test_hash_i32(&one_dimensional_checksum, values[value_index]);
        }
    }
    CHECK(one_dimensional_checksum == 0xd8b78fa96344285eULL);

    for (tx_size = 0U; tx_size < AV1_TX_SIZES_ALL; ++tx_size) {
        size_t width = av1_tx_size_info[tx_size].width;
        size_t height = av1_tx_size_info[tx_size].height;
        size_t packed = (width < 32U ? width : 32U) *
                        (height < 32U ? height : 32U);
        unsigned int tx_type;
        for (tx_type = 0U; tx_type < AV1_TX_TYPES; ++tx_type) {
            unsigned int depth_index;
            for (depth_index = 0U; depth_index < 3U; ++depth_index) {
                uint8_t bit_depth = (uint8_t)(8U + depth_index * 2U);
                AvifdecStatus status;
                size_t value_index;
                for (value_index = 0U; value_index < packed; ++value_index) {
                    int32_t magnitude = (int32_t)((value_index * 29U +
                        tx_size * 17U + tx_type * 13U + depth_index * 7U) & 1023U);
                    dequantized[value_index] = (value_index & 2U) != 0U
                                               ? -magnitude : magnitude;
                }
                status = av1_recon_inverse_transform(
                    dequantized, packed, (Av1TxSize)tx_size,
                    (Av1TxType)tx_type, bit_depth, 0U,
                    residual, width * height);
                if (test_tx_type_has_long_adst(
                        (Av1TxType)tx_type,
                        av1_tx_size_info[tx_size].width_log2,
                        av1_tx_size_info[tx_size].height_log2)) {
                    CHECK(status == AVIFDEC_INVALID_ARGUMENT);
                } else {
                    CHECK(status == AVIFDEC_OK);
                    test_hash_i32(&two_dimensional_checksum, (int32_t)tx_size);
                    test_hash_i32(&two_dimensional_checksum, (int32_t)tx_type);
                    test_hash_i32(&two_dimensional_checksum, bit_depth);
                    for (value_index = 0U; value_index < width * height;
                         ++value_index) {
                        test_hash_i32(&two_dimensional_checksum,
                                      residual[value_index]);
                    }
                }
            }
        }
    }
    CHECK(two_dimensional_checksum == 0x7b29398ca93fab3eULL);
    return 0;
}

static int test_av1_dsp_add_residual(void) {
    static const size_t widths[] = { 4U, 8U, 12U, 16U, 32U, 64U };
    uint16_t expected[67U * 8U];
    uint16_t actual[67U * 8U];
    int32_t residual[64U * 8U];
    unsigned int depth_index;
    unsigned int flip_lr;
    unsigned int flip_ud;
    size_t width_index;

    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        uint8_t bit_depth = (uint8_t)(8U + depth_index * 2U);
        uint16_t maximum = (uint16_t)(((uint32_t)1U << bit_depth) - 1U);
        for (flip_lr = 0U; flip_lr < 2U; ++flip_lr) {
            for (flip_ud = 0U; flip_ud < 2U; ++flip_ud) {
                for (width_index = 0U;
                     width_index < sizeof(widths) / sizeof(widths[0]);
                     ++width_index) {
                    size_t width = widths[width_index];
                    size_t index;

                    for (index = 0U;
                         index < sizeof(expected) / sizeof(expected[0]);
                         ++index) {
                        expected[index] =
                            (uint16_t)((index * 193U + width * 17U) & maximum);
                        actual[index] = expected[index];
                    }
                    for (index = 0U; index < width * 8U; ++index) {
                        residual[index] =
                            (int32_t)((index * 8191U + width * 257U) & 8191U) -
                            4096;
                    }
                    residual[0] = INT32_MIN;
                    residual[1] = INT32_MAX;
                    residual[width * 8U - 2U] = INT32_MAX;
                    residual[width * 8U - 1U] = INT32_MIN;
                    av1_dsp_add_residual_c(
                        expected, 67U, width, 8U, residual,
                        bit_depth, (uint8_t)flip_lr, (uint8_t)flip_ud);
                    CHECK(expected[
                              (flip_ud != 0U ? 7U : 0U) * 67U +
                              (flip_lr != 0U ? width - 1U : 0U)] == 0U);
                    CHECK(expected[
                              (flip_ud != 0U ? 7U : 0U) * 67U +
                              (flip_lr != 0U ? width - 2U : 1U)] == maximum);
                    av1_dsp_add_residual(
                        actual, 67U, width, 8U, residual,
                        bit_depth, (uint8_t)flip_lr, (uint8_t)flip_ud);
                    CHECK(avifdec_memory_compare(
                        expected, actual, sizeof(expected)) == 0);
                }
            }
        }
    }
    return 0;
}

static int test_av1_dsp_inverse_dct4(void) {
    int32_t input[16];
    int32_t expected[16];
    int32_t actual[16];
    uint32_t random = 0x6d2b79f5U;
    unsigned int iteration;
    size_t index;

    for (iteration = 0U; iteration < 4096U; ++iteration) {
        for (index = 0U; index < 16U; ++index) {
            random = random * 1664525U + 1013904223U;
            input[index] = (int32_t)((random >> 16) & 0xffffU) - 32768;
        }
        if (iteration == 0U) {
            for (index = 0U; index < 16U; ++index) input[index] = 0;
        } else if (iteration == 1U) {
            for (index = 0U; index < 16U; ++index) {
                input[index] = (index & 1U) != 0U ? -32768 : 32767;
            }
        }
        av1_dsp_inverse_dct4_c(input, expected);
        av1_dsp_inverse_dct4(input, actual);
        CHECK(avifdec_memory_compare(
            expected, actual, sizeof(expected)) == 0);
    }
    return 0;
}

static int test_av1_dsp_inverse_dct8(void) {
    int32_t input[64];
    int32_t expected[64];
    int32_t actual[64];
    uint32_t random = 0x9e3779b9U;
    unsigned int iteration;
    size_t index;

    for (iteration = 0U; iteration < 4096U; ++iteration) {
        for (index = 0U; index < 64U; ++index) {
            random = random * 1664525U + 1013904223U;
            input[index] = (int32_t)((random >> 16) & 0xffffU) - 32768;
        }
        if (iteration == 0U) {
            for (index = 0U; index < 64U; ++index) input[index] = 0;
        } else if (iteration == 1U) {
            for (index = 0U; index < 64U; ++index) {
                input[index] = (index & 1U) != 0U ? -32768 : 32767;
            }
        }
        av1_dsp_inverse_dct8_c(input, expected);
        av1_dsp_inverse_dct8(input, actual);
        CHECK(avifdec_memory_compare(
            expected, actual, sizeof(expected)) == 0);
    }
    return 0;
}

static int test_av1_dsp_inverse_dct16(void) {
    int32_t input[256];
    int32_t expected[256];
    int32_t actual[256];
    uint32_t random = 0x243f6a88U;
    unsigned int iteration;
    size_t index;

    for (iteration = 0U; iteration < 2048U; ++iteration) {
        for (index = 0U; index < 256U; ++index) {
            random = random * 1664525U + 1013904223U;
            input[index] = (int32_t)((random >> 16) & 0xffffU) - 32768;
        }
        if (iteration == 0U) {
            for (index = 0U; index < 256U; ++index) input[index] = 0;
        } else if (iteration == 1U) {
            for (index = 0U; index < 256U; ++index) {
                input[index] = (index & 1U) != 0U ? -32768 : 32767;
            }
        }
        av1_dsp_inverse_dct16_c(input, expected);
        av1_dsp_inverse_dct16(input, actual);
        CHECK(avifdec_memory_compare(
            expected, actual, sizeof(expected)) == 0);
    }
    return 0;
}

    static int test_av1_loop_filter(void) {
        uint16_t samples[32];
        uint16_t *edge = samples + 12U;
        uint16_t boundary_samples[9];
        unsigned int index;

        for (index = 0U; index < 12U; ++index) samples[index] = 100U;
        for (index = 12U; index < 32U; ++index) samples[index] = 104U;
        CHECK(av1_loop_filter_sample(edge, 1, 0U, 4U, 8U, 32U, 1U, 8U) ==
            AVIFDEC_OK);
        CHECK(edge[-2] == 101U && edge[-1] == 101U &&
            edge[0] == 102U && edge[1] == 103U);

        for (index = 0U; index < 12U; ++index) samples[index] = 100U;
        for (index = 12U; index < 32U; ++index) samples[index] = 102U;
        CHECK(av1_loop_filter_sample(edge, 1, 0U, 8U, 8U, 32U, 1U, 8U) ==
            AVIFDEC_OK);
        CHECK(edge[-4] == 100U && edge[-3] == 100U && edge[-2] == 101U &&
            edge[-1] == 101U && edge[0] == 101U && edge[1] == 102U &&
            edge[2] == 102U && edge[3] == 102U);

        for (index = 0U; index < 12U; ++index) samples[index] = 100U;
        for (index = 12U; index < 32U; ++index) samples[index] = 102U;
        CHECK(av1_loop_filter_sample(edge, 1, 1U, 8U, 8U, 32U, 1U, 8U) ==
            AVIFDEC_OK);
        CHECK(edge[-3] == 100U && edge[-2] == 100U && edge[-1] == 101U &&
            edge[0] == 101U && edge[1] == 102U && edge[2] == 102U);

        for (index = 0U; index < 12U; ++index) samples[index] = 400U;
        for (index = 12U; index < 32U; ++index) samples[index] = 416U;
        CHECK(av1_loop_filter_sample(edge, 1, 0U, 4U, 8U, 32U, 1U, 10U) ==
            AVIFDEC_OK);
        CHECK(edge[-2] == 403U && edge[-1] == 406U &&
            edge[0] == 410U && edge[1] == 413U);

        for (index = 0U; index < 12U; ++index) samples[index] = 1600U;
        for (index = 12U; index < 32U; ++index) samples[index] = 1664U;
        CHECK(av1_loop_filter_sample(edge, 1, 0U, 4U, 8U, 32U, 1U, 12U) ==
            AVIFDEC_OK);
        CHECK(edge[-2] == 1612U && edge[-1] == 1624U &&
            edge[0] == 1640U && edge[1] == 1652U);
        CHECK(av1_loop_filter_sample(0, 1, 0U, 4U, 8U, 32U, 1U, 8U) ==
            AVIFDEC_INVALID_ARGUMENT);
        for (index = 0U; index < 2U; ++index) {
            boundary_samples[index] = 100U;
        }
        for (; index < 9U; ++index) {
            boundary_samples[index] = 104U;
        }
        CHECK(av1_loop_filter_sample(
            boundary_samples + 2U, 1, 0U, 4U,
            8U, 32U, 1U, 8U) == AVIFDEC_OK);
        return 0;
    }

static AvifdecStatus partition_all_split(void *user_data,
                                         uint32_t row,
                                         uint32_t column,
                                         uint32_t block_mi,
                                         unsigned int context,
                                         int has_rows,
                                         int has_columns,
                                         Av1Partition *partition) {
    (void)user_data;
    (void)row;
    (void)column;
    (void)block_mi;
    (void)context;
    (void)has_rows;
    (void)has_columns;
    *partition = AV1_PARTITION_SPLIT;
    return AVIFDEC_OK;
}

static AvifdecStatus partition_invalid(void *user_data,
                                       uint32_t row,
                                       uint32_t column,
                                       uint32_t block_mi,
                                       unsigned int context,
                                       int has_rows,
                                       int has_columns,
                                       Av1Partition *partition) {
    (void)user_data;
    (void)row;
    (void)column;
    (void)block_mi;
    (void)context;
    (void)has_rows;
    (void)has_columns;
    *partition = (Av1Partition)10;
    return AVIFDEC_OK;
}

static AvifdecStatus partition_fixed(void *user_data,
                                      uint32_t row,
                                      uint32_t column,
                                      uint32_t block_mi,
                                      unsigned int context,
                                      int has_rows,
                                      int has_columns,
                                      Av1Partition *partition) {
    (void)row;
    (void)column;
    (void)block_mi;
    (void)context;
    (void)has_rows;
    (void)has_columns;
    *partition = *(const Av1Partition *)user_data;
    return AVIFDEC_OK;
}

typedef struct {
    size_t count;
    uint64_t order;
} PartitionBlocks;

static AvifdecStatus partition_record_block(void *user_data,
                                             uint32_t row,
                                             uint32_t column,
                                             uint32_t width,
                                             uint32_t height) {
    PartitionBlocks *blocks = (PartitionBlocks *)user_data;
    uint32_t expected_row = 0U;
    uint32_t expected_column = 0U;
    unsigned int bit;

    CHECK(width == 1U && height == 1U);
    for (bit = 0U; bit < 4U; ++bit) {
        expected_column |= (uint32_t)((blocks->count >> (2U * bit)) & 1U) << bit;
        expected_row |= (uint32_t)((blocks->count >> (2U * bit + 1U)) & 1U) << bit;
    }
    CHECK(row == expected_row);
    CHECK(column == expected_column);
    blocks->order = blocks->order * 257U + row * 16U + column;
    ++blocks->count;
    return AVIFDEC_OK;
}

static int test_av1_partition_engine(void) {
    static const size_t expected_blocks[10] = { 1U, 2U, 2U, 256U, 3U,
                                                 3U, 3U, 3U, 4U, 4U };
    uint8_t widths[32U * 32U];
    uint8_t heights[32U * 32U];
    Av1PartitionGrid grid;
    Av1PartitionTrace trace;
    Av1PartitionTrace repeated;
    Av1PartitionCdfs cdfs;
    PartitionBlocks blocks;
    Av1Partition partition;

    av1_partition_cdfs_init(&cdfs);
    CHECK(cdfs.width8[0][0] == 19132U && cdfs.width128[3][7] == 32768U);
    CHECK(av1_partition_cdfs_checksum(&cdfs) == (uint64_t)0xc4f4c5009094a509ULL);

    avifdec_memory_fill(&grid, 0U, sizeof(grid));
    avifdec_memory_fill(widths, 0U, sizeof(widths));
    avifdec_memory_fill(heights, 0U, sizeof(heights));
    grid.mi_rows = 32U;
    grid.mi_columns = 32U;
    grid.tile_row_end = 32U;
    grid.tile_column_end = 32U;
    grid.block_widths = widths;
    grid.block_heights = heights;
    grid.grid_capacity = sizeof(widths);
    grid.max_partition_nodes = 2000U;
    grid.read_partition = partition_all_split;
    avifdec_memory_fill(&blocks, 0U, sizeof(blocks));
    grid.decode_block = partition_record_block;
    grid.user_data = &blocks;
    CHECK(av1_partition_superblock(&grid, 0U, 0U, 16U, &trace) == AVIFDEC_OK);
    CHECK(trace.partition_nodes == 341U && trace.block_count == 256U && trace.max_depth == 4U);
    CHECK(blocks.count == 256U && blocks.order != 0U);
    CHECK(widths[0] == 1U && heights[31U * 32U + 31U] == 0U);

    avifdec_memory_fill(widths, 0U, sizeof(widths));
    avifdec_memory_fill(heights, 0U, sizeof(heights));
    grid.decode_block = 0;
    grid.user_data = 0;
    CHECK(av1_partition_superblock(&grid, 0U, 0U, 32U, &trace) == AVIFDEC_OK);
    CHECK(trace.partition_nodes == 1365U && trace.block_count == 1024U && trace.max_depth == 5U);
    CHECK(widths[0] == 1U && heights[31U * 32U + 31U] == 1U);
    avifdec_memory_fill(widths, 0U, sizeof(widths));
    avifdec_memory_fill(heights, 0U, sizeof(heights));
    CHECK(av1_partition_superblock(&grid, 0U, 0U, 32U, &repeated) == AVIFDEC_OK);
    CHECK(repeated.checksum == trace.checksum);

    grid.mi_rows = 16U;
    grid.mi_columns = 16U;
    grid.tile_row_end = 16U;
    grid.tile_column_end = 16U;
    grid.grid_capacity = 16U * 16U;
    grid.read_partition = partition_fixed;
    for (partition = AV1_PARTITION_NONE; partition <= AV1_PARTITION_VERT_4;
         partition = (Av1Partition)(partition + 1)) {
        avifdec_memory_fill(widths, 0U, sizeof(widths));
        avifdec_memory_fill(heights, 0U, sizeof(heights));
        grid.user_data = &partition;
        CHECK(av1_partition_superblock(&grid, 0U, 0U, 16U, &trace) == AVIFDEC_OK);
        CHECK(trace.block_count == expected_blocks[partition]);
    }

    grid.mi_rows = 32U;
    grid.mi_columns = 32U;
    grid.tile_row_end = 32U;
    grid.tile_column_end = 32U;
    grid.grid_capacity = sizeof(widths);
    grid.read_partition = partition_all_split;
    grid.user_data = 0;
    grid.max_partition_nodes = 10U;
    CHECK(av1_partition_superblock(&grid, 0U, 0U, 32U, &trace) == AVIFDEC_LIMIT_EXCEEDED);
    grid.max_partition_nodes = 2000U;
    grid.read_partition = partition_invalid;
    CHECK(av1_partition_superblock(&grid, 0U, 0U, 16U, &trace) == AVIFDEC_INVALID_DATA);
    return 0;
}

static unsigned int test_cdef_direction_line(unsigned int direction,
                                             unsigned int row,
                                             unsigned int column) {
    switch (direction) {
        case 0U: return row + column;
        case 1U: return row + column / 2U;
        case 2U: return row;
        case 3U: return 3U + row - column / 2U;
        case 4U: return 7U + row - column;
        case 5U: return 3U - row / 2U + column;
        case 6U: return column;
        default: return row / 2U + column;
    }
}

static int test_av1_cdef_interior(void) {
    static const uint8_t subsampling[3][2] = {
        { 0U, 0U }, { 1U, 0U }, { 1U, 1U }
    };
    uint16_t source[3][32U * 24U];
    uint16_t destination[3][32U * 24U];
    Av1BlockCell cells[8U * 6U];
    Av1BlockState blocks;
    Av1FramePlanes input;
    Av1FramePlanes output;
    Av1CdefParams params;
    uint8_t indices[8U * 6U];
    uint8_t primary[1];
    uint8_t secondary[1];
    unsigned int depth_index;
    unsigned int subsampling_index;
    unsigned int strength_case;
    unsigned int index;

    avifdec_memory_fill(cells, 0U, sizeof(cells));
    avifdec_memory_fill(indices, 0U, sizeof(indices));
    for (index = 0U; index < 8U * 6U; ++index) {
        cells[index].width = 2U;
        cells[index].height = 2U;
    }
    avifdec_memory_fill(&blocks, 0U, sizeof(blocks));
    blocks.mi_rows = 6U;
    blocks.mi_columns = 8U;
    blocks.cells = cells;
    blocks.cell_capacity = 8U * 6U;

    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        uint8_t bit_depth = (uint8_t)(8U + 2U * depth_index);
        unsigned int coeff_shift = bit_depth - 8U;

        for (subsampling_index = 0U; subsampling_index < 3U;
             ++subsampling_index) {
            uint8_t sub_x = subsampling[subsampling_index][0];
            uint8_t sub_y = subsampling[subsampling_index][1];

            for (strength_case = 0U; strength_case < 4U; ++strength_case) {
                unsigned int direction_count =
                    strength_case == 0U ? 8U : 1U;
                unsigned int direction;

                primary[0] = strength_case < 2U ? 8U : 0U;
                secondary[0] =
                    strength_case == 0U || strength_case == 2U ? 4U : 0U;
                for (direction = 0U; direction < direction_count;
                     ++direction) {
                    unsigned int plane;
                    unsigned int changed = 0U;
                    uint8_t detected_direction;
                    uint32_t variance;

                    avifdec_memory_fill(&input, 0U, sizeof(input));
                    avifdec_memory_fill(&output, 0U, sizeof(output));
                    avifdec_memory_fill(&params, 0U, sizeof(params));
                    for (plane = 0U; plane < 3U; ++plane) {
                        unsigned int plane_sub_x = plane == 0U ? 0U : sub_x;
                        unsigned int plane_sub_y = plane == 0U ? 0U : sub_y;
                        unsigned int stride = 32U >> plane_sub_x;
                        unsigned int backing_height = 24U >> plane_sub_y;
                        unsigned int width =
                            (30U + ((1U << plane_sub_x) - 1U)) >> plane_sub_x;
                        unsigned int height =
                            (22U + ((1U << plane_sub_y) - 1U)) >> plane_sub_y;
                        unsigned int block_width = 8U >> plane_sub_x;
                        unsigned int block_height = 8U >> plane_sub_y;
                        unsigned int row;
                        unsigned int column;

                        input.data[plane] = source[plane];
                        input.stride[plane] = stride;
                        input.width[plane] = width;
                        input.height[plane] = height;
                        output.data[plane] = destination[plane];
                        output.stride[plane] = stride;
                        output.width[plane] = width;
                        output.height[plane] = height;
                        for (row = 0U; row < backing_height; ++row) {
                            for (column = 0U; column < stride; ++column) {
                                unsigned int local_row = row % block_height;
                                unsigned int local_column =
                                    column % block_width;
                                unsigned int value;

                                if (plane == 0U) {
                                    value = 48U + 10U *
                                        test_cdef_direction_line(
                                            direction, local_row, local_column) +
                                        ((local_row * 5U +
                                          local_column * 3U) & 3U) +
                                        (local_row == 3U &&
                                         local_column == 4U ? 6U : 0U);
                                } else {
                                    value = 40U +
                                        ((local_row * 31U +
                                          local_column * 17U + plane * 29U +
                                          direction * 7U) & 127U);
                                }
                                source[plane][row * stride + column] =
                                    (uint16_t)(value << coeff_shift);
                                destination[plane][row * stride + column] = 0U;
                            }
                        }
                    }
                    params.frame_width = 30U;
                    params.frame_height = 22U;
                    params.mi_rows = 6U;
                    params.mi_columns = 8U;
                    params.bit_depth = bit_depth;
                    params.subsampling_x = sub_x;
                    params.subsampling_y = sub_y;
                    params.damping = 3U;
                    params.y_pri_strength = primary;
                    params.y_sec_strength = secondary;
                    params.uv_pri_strength = primary;
                    params.uv_sec_strength = secondary;
                    params.indices = indices;
                    params.index_capacity = sizeof(indices);
                    CHECK(av1_cdef_find_direction(
                              source[0] + 8U * 32U + 8U, 32U, bit_depth,
                              &detected_direction, &variance) == AVIFDEC_OK);
                    CHECK(detected_direction == direction && variance != 0U);
                    CHECK(av1_cdef_frame(
                              &output, &input, &blocks, &params) == AVIFDEC_OK);

                    for (plane = 0U; plane < 3U; ++plane) {
                        unsigned int plane_sub_x = plane == 0U ? 0U : sub_x;
                        unsigned int plane_sub_y = plane == 0U ? 0U : sub_y;
                        unsigned int stride = 32U >> plane_sub_x;
                        unsigned int width = output.width[plane];
                        unsigned int height = output.height[plane];
                        unsigned int block_width = 8U >> plane_sub_x;
                        unsigned int block_height = 8U >> plane_sub_y;
                        unsigned int middle_x = 8U >> plane_sub_x;
                        unsigned int middle_y = 8U >> plane_sub_y;
                        unsigned int right_x = 24U >> plane_sub_x;
                        unsigned int bottom_y = 16U >> plane_sub_y;
                        unsigned int comparable_right =
                            (32U >> plane_sub_x) - right_x - 2U;
                        unsigned int comparable_bottom =
                            (24U >> plane_sub_y) - bottom_y - 2U;
                        unsigned int row;
                        unsigned int column;

                        for (row = 0U; row < block_height; ++row) {
                            for (column = 2U; column < block_width; ++column) {
                                CHECK(destination[plane][
                                          (middle_y + row) * stride + column] ==
                                      destination[plane][
                                          (middle_y + row) * stride +
                                          middle_x + column]);
                            }
                        }
                        for (row = 2U; row < block_height; ++row) {
                            for (column = 0U; column < block_width; ++column) {
                                CHECK(destination[plane][
                                          row * stride + middle_x + column] ==
                                      destination[plane][
                                          (middle_y + row) * stride +
                                          middle_x + column]);
                            }
                        }
                        for (row = 0U; row < block_height; ++row) {
                            for (column = 0U; column < comparable_right;
                                 ++column) {
                                CHECK(destination[plane][
                                          (middle_y + row) * stride +
                                          right_x + column] ==
                                      destination[plane][
                                          (middle_y + row) * stride +
                                          middle_x + column]);
                            }
                        }
                        for (row = 0U; row < comparable_bottom; ++row) {
                            for (column = 0U; column < block_width; ++column) {
                                CHECK(destination[plane][
                                          (bottom_y + row) * stride +
                                          middle_x + column] ==
                                      destination[plane][
                                          (middle_y + row) * stride +
                                          middle_x + column]);
                            }
                        }
                        for (row = 0U; row < height; ++row) {
                            for (column = 0U; column < width; ++column) {
                                changed += source[plane][row * stride + column] !=
                                    destination[plane][row * stride + column];
                            }
                        }
                    }
                    if (strength_case == 3U) {
                        CHECK(changed == 0U);
                    } else if (strength_case == 0U) {
                        CHECK(changed != 0U);
                    }
                }
            }
        }
    }
    return 0;
}

static int test_av1_cdef(void) {
    uint16_t source[16U * 16U];
    uint16_t destination[16U * 16U];
    uint16_t parallel_destination[16U * 16U];
    Av1BlockCell cells[16];
    Av1BlockState blocks;
    Av1FramePlanes input;
    Av1FramePlanes output;
    Av1FramePlanes parallel_output;
    Av1CdefParams params;
    OutOfOrderExecutorState executor_state = { 0U, 0U, 0U };
    AvifdecExecutor executor = {
        &executor_state, 4U, out_of_order_parallel_for
    };
    uint8_t indices[16];
    uint8_t primary[8] = { 8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t secondary[8] = { 4U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
    uint8_t direction;
    uint32_t variance;
    unsigned int row;
    unsigned int column;

    for (row = 0U; row < 8U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            source[row * 16U + column] = 128U;
        }
    }
    CHECK(av1_cdef_find_direction(source, 16U, 8U, &direction, &variance) ==
          AVIFDEC_OK);
    CHECK(direction == 0U && variance == 0U);
    for (row = 0U; row < 8U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            source[row * 16U + column] = (uint16_t)(column * 16U);
        }
    }
    CHECK(av1_cdef_find_direction(source, 16U, 8U, &direction, &variance) ==
          AVIFDEC_OK);
    CHECK(direction == 6U && variance != 0U);
    for (row = 0U; row < 8U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            source[row * 16U + column] = (uint16_t)(row * 16U);
        }
    }
    CHECK(av1_cdef_find_direction(source, 16U, 8U, &direction, &variance) ==
          AVIFDEC_OK);
    CHECK(direction == 2U && variance != 0U);
    for (row = 0U; row < 8U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            source[row * 16U + column] = (uint16_t)((row + column) * 8U);
        }
    }
    CHECK(av1_cdef_find_direction(source, 16U, 8U, &direction, &variance) ==
          AVIFDEC_OK);
    CHECK(direction == 0U && variance != 0U);

    avifdec_memory_fill(cells, 0U, sizeof(cells));
    avifdec_memory_fill(&blocks, 0U, sizeof(blocks));
    avifdec_memory_fill(&input, 0U, sizeof(input));
    avifdec_memory_fill(&output, 0U, sizeof(output));
    avifdec_memory_fill(
        &parallel_output, 0U, sizeof(parallel_output));
    avifdec_memory_fill(&params, 0U, sizeof(params));
    avifdec_memory_fill(indices, 0U, sizeof(indices));
    for (row = 0U; row < 16U; ++row) {
        for (column = 0U; column < 16U; ++column) {
            source[row * 16U + column] = 173U;
            cells[(row >> 2) * 4U + (column >> 2)].width = 2U;
            cells[(row >> 2) * 4U + (column >> 2)].height = 2U;
        }
    }
    blocks.mi_rows = 4U;
    blocks.mi_columns = 4U;
    blocks.cells = cells;
    blocks.cell_capacity = 16U;
    input.data[0] = source;
    input.stride[0] = 16U;
    input.width[0] = 16U;
    input.height[0] = 16U;
    output.data[0] = destination;
    output.stride[0] = 16U;
    output.width[0] = 16U;
    output.height[0] = 16U;
    parallel_output.data[0] = parallel_destination;
    parallel_output.stride[0] = 16U;
    parallel_output.width[0] = 16U;
    parallel_output.height[0] = 16U;
    params.frame_width = 16U;
    params.frame_height = 16U;
    params.mi_rows = 4U;
    params.mi_columns = 4U;
    params.bit_depth = 8U;
    params.monochrome = 1U;
    params.damping = 3U;
    params.y_pri_strength = primary;
    params.y_sec_strength = secondary;
    params.indices = indices;
    params.index_capacity = 16U;
    CHECK(av1_cdef_frame(&output, &input, &blocks, &params) == AVIFDEC_OK);
    for (row = 0U; row < 16U * 16U; ++row) {
        CHECK(destination[row] == 173U);
    }
    for (row = 0U; row < 16U; ++row) {
        for (column = 0U; column < 16U; ++column) {
            source[row * 16U + column] =
                (uint16_t)((row * 29U + column * 17U +
                            ((row ^ column) * 11U)) & 255U);
            destination[row * 16U + column] = 0U;
            parallel_destination[row * 16U + column] = 0U;
        }
    }
    CHECK(av1_cdef_frame(
        &output, &input, &blocks, &params) == AVIFDEC_OK);
    CHECK(av1_cdef_frame_ex(
        &parallel_output, &input, &blocks, &params,
        &executor) == AVIFDEC_OK);
    CHECK(executor_state.calls == 1U &&
          executor_state.count == 2U &&
          executor_state.min_chunk == 1U);
    for (row = 0U; row < 16U * 16U; ++row) {
        CHECK(destination[row] ==
              parallel_destination[row]);
    }
    executor_state.calls = 0U;
    indices[0] = 1U;
    CHECK(av1_cdef_frame_ex(
        &parallel_output, &input, &blocks, &params,
        &executor) == AVIFDEC_INVALID_DATA);
    CHECK(executor_state.calls == 0U);
    return 0;
}

static int test_av1_superres(void) {
    static const uint16_t expected[11] = {
        0U, 14U, 0U, 12U, 227U, 157U, 0U, 0U, 10U, 0U, 0U
    };
    uint16_t input[16];
    uint16_t output[22];
    uint16_t parallel_output[22];
    TestReverseExecutorState executor_state = { 0U, 4U };
    AvifdecExecutor executor = {
        &executor_state, 4U, test_reverse_parallel_for
    };
    unsigned int index;
    unsigned int depth_index;

    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        uint8_t bit_depth = (uint8_t)(8U + depth_index * 2U);
        uint16_t value = (uint16_t)(173U << (bit_depth - 8U));

        avifdec_memory_fill(input, 0U, sizeof(input));
        input[3] = 255U;
        CHECK(av1_superres_upscale_plane(output, 11U, 11U, input, 8U,
                                         8U, 8U, 1U, bit_depth) == AVIFDEC_OK);
        for (index = 0U; index < 11U; ++index) {
            CHECK(output[index] == expected[index]);
        }
        for (index = 0U; index < 16U; ++index) input[index] = value;
        CHECK(av1_superres_upscale_plane(output, 11U, 11U, input, 8U,
                                         8U, 8U, 2U, bit_depth) == AVIFDEC_OK);
        CHECK(av1_superres_upscale_plane_ex(
            parallel_output, 11U, 11U, input, 8U,
            8U, 8U, 2U, bit_depth, &executor) == AVIFDEC_OK);
        for (index = 0U; index < 22U; ++index) CHECK(output[index] == value);
        CHECK(avifdec_memory_compare(
            output, parallel_output, sizeof(output)) == 0);
    }
    CHECK(executor_state.calls == 3U);
    CHECK(av1_superres_upscale_plane(output, 8U, 8U, input, 8U,
                                     8U, 8U, 1U, 8U) ==
          AVIFDEC_INVALID_ARGUMENT);
    return 0;
}

static int test_av1_loop_restoration(void) {
    uint16_t cdef_data[64];
    uint16_t deblocked_data[64];
    uint16_t output_data[64];
    uint16_t parallel_output_data[64];
    uint16_t cdef_chroma[2][16];
    uint16_t deblocked_chroma[2][16];
    uint16_t output_chroma[2][16];
    uint16_t parallel_output_chroma[2][16];
    Av1FramePlanes cdef;
    Av1FramePlanes deblocked;
    Av1FramePlanes output;
    Av1FramePlanes parallel_output;
    Av1RestorationUnit units[3];
    Av1RestorationState restoration;
    OutOfOrderExecutorState executor_state = { 0U, 0U, 0U };
    AvifdecExecutor executor = {
        &executor_state, 4U, out_of_order_parallel_for
    };
    uint8_t frame_type[3] = { 1U, 0U, 0U };
    uint16_t unit_size[3] = { 64U, 0U, 0U };
    size_t unit_capacity;
    unsigned int depth_index;
    unsigned int plane;
    unsigned int set;
    unsigned int index;

    CHECK(av1_restoration_unit_capacity(
        65U, 33U, 1, 1, 1, &unit_capacity) == AVIFDEC_OK &&
        unit_capacity == 2U);
    CHECK(av1_restoration_unit_capacity(
        65U, 33U, 0, 1, 1, &unit_capacity) == AVIFDEC_OK &&
        unit_capacity == 4U);
    CHECK(av1_restoration_unit_capacity(
        65U, 33U, 0, 1, 0, &unit_capacity) == AVIFDEC_OK &&
        unit_capacity == 4U);
    CHECK(av1_restoration_unit_capacity(
        65U, 33U, 0, 0, 0, &unit_capacity) == AVIFDEC_OK &&
        unit_capacity == 6U);

    avifdec_memory_fill(&cdef, 0U, sizeof(cdef));
    avifdec_memory_fill(&deblocked, 0U, sizeof(deblocked));
    avifdec_memory_fill(&output, 0U, sizeof(output));
    avifdec_memory_fill(
        &parallel_output, 0U, sizeof(parallel_output));
    cdef.data[0] = cdef_data;
    deblocked.data[0] = deblocked_data;
    output.data[0] = output_data;
    parallel_output.data[0] = parallel_output_data;
    cdef.stride[0] = 8U;
    deblocked.stride[0] = 8U;
    output.stride[0] = 8U;
    parallel_output.stride[0] = 8U;
    for (depth_index = 0U; depth_index < 3U; ++depth_index) {
        uint8_t bit_depth = (uint8_t)(8U + 2U * depth_index);
        uint16_t value = (uint16_t)(137U << (bit_depth - 8U));
        uint16_t sgr_value = bit_depth == 12U ? value - 1U : value;
        for (index = 0U; index < 64U; ++index) {
            cdef_data[index] = value;
            deblocked_data[index] = value;
            output_data[index] = 0U;
            parallel_output_data[index] = 0U;
        }
        CHECK(av1_restoration_state_init(
                  &restoration, units, 3U, 8U, 8U, 8U, frame_type,
                  unit_size, 1, 0, 0) == AVIFDEC_OK);
        units[0].parsed = 1U;
        units[0].type = 1U;
        units[0].wiener[0][0] = 3;
        units[0].wiener[0][1] = -7;
        units[0].wiener[0][2] = 15;
        units[0].wiener[1][0] = -5;
        units[0].wiener[1][1] = 8;
        units[0].wiener[1][2] = 22;
        CHECK(av1_loop_restoration_frame(
                  &output, &cdef, &deblocked, &restoration, bit_depth) ==
              AVIFDEC_OK);
        executor_state.calls = 0U;
        CHECK(av1_loop_restoration_frame_ex(
                  &parallel_output, &cdef, &deblocked,
                  &restoration, bit_depth, &executor) ==
              AVIFDEC_OK);
        CHECK(executor_state.calls == 1U &&
              executor_state.count == 2U &&
              executor_state.min_chunk == 1U);
        for (index = 0U; index < 64U; ++index) CHECK(output_data[index] == value);
        for (index = 0U; index < 64U; ++index) {
            CHECK(parallel_output_data[index] ==
                  output_data[index]);
        }
        frame_type[0] = 2U;
        for (set = 0U; set < 16U; ++set) {
            CHECK(av1_restoration_state_init(
                      &restoration, units, 3U, 8U, 8U, 8U, frame_type,
                      unit_size, 1, 0, 0) == AVIFDEC_OK);
            units[0].parsed = 1U;
            units[0].type = 2U;
            units[0].sgr_set = (uint8_t)set;
            units[0].sgr_xqd[0] = -32;
            units[0].sgr_xqd[1] = 31;
            CHECK(av1_loop_restoration_frame(
                      &output, &cdef, &deblocked, &restoration, bit_depth) ==
                  AVIFDEC_OK);
            for (index = 0U; index < 64U; ++index) {
                CHECK(output_data[index] == sgr_value);
            }
        }
        frame_type[0] = 0U;
        CHECK(av1_restoration_state_init(
                  &restoration, units, 3U, 8U, 8U, 8U, frame_type,
                  unit_size, 1, 0, 0) == AVIFDEC_OK);
        CHECK(av1_loop_restoration_frame(
                  &output, &cdef, &deblocked, &restoration, bit_depth) ==
              AVIFDEC_OK);
        for (index = 0U; index < 64U; ++index) CHECK(output_data[index] == value);
        frame_type[0] = 1U;
    }
    for (index = 0U; index < 64U; ++index) {
        cdef_data[index] = (uint16_t)(
            (index * 37U + (index >> 3U) * 19U) & 255U);
        deblocked_data[index] = (uint16_t)(
            (index * 11U + 53U) & 255U);
        output_data[index] = 0U;
        parallel_output_data[index] = 0U;
    }
    CHECK(av1_restoration_state_init(
              &restoration, units, 3U, 8U, 8U, 8U,
              frame_type, unit_size, 1, 0, 0) == AVIFDEC_OK);
    units[0].parsed = 1U;
    units[0].type = 1U;
    units[0].wiener[0][0] = 3;
    units[0].wiener[0][1] = -7;
    units[0].wiener[0][2] = 15;
    units[0].wiener[1][0] = -5;
    units[0].wiener[1][1] = 8;
    units[0].wiener[1][2] = 22;
    CHECK(av1_loop_restoration_frame(
              &output, &cdef, &deblocked,
              &restoration, 8U) == AVIFDEC_OK);
    executor_state.calls = 0U;
    CHECK(av1_loop_restoration_frame_ex(
              &parallel_output, &cdef, &deblocked,
              &restoration, 8U, &executor) == AVIFDEC_OK);
    CHECK(executor_state.calls == 1U &&
          executor_state.count == 2U);
    for (index = 0U; index < 64U; ++index) {
        CHECK(parallel_output_data[index] ==
              output_data[index]);
    }
    units[0].type = 3U;
    executor_state.calls = 0U;
    CHECK(av1_loop_restoration_frame_ex(
              &parallel_output, &cdef, &deblocked,
              &restoration, 8U, &executor) ==
          AVIFDEC_INVALID_DATA);
    CHECK(executor_state.calls == 0U);
    for (plane = 0U; plane < 2U; ++plane) {
        cdef.data[plane + 1U] = cdef_chroma[plane];
        deblocked.data[plane + 1U] =
            deblocked_chroma[plane];
        output.data[plane + 1U] = output_chroma[plane];
        parallel_output.data[plane + 1U] =
            parallel_output_chroma[plane];
        cdef.stride[plane + 1U] = 4U;
        deblocked.stride[plane + 1U] = 4U;
        output.stride[plane + 1U] = 4U;
        parallel_output.stride[plane + 1U] = 4U;
        for (index = 0U; index < 16U; ++index) {
            cdef_chroma[plane][index] =
                (uint16_t)(plane * 31U + index * 7U);
            deblocked_chroma[plane][index] =
                (uint16_t)(plane * 17U + index * 5U);
            output_chroma[plane][index] = 0U;
            parallel_output_chroma[plane][index] = 0U;
        }
    }
    CHECK(av1_restoration_state_init(
              &restoration, units, 3U, 8U, 8U, 8U,
              frame_type, unit_size, 0, 1, 1) == AVIFDEC_OK);
    units[0].parsed = 1U;
    units[0].type = 1U;
    units[0].wiener[0][0] = 3;
    units[0].wiener[0][1] = -7;
    units[0].wiener[0][2] = 15;
    units[0].wiener[1][0] = -5;
    units[0].wiener[1][1] = 8;
    units[0].wiener[1][2] = 22;
    CHECK(av1_loop_restoration_frame(
              &output, &cdef, &deblocked,
              &restoration, 8U) == AVIFDEC_OK);
    executor_state.calls = 0U;
    CHECK(av1_loop_restoration_frame_ex(
              &parallel_output, &cdef, &deblocked,
              &restoration, 8U, &executor) == AVIFDEC_OK);
    CHECK(executor_state.calls == 1U &&
          executor_state.count == 6U);
    for (index = 0U; index < 64U; ++index) {
        CHECK(parallel_output_data[index] ==
              output_data[index]);
    }
    for (plane = 0U; plane < 2U; ++plane) {
        for (index = 0U; index < 16U; ++index) {
            CHECK(parallel_output_chroma[plane][index] ==
                  output_chroma[plane][index]);
        }
    }
    return 0;
}

    static int test_av1_block_state(void) {
        static const unsigned char zero_data[2] = { 0x20U, 0x00U };
        Av1BlockCell cells[6U * 8U];
        union {
            uint32_t alignment;
            uint8_t bytes[sizeof(cells)];
        } compact_cells;
        Av1BlockState state;
        Av1BlockAvailability availability;
        Av1BlockTraceFields fields;
        Av1BlockTrace trace;
        Av1TileCdfs cdfs;
        Av1TileCdfs fresh_cdfs;
        Av1TilePaletteContext palette_context;
        Av1SymbolDecoder decoder;
        AvifdecSpan span;
        uint8_t value;
        int8_t angle;
        int32_t delta;
        uint64_t first_checksum;

        av1_tile_cdfs_init(&cdfs);
        av1_tile_cdfs_init(&fresh_cdfs);
        CHECK(cdfs.skip[0][0] == 31671U && cdfs.segment_id[2][7] == 32768U);
        CHECK(cdfs.y_mode[3][12] == 32768U && cdfs.tx64[2][1] == 22759U);
        CHECK(av1_tile_cdfs_checksum(&cdfs) == av1_tile_cdfs_checksum(&fresh_cdfs));
        cdfs.skip[0][0] = 0U;
        CHECK(av1_tile_cdfs_checksum(&cdfs) != av1_tile_cdfs_checksum(&fresh_cdfs));
        av1_tile_cdfs_init(&cdfs);

        CHECK(AV1_BLOCK_BASE_CELL_SIZE == 24U);
        CHECK(sizeof(Av1BlockCell) == 124U);
        CHECK(av1_tile_palette_context_init(
                  &palette_context, 8U, 16U, 32U) == AVIFDEC_OK);
        palette_context.above[0].y[0] = 7U;
        palette_context.left[0].u[0] = 9U;
        CHECK(av1_tile_palette_context_new_row_band(
                  &palette_context, 40U) == AVIFDEC_OK);
        CHECK(palette_context.above[0].y[0] == 7U &&
              palette_context.left[0].u[0] == 0U &&
              palette_context.row_band_start == 40U);
        CHECK(av1_tile_palette_context_init(
                  &palette_context, 0U, 0U,
                  AV1_TILE_PALETTE_ABOVE_MI + 1U) ==
              AVIFDEC_INVALID_ARGUMENT);
        CHECK(av1_block_state_init_compact(
                  &state, 6U, 8U,
                  (Av1BlockCell *)(void *)compact_cells.bytes,
                  48U, 0, 1, 1) == AVIFDEC_OK);
        avifdec_memory_fill(&fields, 0U, sizeof(fields));
        fields.row = 5U;
        fields.column = 7U;
        fields.width = 1U;
        fields.height = 1U;
        CHECK(av1_block_state_record(&state, &fields, 0) == AVIFDEC_OK);
        CHECK(av1_block_cell(&state, 5U, 7U)->width == 1U);
        CHECK((const uint8_t *)(const void *)av1_block_cell(&state, 5U, 7U) -
                  compact_cells.bytes ==
              47U * AV1_BLOCK_BASE_CELL_SIZE);

        CHECK(av1_block_state_init(&state, 6U, 8U, cells, 47U, 0, 1, 1) ==
            AVIFDEC_LIMIT_EXCEEDED);
        CHECK(av1_block_state_init(&state, 6U, 8U, cells, 48U, 0, 1, 1) == AVIFDEC_OK);
        CHECK(av1_block_state_set_tile(&state, 1U, 6U, 2U, 8U) == AVIFDEC_OK);
        CHECK(av1_block_state_availability(&state, 1U, 2U, 1U, 1U, &availability) ==
            AVIFDEC_OK);
        CHECK(availability.has_chroma == 0U && availability.above == 0U &&
            availability.left == 0U && availability.above_chroma == 0U &&
            availability.left_chroma == 0U);
        CHECK(av1_block_state_availability(&state, 1U, 3U, 1U, 1U, &availability) ==
            AVIFDEC_OK);
        CHECK(availability.has_chroma == 1U && availability.left == 1U &&
            availability.left_chroma == 0U);
        CHECK(av1_block_state_availability(&state, 2U, 2U, 2U, 2U, &availability) ==
            AVIFDEC_OK);
        CHECK(availability.has_chroma == 1U && availability.above == 1U &&
            availability.left == 0U);

            span.data = zero_data;
            span.size = sizeof(zero_data);
            span.file_offset = 0U;
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_skip(&decoder, &cdfs, &state, &availability,
                             2U, 2U, 0, &value) == AVIFDEC_OK && value == 0U);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_segment_id(&decoder, &cdfs, &state, &availability,
                                 2U, 2U, 0U, 0, &value) == AVIFDEC_OK && value == 0U);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_segment_id(&decoder, &cdfs, &state, &availability,
                                 2U, 2U, 2U, 0, &value) == AVIFDEC_OK && value <= 2U);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_delta(&decoder, cdfs.delta_q, &delta) == AVIFDEC_OK && delta == 0);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_y_mode(&decoder, &cdfs, 2U, 2U, &value) == AVIFDEC_OK &&
                value < 13U);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_angle_delta(&decoder, &cdfs, 1U, &angle) == AVIFDEC_OK &&
                angle >= -3 && angle <= 3);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_tx_depth(&decoder, &cdfs, 2U, 2U, 0U, &value) == AVIFDEC_OK &&
                value <= 2U);
            CHECK(av1_tile_read_tx_depth(&decoder, &cdfs, 4U, 0U, 0U, &value) == AVIFDEC_OK &&
                value == 0U);

            cells[1U * 8U + 3U].y_mode = 1U;
            cells[2U * 8U + 2U].y_mode = 2U;
            CHECK(av1_block_state_availability(&state, 2U, 3U, 2U, 2U,
                                     &availability) == AVIFDEC_OK);
            av1_tile_cdfs_init(&cdfs);
            CHECK(cdfs.intra.y_mode[1][2][13] == 0U);
            CHECK(av1_symbol_init(&decoder, &span, 1U, 0U, span.size, 0) == AVIFDEC_OK);
            CHECK(av1_tile_read_intra_frame_y_mode(
                    &decoder, &cdfs, &state, &availability, 2U, 3U, &value) ==
                AVIFDEC_OK);
            CHECK(value < AV1_INTRA_MODES && cdfs.intra.y_mode[1][2][13] == 1U &&
                cdfs.intra.y_mode[0][0][13] == 0U);

        av1_block_trace_init(&trace);
        avifdec_memory_fill(&fields, 0U, sizeof(fields));
        fields.row = 1U;
        fields.column = 2U;
        fields.width = 1U;
        fields.height = 1U;
        fields.y_mode = 3U;
        fields.uv_mode = 4U;
        fields.tx_size = 1U;
        CHECK(av1_block_state_record(&state, &fields, &trace) == AVIFDEC_OK);
        fields.row = 5U;
        fields.column = 7U;
        fields.width = 4U;
        fields.height = 4U;
        fields.segment_id = 2U;
        fields.skip = 1U;
        fields.y_mode = 0U;
        fields.uv_mode = 0U;
        fields.tx_size = 0U;
        CHECK(av1_block_state_record(&state, &fields, &trace) == AVIFDEC_OK);
        CHECK(trace.block_count == 2U);
        CHECK(cells[1U * 8U + 2U].y_mode == 3U && cells[1U * 8U + 2U].width == 1U);
        CHECK(cells[5U * 8U + 7U].segment_id == 2U && cells[5U * 8U + 7U].width == 4U);
        first_checksum = trace.checksum;

        av1_block_trace_init(&trace);
        fields.row = 1U;
        fields.column = 2U;
        fields.width = 1U;
        fields.height = 1U;
        fields.segment_id = 0U;
        fields.skip = 0U;
        fields.y_mode = 3U;
        fields.uv_mode = 4U;
        fields.tx_size = 1U;
        CHECK(av1_block_state_record(&state, &fields, &trace) == AVIFDEC_OK);
        fields.row = 5U;
        fields.column = 7U;
        fields.width = 4U;
        fields.height = 4U;
        fields.segment_id = 2U;
        fields.skip = 1U;
        fields.y_mode = 0U;
        fields.uv_mode = 0U;
        fields.tx_size = 0U;
        CHECK(av1_block_state_record(&state, &fields, &trace) == AVIFDEC_OK);
        CHECK(trace.checksum == first_checksum);
        return 0;
    }

        typedef struct {
            Av1TileCdfs *expected_cdfs;
            size_t calls;
        } TileModeCallbackState;

        static AvifdecStatus tile_mode_callback(void *user_data,
                                                 Av1SymbolDecoder *decoder,
                                                 Av1TileCdfs *cdfs,
                                                 uint32_t row,
                                                 uint32_t column,
                                                 uint32_t width,
                                                 uint32_t height) {
            TileModeCallbackState *state = (TileModeCallbackState *)user_data;

            if (decoder == 0 || cdfs != state->expected_cdfs || row != 0U ||
                column != 0U || width != 16U || height != 16U) {
                return AVIFDEC_INVALID_DATA;
            }
            ++state->calls;
            return AVIFDEC_OK;
        }

        typedef struct {
            size_t calls;
            Av1BlockTraceFields block;
        } BeforeResidualState;

        static AvifdecStatus before_residual(void *user_data,
                                              Av1SymbolDecoder *decoder,
                                              const Av1BlockTraceFields *block) {
            BeforeResidualState *state = (BeforeResidualState *)user_data;

            if (decoder == 0 || decoder->status != AVIFDEC_OK || block == 0) {
                return AVIFDEC_INVALID_DATA;
            }
            state->block = *block;
            ++state->calls;
            return AVIFDEC_IO_ERROR;
        }

        static void force_partition_symbol(uint16_t cdfs[4][11],
                                           size_t symbols,
                                           size_t forced_symbol) {
            unsigned int context;

            for (context = 0U; context < 4U; ++context) {
                force_cdf_symbol(cdfs[context], symbols, forced_symbol);
            }
        }

        static void force_key_y_mode(Av1TileCdfs *cdfs, size_t mode) {
            unsigned int above;
            unsigned int left;

            for (above = 0U; above < AV1_INTRA_MODE_CONTEXTS; ++above) {
                for (left = 0U; left < AV1_INTRA_MODE_CONTEXTS; ++left) {
                    force_cdf_symbol(cdfs->intra.y_mode[above][left],
                                     AV1_INTRA_MODES, mode);
                }
            }
        }

        static int test_av1_tile_key_frame_modes(void) {
            static const unsigned char tile_data[16] = {
                0x40U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U
            };
            Av1BlockCell cells[16U * 16U];
            uint8_t widths[16U * 16U];
            uint8_t heights[16U * 16U];
            Av1BlockState block_state;
            Av1BlockTrace block_trace;
            Av1PartitionTrace partition_trace;
            Av1TileCdfs frame_cdfs;
            Av1TileCdfs tile_cdfs;
            Av1TilePartitionConfig partition_config;
            Av1TileModeConfig mode_config;
            BeforeResidualState boundary;
            uint8_t feature_enabled[8][8];
            int16_t feature_data[8][8];
            uint8_t lossless_array[8];
            uint8_t palette_map[64U * 64U];
            AvifdecSpan span;

            span.data = tile_data;
            span.size = sizeof(tile_data);
            span.file_offset = 0U;
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&partition_config, 0U, sizeof(partition_config));
            partition_config.spans = &span;
            partition_config.span_count = 1U;
            partition_config.size = sizeof(tile_data);
            partition_config.mi_rows = 16U;
            partition_config.mi_columns = 16U;
            partition_config.tile_row_end = 16U;
            partition_config.tile_column_end = 16U;
            partition_config.superblock_mi = 16U;
            partition_config.block_widths = widths;
            partition_config.block_heights = heights;
            partition_config.grid_capacity = sizeof(widths);
            partition_config.max_partition_nodes = 512U;
            CHECK(av1_block_state_init(&block_state, 16U, 16U, cells,
                                       16U * 16U, 1, 1, 1) == AVIFDEC_OK);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(&tile_cdfs, 0U, sizeof(tile_cdfs));
            avifdec_memory_fill(&mode_config, 0U, sizeof(mode_config));
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            avifdec_memory_fill(feature_enabled, 0U, sizeof(feature_enabled));
            avifdec_memory_fill(feature_data, 0U, sizeof(feature_data));
            avifdec_memory_fill(lossless_array, 0U, sizeof(lossless_array));
            mode_config.block_state = &block_state;
            mode_config.block_trace = &block_trace;
            mode_config.before_residual = before_residual;
            mode_config.user_data = &boundary;
            mode_config.bit_depth = 8U;
            mode_config.palette_map = palette_map;
            mode_config.palette_map_capacity = sizeof(palette_map);
            mode_config.superblock_mi = 16U;

            mode_config.segmentation_enabled = 1U;
            mode_config.feature_enabled = &feature_enabled[0][0];
            mode_config.feature_data = &feature_data[0][0];
            mode_config.lossless_array = lossless_array;
            mode_config.base_q_index = 20U;
            feature_enabled[0][0] = 1U;
            feature_data[0][0] = -20;
            lossless_array[0] = 1U;
            CHECK(av1_tile_decode_modes(
                      &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && block_trace.block_count == 1U &&
                boundary.block.segment_id == 0U && boundary.block.q_index == 0U &&
                boundary.block.lossless == 1U && boundary.block.tx_size == 0U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 1, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_cdf_symbol(frame_cdfs.segment_id[0], 8U, 1U);
            mode_config.seg_id_pre_skip = 1U;
            mode_config.last_active_segment = 1U;
            feature_enabled[1][6] = 1U;
            CHECK(av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && block_trace.block_count == 1U &&
                boundary.block.segment_id == 1U && boundary.block.skip == 1U &&
                boundary.block.q_index == 20U &&
                boundary.block.lossless == 0U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 1, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            mode_config.segmentation_enabled = 0U;
            mode_config.seg_id_pre_skip = 0U;
            mode_config.last_active_segment = 0U;
            mode_config.base_q_index = 0U;
            mode_config.lossless = 1U;
            CHECK(av1_tile_decode_modes(
                      &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                      &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && block_trace.block_count == 1U);
            CHECK(boundary.block.row == 0U && boundary.block.column == 0U &&
                  boundary.block.width == 16U && boundary.block.height == 16U &&
                  boundary.block.y_mode < AV1_INTRA_MODES &&
                  boundary.block.tx_size == 0U);
            CHECK(cells[0].y_mode == boundary.block.y_mode && cells[0].tx_size == 0U);
            CHECK(partition_trace.partition_nodes == 0U &&
                partition_trace.block_count == 0U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 1, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_key_y_mode(&frame_cdfs, 1U);
            force_cdf_symbol(frame_cdfs.intra.angle_delta[0], 7U, 6U);
            force_cdf_symbol(frame_cdfs.tx64[0], 3U, 0U);
            mode_config.lossless = 0U;
            mode_config.tx_mode = 2U;
            CHECK(av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && boundary.block.y_mode == 1U &&
                boundary.block.angle_delta_y == 3 && boundary.block.tx_size == 4U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 0, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_partition_symbol(frame_cdfs.partition.width64, 10U,
                             AV1_PARTITION_SPLIT);
            force_partition_symbol(frame_cdfs.partition.width32, 10U,
                             AV1_PARTITION_SPLIT);
            force_partition_symbol(frame_cdfs.partition.width16, 10U,
                             AV1_PARTITION_SPLIT);
            force_cdf_symbol(frame_cdfs.partition.width8[0], 4U,
                             AV1_PARTITION_NONE);
            force_cdf_symbol(frame_cdfs.partition.width8[1], 4U,
                             AV1_PARTITION_NONE);
            force_cdf_symbol(frame_cdfs.partition.width8[2], 4U,
                             AV1_PARTITION_NONE);
            force_cdf_symbol(frame_cdfs.partition.width8[3], 4U,
                             AV1_PARTITION_NONE);
            force_key_y_mode(&frame_cdfs, 0U);
            force_cdf_symbol(frame_cdfs.intra.uv_mode_cfl_allowed[0],
                         AV1_UV_INTRA_MODES_CFL_ALLOWED, 13U);
            force_cdf_symbol(frame_cdfs.intra.cfl_sign,
                         AV1_CFL_JOINT_SIGNS, 7U);
            force_cdf_symbol(frame_cdfs.intra.cfl_alpha[5],
                         AV1_CFL_ALPHABET_SIZE, 0U);
            mode_config.lossless = 1U;
            mode_config.tx_mode = 0U;
            CHECK(av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && boundary.block.width == 2U &&
                boundary.block.height == 2U && boundary.block.lossless == 1U &&
                boundary.block.uv_mode == 13U &&
                boundary.block.cfl_alpha_u == 1 && boundary.block.cfl_alpha_v == 1);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 0, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_partition_symbol(frame_cdfs.partition.width64, 10U,
                             AV1_PARTITION_SPLIT);
            force_partition_symbol(frame_cdfs.partition.width32, 10U,
                             AV1_PARTITION_SPLIT);
            force_partition_symbol(frame_cdfs.partition.width16, 10U,
                             AV1_PARTITION_NONE);
            force_key_y_mode(&frame_cdfs, 0U);
            force_cdf_symbol(frame_cdfs.intra.uv_mode_cfl_allowed[0],
                         AV1_UV_INTRA_MODES_CFL_ALLOWED, 13U);
            force_cdf_symbol(frame_cdfs.intra.cfl_sign,
                         AV1_CFL_JOINT_SIGNS, 7U);
            force_cdf_symbol(frame_cdfs.intra.cfl_alpha[5],
                         AV1_CFL_ALPHABET_SIZE, 0U);
            force_cdf_symbol(frame_cdfs.intra.filter_intra[6], 2U, 1U);
            force_cdf_symbol(frame_cdfs.intra.filter_intra_mode, 5U, 4U);
            mode_config.lossless = 0U;
            mode_config.tx_mode = 1U;
            mode_config.enable_filter_intra = 1U;
            CHECK(av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_IO_ERROR);
            CHECK(boundary.calls == 1U && boundary.block.width == 4U &&
                boundary.block.height == 4U && boundary.block.uv_mode == 13U &&
                boundary.block.cfl_alpha_u == 1 && boundary.block.cfl_alpha_v == 1 &&
                boundary.block.use_filter_intra == 1U &&
                boundary.block.filter_intra_mode == 4U &&
                boundary.block.tx_size == 2U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 1, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_key_y_mode(&frame_cdfs, 0U);
            force_cdf_symbol(frame_cdfs.intrabc, 2U, 1U);
            mode_config.allow_intrabc = 1U;
            mode_config.enable_filter_intra = 0U;
            mode_config.lossless = 1U;
            mode_config.tx_mode = 0U;
            CHECK(av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace) == AVIFDEC_INVALID_DATA);
            CHECK(boundary.calls == 0U && block_trace.block_count == 0U &&
                tile_cdfs.intra.y_mode[0][0][13] == 0U);

            av1_block_state_init(&block_state, 16U, 16U, cells,
                           16U * 16U, 1, 1, 1);
            av1_block_trace_init(&block_trace);
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&boundary, 0U, sizeof(boundary));
            force_key_y_mode(&frame_cdfs, 0U);
            force_cdf_symbol(frame_cdfs.palette_y_mode[6][0], 2U, 1U);
            force_cdf_symbol(frame_cdfs.palette_y_size[6], 7U, 0U);
            mode_config.allow_intrabc = 0U;
            mode_config.allow_screen_content_tools = 1U;
            {
                AvifdecStatus palette_status = av1_tile_decode_modes(
                    &partition_config, &mode_config, &frame_cdfs, &tile_cdfs,
                    &partition_trace);
                CHECK(palette_status == AVIFDEC_IO_ERROR);
            }
            CHECK(boundary.calls == 1U && block_trace.block_count == 1U &&
                boundary.block.palette_size_y == 2U &&
                tile_cdfs.palette_y_mode[6][0][2] == 1U &&
                tile_cdfs.intra.filter_intra[12][2] == 0U);
            return 0;
        }

        static int test_av1_tile_partition_decode(void) {
            static const unsigned char tile_data[2] = { 0x40U, 0x00U };
            static const unsigned char bad_tail[2] = { 0x40U, 0x01U };
            uint8_t widths[16U * 16U];
            uint8_t heights[16U * 16U];
            AvifdecSpan span;
            Av1TileCdfs frame_cdfs;
            Av1TileCdfs tile_cdfs;
            Av1TilePartitionConfig config;
            Av1PartitionTrace trace;
            TileModeCallbackState callback_state;

            span.data = tile_data;
            span.size = sizeof(tile_data);
            span.file_offset = 0U;
            av1_tile_cdfs_init(&frame_cdfs);
            avifdec_memory_fill(&tile_cdfs, 0U, sizeof(tile_cdfs));
            avifdec_memory_fill(widths, 0U, sizeof(widths));
            avifdec_memory_fill(heights, 0U, sizeof(heights));
            avifdec_memory_fill(&config, 0U, sizeof(config));
            config.spans = &span;
            config.span_count = 1U;
            config.size = sizeof(tile_data);
            config.mi_rows = 16U;
            config.mi_columns = 16U;
            config.tile_row_end = 16U;
            config.tile_column_end = 16U;
            config.superblock_mi = 16U;
            config.block_widths = widths;
            config.block_heights = heights;
            config.grid_capacity = sizeof(widths);
            config.max_partition_nodes = 512U;
            CHECK(av1_tile_decode_partitions(&config, &frame_cdfs, &tile_cdfs, &trace) ==
                AVIFDEC_OK);
            CHECK(trace.partition_nodes == 1U && trace.block_count == 1U &&
                widths[0] == 16U && heights[15U * 16U + 15U] == 16U);
            CHECK(av1_tile_cdfs_checksum(&frame_cdfs) != av1_tile_cdfs_checksum(&tile_cdfs));
            callback_state.expected_cdfs = &tile_cdfs;
            callback_state.calls = 0U;
            config.decode_mode_block = tile_mode_callback;
            config.user_data = &callback_state;
            CHECK(av1_tile_decode_partitions(&config, &frame_cdfs, &tile_cdfs, &trace) ==
                AVIFDEC_OK);
            CHECK(callback_state.calls == 1U);
            config.decode_mode_block = 0;
            config.user_data = 0;
            span.data = bad_tail;
            CHECK(av1_tile_decode_partitions(&config, &frame_cdfs, &tile_cdfs, &trace) ==
                AVIFDEC_INVALID_DATA);
            span.data = tile_data;
            tile_cdfs = frame_cdfs;
            config.size = 3U;
            CHECK(av1_tile_decode_partitions(&config, &frame_cdfs, &tile_cdfs, &trace) ==
                AVIFDEC_TRUNCATED);
            return 0;
        }

    static int test_avif_entropy_trace(void) {
        static const unsigned char file[] = {
          0x00U, 0x00U, 0x00U, 0x20U, 0x66U, 0x74U, 0x79U, 0x70U,
          0x61U, 0x76U, 0x69U, 0x66U, 0x00U, 0x00U, 0x00U, 0x00U,
          0x61U, 0x76U, 0x69U, 0x66U, 0x6dU, 0x69U, 0x66U, 0x31U,
          0x6dU, 0x69U, 0x61U, 0x66U, 0x4dU, 0x41U, 0x31U, 0x41U,
          0x00U, 0x00U, 0x00U, 0xebU, 0x6dU, 0x65U, 0x74U, 0x61U,
          0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x21U,
          0x68U, 0x64U, 0x6cU, 0x72U, 0x00U, 0x00U, 0x00U, 0x00U,
          0x00U, 0x00U, 0x00U, 0x00U, 0x70U, 0x69U, 0x63U, 0x74U,
          0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x0eU,
          0x70U, 0x69U, 0x74U, 0x6dU, 0x00U, 0x00U, 0x00U, 0x00U,
          0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x1eU, 0x69U, 0x6cU,
          0x6fU, 0x63U, 0x00U, 0x00U, 0x00U, 0x00U, 0x44U, 0x00U,
          0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
          0x00U, 0x00U, 0x01U, 0x13U, 0x00U, 0x00U, 0x00U, 0x18U,
          0x00U, 0x00U, 0x00U, 0x28U, 0x69U, 0x69U, 0x6eU, 0x66U,
          0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U,
          0x00U, 0x1aU, 0x69U, 0x6eU, 0x66U, 0x65U, 0x02U, 0x00U,
          0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x61U, 0x76U,
          0x30U, 0x31U, 0x43U, 0x6fU, 0x6cU, 0x6fU, 0x72U, 0x00U,
          0x00U, 0x00U, 0x00U, 0x6aU, 0x69U, 0x70U, 0x72U, 0x70U,
          0x00U, 0x00U, 0x00U, 0x4bU, 0x69U, 0x70U, 0x63U, 0x6fU,
          0x00U, 0x00U, 0x00U, 0x14U, 0x69U, 0x73U, 0x70U, 0x65U,
          0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U,
          0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x10U,
          0x70U, 0x69U, 0x78U, 0x69U, 0x00U, 0x00U, 0x00U, 0x00U,
          0x03U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U, 0x00U, 0x0cU,
          0x61U, 0x76U, 0x31U, 0x43U, 0x81U, 0x20U, 0x00U, 0x00U,
          0x00U, 0x00U, 0x00U, 0x13U, 0x63U, 0x6fU, 0x6cU, 0x72U,
          0x6eU, 0x63U, 0x6cU, 0x78U, 0x00U, 0x01U, 0x00U, 0x0dU,
          0x00U, 0x06U, 0x80U, 0x00U, 0x00U, 0x00U, 0x17U, 0x69U,
          0x70U, 0x6dU, 0x61U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
          0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x04U, 0x01U, 0x02U,
          0x83U, 0x04U, 0x00U, 0x00U, 0x00U, 0x20U, 0x6dU, 0x64U,
          0x61U, 0x74U, 0x12U, 0x00U, 0x0aU, 0x04U, 0x38U, 0x00U,
          0x06U, 0x09U, 0x32U, 0x0eU, 0x1eU, 0x40U, 0x3fU, 0xffU,
          0xffU, 0xc4U, 0x00U, 0x00U, 0xafU, 0x28U, 0xc4U, 0x04U,
          0x06U, 0x40U
        };
        static unsigned char corrupt[sizeof(file)];
        static unsigned char workspace[700000U];
        uint16_t traced_planes[3];
        uint16_t untraced_planes[3];
        AvifdecEntropyTrace trace;
        AvifdecEntropyTrace parallel_trace;
        AvifdecImageInfo info;
        AvifdecImageInfo parallel_info;
        AvifdecImage traced_image;
        AvifdecImage untraced_image;
        TestReverseExecutorState executor_state = { 0U, 4U };
        AvifdecExecutor executor = {
            &executor_state, 4U, test_reverse_parallel_for
        };
        AvifdecError error;
        size_t workspace_offset;

        CHECK(avifdec_query(file, sizeof(file), 0, 0, 0U, &info, &error) ==
            AVIFDEC_OK);
        CHECK(info.reduced_still_picture_header == 1U &&
            info.workspace_plane_buffer_count == 1U &&
            info.workspace_required == 247442U &&
            info.workspace_required <= sizeof(workspace));
        CHECK(avifdec_trace(
                  file, sizeof(file), 0, workspace,
                  info.workspace_required - 1U,
                  &trace, &error) == AVIFDEC_OUT_OF_MEMORY);
        CHECK(avifdec_trace(
                  file, sizeof(file), 0, workspace,
                  info.workspace_required,
                  &trace, &error) == AVIFDEC_OK);
        CHECK(trace.reconstruction_checksum == trace.deblocked_checksum &&
              trace.deblocked_checksum == trace.cdef_checksum &&
              trace.cdef_checksum == trace.superres_checksum &&
              trace.superres_checksum == trace.restoration_checksum);
        CHECK(avifdec_query_ex(
            file, sizeof(file), 0, &executor, 0, 0U,
            &parallel_info, &error) == AVIFDEC_OK);
        CHECK(parallel_info.workspace_required <= sizeof(workspace));
        CHECK(avifdec_trace_ex(
            file, sizeof(file), 0, &executor, workspace,
            parallel_info.workspace_required, &parallel_trace,
            &error) == AVIFDEC_OK);
        CHECK(executor_state.calls != 0U &&
              avifdec_memory_compare(
                  &trace, &parallel_trace,
                  sizeof(trace)) == 0);
        CHECK(trace.tile_count == 1U && trace.partition_nodes == 4U &&
            trace.block_count == 1U && trace.transform_count == 3U &&
            trace.nonzero_transform_count == 3U &&
            trace.coefficient_count == 3U &&
            trace.checksum == 0x786a84f4336e4869ULL);
        avifdec_memory_fill(&traced_image, 0U, sizeof(traced_image));
        avifdec_memory_fill(&untraced_image, 0U, sizeof(untraced_image));
        traced_image.planes[0] = &traced_planes[0];
        traced_image.planes[1] = &traced_planes[1];
        traced_image.planes[2] = &traced_planes[2];
        untraced_image.planes[0] = &untraced_planes[0];
        untraced_image.planes[1] = &untraced_planes[1];
        untraced_image.planes[2] = &untraced_planes[2];
        traced_image.strides[0] = traced_image.strides[1] =
            traced_image.strides[2] = 1U;
        untraced_image.strides[0] = untraced_image.strides[1] =
            untraced_image.strides[2] = 1U;
        CHECK(avifdec_decode(file, sizeof(file), 0, workspace,
                    info.workspace_required, &traced_image, &trace, &error) ==
            AVIFDEC_OK);
        CHECK(avifdec_decode(file, sizeof(file), 0, workspace,
                    info.workspace_required, &untraced_image, 0, &error) ==
            AVIFDEC_OK);
        CHECK(avifdec_decode(file, sizeof(file), 0, workspace,
                    info.workspace_required, &untraced_image, 0, &error) ==
            AVIFDEC_OK);
        CHECK(avifdec_decode(file, sizeof(file), 0, workspace,
                    info.workspace_required - 1U,
                    &untraced_image, 0, &error) ==
            AVIFDEC_OUT_OF_MEMORY);
        for (workspace_offset = 0U;
             workspace_offset < 16U;
             ++workspace_offset) {
            CHECK(avifdec_decode(
                file, sizeof(file), 0, workspace + workspace_offset,
                info.workspace_required, &untraced_image, 0, &error) ==
                AVIFDEC_OK);
            CHECK(avifdec_decode_ex(
                file, sizeof(file), 0, &executor,
                workspace + workspace_offset,
                parallel_info.workspace_required,
                &untraced_image, 0, &error) == AVIFDEC_OK);
        }
        CHECK(traced_image.widths[0] == untraced_image.widths[0] &&
            traced_image.heights[0] == untraced_image.heights[0] &&
            traced_planes[0] == untraced_planes[0] &&
            traced_planes[1] == untraced_planes[1] &&
            traced_planes[2] == untraced_planes[2]);
        avifdec_memory_copy(corrupt, file, sizeof(file));
        corrupt[sizeof(corrupt) - 1U] ^= 1U;
        CHECK(avifdec_trace(corrupt, sizeof(corrupt), 0, workspace,
                    sizeof(workspace), &trace, &error) ==
            AVIFDEC_INVALID_DATA);
        return 0;
    }

int main(int argc, char **argv) {
    int result;

    (void)argc;
    (void)argv;
    result = test_checked_arithmetic();
    if (result != 0) return result;
    result = test_memory_and_endian();
    if (result != 0) return result;
    result = test_av1_reference_state();
    if (result != 0) return result;
    result = test_av1_motion_vectors();
    if (result != 0) return result;
    result = test_av1_inter_prediction();
    if (result != 0) return result;
    result = test_av1_warp_and_obmc();
    if (result != 0) return result;
    result = test_av1_nondirectional_predictors();
    if (result != 0) return result;
    result = test_av1_directional_predictors();
    if (result != 0) return result;
    result = test_av1_cfl_predictor();
    if (result != 0) return result;
    result = test_av1_prediction_edges_and_tools();
    if (result != 0) return result;
    result = test_av1_predictor_matrix();
    if (result != 0) return result;
    result = test_byte_reader();
    if (result != 0) return result;
    result = test_bit_reader();
    if (result != 0) return result;
    result = test_av1_metadata();
    if (result != 0) return result;
    result = test_av1_profile_and_level();
    if (result != 0) return result;
    result = test_av1_film_grain();
    if (result != 0) return result;
    result = test_arena();
    if (result != 0) return result;
    result = test_avif_presentation_helpers();
    if (result != 0) return result;
    result = test_avif_sample_transform();
    if (result != 0) return result;
    result = test_avif_rgb_conversion();
    if (result != 0) return result;
    result = test_png_writer();
    if (result != 0) return result;
    result = test_bmff_valid();
    if (result != 0) return result;
    result = test_bmff_large_uuid();
    if (result != 0) return result;
    result = test_bmff_invalid();
    if (result != 0) return result;
    result = test_bmff_mutation_sweep();
    if (result != 0) return result;
    result = test_avif_query_extents();
    if (result != 0) return result;
    result = test_avif_item_graph();
    if (result != 0) return result;
    result = test_avif_sample_transform_executor();
    if (result != 0) return result;
    result = test_av1_obu_errors();
    if (result != 0) return result;
    result = test_av1_regular_header();
    if (result != 0) return result;
    result = test_av1_format_matrix();
    if (result != 0) return result;
    result = test_av1_symbol_decoder();
    if (result != 0) return result;
    result = test_av1_coeff_cdfs();
    if (result != 0) return result;
    result = test_av1_coeff_parser();
    if (result != 0) return result;
    result = test_av1_dequantization();
    if (result != 0) return result;
    result = test_av1_inverse_transforms();
    if (result != 0) return result;
    result = test_av1_dsp_add_residual();
    if (result != 0) return result;
    result = test_av1_dsp_inverse_dct4();
    if (result != 0) return result;
    result = test_av1_dsp_inverse_dct8();
    if (result != 0) return result;
    result = test_av1_dsp_inverse_dct16();
    if (result != 0) return result;
    result = test_av1_loop_filter();
    if (result != 0) return result;
    result = test_av1_cdef();
    if (result != 0) return result;
    result = test_av1_cdef_interior();
    if (result != 0) return result;
    result = test_av1_superres();
    if (result != 0) return result;
    result = test_av1_loop_restoration();
    if (result != 0) return result;
    result = test_av1_intra_cdfs();
    if (result != 0) return result;
    result = test_av1_partition_engine();
    if (result != 0) return result;
    result = test_av1_block_state();
    if (result != 0) return result;
    result = test_av1_tile_partition_decode();
    if (result != 0) return result;
    result = test_avif_entropy_trace();
    if (result != 0) return result;
    return test_av1_tile_key_frame_modes();
}