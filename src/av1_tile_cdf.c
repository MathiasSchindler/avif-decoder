#include "av1_tile.h"
#include "base.h"

#include "av1_palette_tables.inc"

static const uint16_t av1_default_skip[3][3] = {
    { 31671U, 32768U, 0U },
    { 16515U, 32768U, 0U },
    { 4576U, 32768U, 0U }
};

static const uint16_t av1_default_segment_id[3][9] = {
    { 5622U, 7893U, 16093U, 18233U, 27809U, 28373U, 32533U, 32768U, 0U },
    { 14274U, 18230U, 22557U, 24935U, 29980U, 30851U, 32344U, 32768U, 0U },
    { 27527U, 28487U, 28723U, 28890U, 32397U, 32647U, 32679U, 32768U, 0U }
};

static const uint16_t av1_default_delta[5] = {
    28160U, 32120U, 32677U, 32768U, 0U
};

static const uint16_t av1_default_txfm_partition[21][3] = {
    { 28581U, 32768U, 0U }, { 23846U, 32768U, 0U },
    { 20847U, 32768U, 0U }, { 24315U, 32768U, 0U },
    { 18196U, 32768U, 0U }, { 12133U, 32768U, 0U },
    { 18791U, 32768U, 0U }, { 10887U, 32768U, 0U },
    { 11005U, 32768U, 0U }, { 27179U, 32768U, 0U },
    { 20004U, 32768U, 0U }, { 11281U, 32768U, 0U },
    { 26549U, 32768U, 0U }, { 19308U, 32768U, 0U },
    { 14224U, 32768U, 0U }, { 28015U, 32768U, 0U },
    { 21546U, 32768U, 0U }, { 14400U, 32768U, 0U },
    { 28165U, 32768U, 0U }, { 22401U, 32768U, 0U },
    { 16088U, 32768U, 0U }
};

static const uint16_t av1_default_y_mode[4][14] = {
    { 22801U, 23489U, 24293U, 24756U, 25601U, 26123U, 26606U,
      27418U, 27945U, 29228U, 29685U, 30349U, 32768U, 0U },
    { 18673U, 19845U, 22631U, 23318U, 23950U, 24649U, 25527U,
      27364U, 28152U, 29701U, 29984U, 30852U, 32768U, 0U },
    { 19770U, 20979U, 23396U, 23939U, 24241U, 24654U, 25136U,
      27073U, 27830U, 29360U, 29730U, 30659U, 32768U, 0U },
    { 20155U, 21301U, 22838U, 23178U, 23261U, 23533U, 23703U,
      24804U, 25352U, 26575U, 27016U, 28049U, 32768U, 0U }
};

static const uint16_t av1_default_angle_delta[8][8] = {
    { 2180U, 5032U, 7567U, 22776U, 26989U, 30217U, 32768U, 0U },
    { 2301U, 5608U, 8801U, 23487U, 26974U, 30330U, 32768U, 0U },
    { 3780U, 11018U, 13699U, 19354U, 23083U, 31286U, 32768U, 0U },
    { 4581U, 11226U, 15147U, 17138U, 21834U, 28397U, 32768U, 0U },
    { 1737U, 10927U, 14509U, 19588U, 22745U, 28823U, 32768U, 0U },
    { 2664U, 10176U, 12485U, 17650U, 21600U, 30495U, 32768U, 0U },
    { 2240U, 11096U, 15453U, 20341U, 22561U, 28917U, 32768U, 0U },
    { 3605U, 10428U, 12459U, 17676U, 21244U, 30655U, 32768U, 0U }
};

static const uint16_t av1_default_tx8[3][3] = {
    { 19968U, 32768U, 0U }, { 19968U, 32768U, 0U }, { 24320U, 32768U, 0U }
};

static const uint16_t av1_default_tx16[3][4] = {
    { 12272U, 30172U, 32768U, 0U }, { 12272U, 30172U, 32768U, 0U },
    { 18677U, 30848U, 32768U, 0U }
};

static const uint16_t av1_default_tx32[3][4] = {
    { 12986U, 15180U, 32768U, 0U }, { 12986U, 15180U, 32768U, 0U },
    { 24302U, 25602U, 32768U, 0U }
};

static const uint16_t av1_default_tx64[3][4] = {
    { 5782U, 11475U, 32768U, 0U }, { 5782U, 11475U, 32768U, 0U },
    { 16803U, 22759U, 32768U, 0U }
};

static const uint16_t av1_default_palette_y_mode[7][3][3] = {
    { { 31676U, 32768U, 0U }, { 3419U, 32768U, 0U }, { 1261U, 32768U, 0U } },
    { { 31912U, 32768U, 0U }, { 2859U, 32768U, 0U }, { 980U, 32768U, 0U } },
    { { 31823U, 32768U, 0U }, { 3400U, 32768U, 0U }, { 781U, 32768U, 0U } },
    { { 32030U, 32768U, 0U }, { 3561U, 32768U, 0U }, { 904U, 32768U, 0U } },
    { { 32309U, 32768U, 0U }, { 7337U, 32768U, 0U }, { 1462U, 32768U, 0U } },
    { { 32265U, 32768U, 0U }, { 4015U, 32768U, 0U }, { 1521U, 32768U, 0U } },
    { { 32450U, 32768U, 0U }, { 7946U, 32768U, 0U }, { 129U, 32768U, 0U } }
};

static const uint16_t av1_default_palette_uv_mode[2][3] = {
    { 32461U, 32768U, 0U }, { 21488U, 32768U, 0U }
};

static void av1_tile_checksum_bytes(uint64_t *checksum,
                                    const void *data,
                                    size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    for (index = 0U; index < size; ++index) {
        *checksum ^= bytes[index];
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

void av1_tile_cdfs_init_frame(Av1TileCdfs *cdfs, uint8_t base_q_index) {
    static const uint16_t intrabc[3] = { 30531U, 32768U, 0U };
    static const uint16_t use_wiener[3] = { 11570U, 32768U, 0U };
    static const uint16_t use_sgrproj[3] = { 16855U, 32768U, 0U };
    static const uint16_t restoration_type[4] = {
        9413U, 22581U, 32768U, 0U
    };

    if (cdfs == 0) return;
    av1_partition_cdfs_init(&cdfs->partition);
    avifdec_memory_copy(cdfs->skip, av1_default_skip, sizeof(cdfs->skip));
    avifdec_memory_copy(cdfs->segment_id, av1_default_segment_id,
                        sizeof(cdfs->segment_id));
    avifdec_memory_copy(cdfs->delta_q, av1_default_delta, sizeof(cdfs->delta_q));
    avifdec_memory_copy(cdfs->delta_lf, av1_default_delta,     sizeof(cdfs->delta_lf));
    avifdec_memory_copy(cdfs->txfm_partition,
    av1_default_txfm_partition,
    sizeof(cdfs->txfm_partition));
    avifdec_memory_copy(cdfs->y_mode, av1_default_y_mode, sizeof(cdfs->y_mode));
    avifdec_memory_copy(cdfs->angle_delta, av1_default_angle_delta,
                        sizeof(cdfs->angle_delta));
    avifdec_memory_copy(cdfs->intrabc, intrabc, sizeof(cdfs->intrabc));
    avifdec_memory_copy(cdfs->tx8, av1_default_tx8, sizeof(cdfs->tx8));
    avifdec_memory_copy(cdfs->tx16, av1_default_tx16, sizeof(cdfs->tx16));
    avifdec_memory_copy(cdfs->tx32, av1_default_tx32, sizeof(cdfs->tx32));
    avifdec_memory_copy(cdfs->tx64, av1_default_tx64, sizeof(cdfs->tx64));
    avifdec_memory_copy(cdfs->palette_y_mode, av1_default_palette_y_mode,
                        sizeof(cdfs->palette_y_mode));
    avifdec_memory_copy(cdfs->palette_uv_mode, av1_default_palette_uv_mode,
                        sizeof(cdfs->palette_uv_mode));
    avifdec_memory_copy(cdfs->palette_y_size, av1_default_palette_y_size,
                        sizeof(cdfs->palette_y_size));
    avifdec_memory_copy(cdfs->palette_uv_size, av1_default_palette_uv_size,
                        sizeof(cdfs->palette_uv_size));
    avifdec_memory_copy(cdfs->palette_color, av1_default_palette_color,
                        sizeof(cdfs->palette_color));
    avifdec_memory_copy(cdfs->use_wiener, use_wiener,
                        sizeof(cdfs->use_wiener));
    avifdec_memory_copy(cdfs->use_sgrproj, use_sgrproj,
                        sizeof(cdfs->use_sgrproj));
    avifdec_memory_copy(cdfs->restoration_type, restoration_type,
                        sizeof(cdfs->restoration_type));
    av1_inter_cdfs_init(&cdfs->inter);
    avifdec_memory_copy(cdfs->dv_joint, cdfs->inter.mv_joint,
                        sizeof(cdfs->dv_joint));
    avifdec_memory_copy(cdfs->dv_component, cdfs->inter.mv_component,
                        sizeof(cdfs->dv_component));
    av1_intra_cdfs_init(&cdfs->intra);
    av1_coeff_cdfs_init(&cdfs->coeff, base_q_index);
}

void av1_tile_cdfs_init(Av1TileCdfs *cdfs) {
    av1_tile_cdfs_init_frame(cdfs, 0U);
}

static void av1_tile_reset_cdf_counts(uint16_t *cdfs,
                                      size_t size,
                                      size_t symbols,
                                      size_t stride) {
    size_t index;
    size_t count;

    if (cdfs == 0 || symbols < 2U || stride <= symbols ||
        size % (stride * sizeof(*cdfs)) != 0U) {
        return;
    }
    count = size / (stride * sizeof(*cdfs));
    for (index = 0U; index < count; ++index) {
        cdfs[index * stride + symbols] = 0U;
    }
}

#define AV1_RESET_CDFS(field, symbols) \
    av1_tile_reset_cdf_counts((uint16_t *)(field), sizeof(field), \
                              (symbols), (symbols) + 1U)
#define AV1_RESET_CDFS_STRIDE(field, symbols, stride) \
    av1_tile_reset_cdf_counts((uint16_t *)(field), sizeof(field), \
                              (symbols), (stride))

void av1_tile_cdfs_reset_counts(Av1TileCdfs *cdfs) {
    unsigned int palette_size;
    unsigned int component;

    if (cdfs == 0) return;
    AV1_RESET_CDFS(cdfs->partition.width8, 4U);
    AV1_RESET_CDFS(cdfs->partition.width16, 10U);
    AV1_RESET_CDFS(cdfs->partition.width32, 10U);
    AV1_RESET_CDFS(cdfs->partition.width64, 10U);
    AV1_RESET_CDFS(cdfs->partition.width128, 8U);
    AV1_RESET_CDFS(cdfs->skip, 2U);
    AV1_RESET_CDFS(cdfs->segment_id, 8U);
    AV1_RESET_CDFS(cdfs->delta_q, 4U);
    AV1_RESET_CDFS(cdfs->delta_lf, 4U);
    AV1_RESET_CDFS(cdfs->y_mode, 13U);
    AV1_RESET_CDFS(cdfs->angle_delta, 7U);
    AV1_RESET_CDFS(cdfs->intrabc, 2U);
    AV1_RESET_CDFS(cdfs->tx8, 2U);
    AV1_RESET_CDFS(cdfs->tx16, 3U);
    AV1_RESET_CDFS(cdfs->tx32, 3U);
    AV1_RESET_CDFS(cdfs->tx64, 3U);
    AV1_RESET_CDFS(cdfs->txfm_partition, 2U);
    AV1_RESET_CDFS(cdfs->palette_y_mode, 2U);
    AV1_RESET_CDFS(cdfs->palette_uv_mode, 2U);
    AV1_RESET_CDFS(cdfs->palette_y_size, 7U);
    AV1_RESET_CDFS(cdfs->palette_uv_size, 7U);
    for (palette_size = 0U; palette_size < 7U; ++palette_size) {
        AV1_RESET_CDFS_STRIDE(cdfs->palette_color[0][palette_size],
                              palette_size + 2U, 9U);
        AV1_RESET_CDFS_STRIDE(cdfs->palette_color[1][palette_size],
                              palette_size + 2U, 9U);
    }
    AV1_RESET_CDFS(cdfs->use_wiener, 2U);
    AV1_RESET_CDFS(cdfs->use_sgrproj, 2U);
    AV1_RESET_CDFS(cdfs->restoration_type, 3U);

    AV1_RESET_CDFS(cdfs->inter.is_inter, 2U);
    AV1_RESET_CDFS(cdfs->inter.skip_mode, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_reference, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_reference_type, 2U);
    AV1_RESET_CDFS(cdfs->inter.single_reference, 2U);
    AV1_RESET_CDFS(cdfs->inter.unidirectional_reference, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_forward_reference, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_backward_reference, 2U);
    AV1_RESET_CDFS(cdfs->inter.new_mv, 2U);
    AV1_RESET_CDFS(cdfs->inter.zero_mv, 2U);
    AV1_RESET_CDFS(cdfs->inter.reference_mv, 2U);
    AV1_RESET_CDFS(cdfs->inter.drl_mode, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_mode, 8U);
    AV1_RESET_CDFS(cdfs->inter.switchable_interp, 3U);
    AV1_RESET_CDFS(cdfs->inter.motion_mode, 3U);
    AV1_RESET_CDFS(cdfs->inter.obmc, 2U);
    AV1_RESET_CDFS(cdfs->inter.interintra, 2U);
    AV1_RESET_CDFS(cdfs->inter.interintra_mode, 4U);
    AV1_RESET_CDFS(cdfs->inter.wedge_interintra, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_type, 2U);
    AV1_RESET_CDFS(cdfs->inter.wedge_index, 16U);
    AV1_RESET_CDFS(cdfs->inter.compound_index, 2U);
    AV1_RESET_CDFS(cdfs->inter.compound_group_index, 2U);
    AV1_RESET_CDFS(cdfs->inter.tx_type_set1, 16U);
    AV1_RESET_CDFS(cdfs->inter.tx_type_set2, 12U);
    AV1_RESET_CDFS(cdfs->inter.tx_type_set3, 2U);
    AV1_RESET_CDFS(cdfs->inter.mv_joint, 4U);
    AV1_RESET_CDFS(cdfs->dv_joint, 4U);
    for (component = 0U; component < 2U; ++component) {
        Av1MvComponentCdfs *mv = &cdfs->inter.mv_component[component];
        Av1MvComponentCdfs *dv = &cdfs->dv_component[component];

        AV1_RESET_CDFS(mv->class_cdf, 11U);
        AV1_RESET_CDFS(mv->class0_fr, 4U);
        AV1_RESET_CDFS(mv->fr, 4U);
        AV1_RESET_CDFS(mv->sign, 2U);
        AV1_RESET_CDFS(mv->class0_hp, 2U);
        AV1_RESET_CDFS(mv->hp, 2U);
        AV1_RESET_CDFS(mv->class0_bit, 2U);
        AV1_RESET_CDFS(mv->bits, 2U);
        AV1_RESET_CDFS(dv->class_cdf, 11U);
        AV1_RESET_CDFS(dv->class0_fr, 4U);
        AV1_RESET_CDFS(dv->fr, 4U);
        AV1_RESET_CDFS(dv->sign, 2U);
        AV1_RESET_CDFS(dv->class0_hp, 2U);
        AV1_RESET_CDFS(dv->hp, 2U);
        AV1_RESET_CDFS(dv->class0_bit, 2U);
        AV1_RESET_CDFS(dv->bits, 2U);
    }

    AV1_RESET_CDFS(cdfs->intra.y_mode, 13U);
    AV1_RESET_CDFS(cdfs->intra.uv_mode_cfl_not_allowed, 13U);
    AV1_RESET_CDFS(cdfs->intra.uv_mode_cfl_allowed, 14U);
    AV1_RESET_CDFS(cdfs->intra.angle_delta, 7U);
    AV1_RESET_CDFS(cdfs->intra.filter_intra_mode, 5U);
    AV1_RESET_CDFS(cdfs->intra.filter_intra, 2U);
    AV1_RESET_CDFS(cdfs->intra.tx_type_set1, 7U);
    AV1_RESET_CDFS(cdfs->intra.tx_type_set2, 5U);
    AV1_RESET_CDFS(cdfs->intra.cfl_sign, 8U);
    AV1_RESET_CDFS(cdfs->intra.cfl_alpha, 16U);

    AV1_RESET_CDFS(cdfs->coeff.txb_skip, 2U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_16, 5U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_32, 6U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_64, 7U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_128, 8U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_256, 9U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_512, 10U);
    AV1_RESET_CDFS(cdfs->coeff.eob_pt_1024, 11U);
    AV1_RESET_CDFS(cdfs->coeff.eob_extra, 2U);
    AV1_RESET_CDFS(cdfs->coeff.dc_sign, 2U);
    AV1_RESET_CDFS(cdfs->coeff.coeff_base_eob, 3U);
    AV1_RESET_CDFS(cdfs->coeff.coeff_base, 4U);
    AV1_RESET_CDFS(cdfs->coeff.coeff_br, AV1_BR_CDF_SIZE);
}

#undef AV1_RESET_CDFS_STRIDE
#undef AV1_RESET_CDFS

uint64_t av1_tile_cdfs_checksum(const Av1TileCdfs *cdfs) {
    uint64_t checksum = (uint64_t)1469598103934665603ULL;

    if (cdfs == 0) return 0U;
    av1_tile_checksum_bytes(&checksum, &cdfs->partition, sizeof(cdfs->partition));
    av1_tile_checksum_bytes(&checksum, cdfs->skip, sizeof(cdfs->skip));
    av1_tile_checksum_bytes(&checksum, cdfs->segment_id, sizeof(cdfs->segment_id));
    av1_tile_checksum_bytes(&checksum, cdfs->delta_q, sizeof(cdfs->delta_q));
    av1_tile_checksum_bytes(&checksum, cdfs->delta_lf, sizeof(cdfs->delta_lf));
    av1_tile_checksum_bytes(&checksum, cdfs->y_mode, sizeof(cdfs->y_mode));
    av1_tile_checksum_bytes(&checksum, cdfs->angle_delta, sizeof(cdfs->angle_delta));
    av1_tile_checksum_bytes(&checksum, cdfs->intrabc, sizeof(cdfs->intrabc));
    av1_tile_checksum_bytes(&checksum, cdfs->dv_joint,
                            sizeof(cdfs->dv_joint));
    av1_tile_checksum_bytes(&checksum, cdfs->dv_component,
                            sizeof(cdfs->dv_component));
    av1_tile_checksum_bytes(&checksum, cdfs->tx8, sizeof(cdfs->tx8));
    av1_tile_checksum_bytes(&checksum, cdfs->tx16, sizeof(cdfs->tx16));
    av1_tile_checksum_bytes(&checksum, cdfs->tx32, sizeof(cdfs->tx32));
    av1_tile_checksum_bytes(&checksum, cdfs->tx64, sizeof(cdfs->tx64));
    av1_tile_checksum_bytes(&checksum, cdfs->palette_y_mode,
                            sizeof(cdfs->palette_y_mode));
    av1_tile_checksum_bytes(&checksum, cdfs->palette_uv_mode,
                            sizeof(cdfs->palette_uv_mode));
    av1_tile_checksum_bytes(&checksum, cdfs->palette_y_size,
                            sizeof(cdfs->palette_y_size));
    av1_tile_checksum_bytes(&checksum, cdfs->palette_uv_size,
                            sizeof(cdfs->palette_uv_size));
    av1_tile_checksum_bytes(&checksum, cdfs->palette_color,
                            sizeof(cdfs->palette_color));
    av1_tile_checksum_bytes(&checksum, &cdfs->intra, sizeof(cdfs->intra));
    av1_tile_checksum_bytes(&checksum, &cdfs->coeff, sizeof(cdfs->coeff));
    return checksum;
}
