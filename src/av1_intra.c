#include "av1_intra.h"
#include "base.h"

#define INTRA_MODES AV1_INTRA_MODES
#define INTRA_MODE_CONTEXTS AV1_INTRA_MODE_CONTEXTS
#define UV_INTRA_MODES_CFL_NOT_ALLOWED AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED
#define UV_INTRA_MODES_CFL_ALLOWED AV1_UV_INTRA_MODES_CFL_ALLOWED
#define DIRECTIONAL_MODES AV1_DIRECTIONAL_MODES
#define MAX_ANGLE_DELTA AV1_MAX_ANGLE_DELTA
#define BLOCK_SIZES AV1_BLOCK_SIZES
#define CFL_JOINT_SIGNS AV1_CFL_JOINT_SIGNS
#define CFL_ALPHA_CONTEXTS AV1_CFL_ALPHA_CONTEXTS
#define CFL_ALPHABET_SIZE AV1_CFL_ALPHABET_SIZE
#include "av1_intra_defaults.inc"
#undef INTRA_MODES
#undef INTRA_MODE_CONTEXTS
#undef UV_INTRA_MODES_CFL_NOT_ALLOWED
#undef UV_INTRA_MODES_CFL_ALLOWED
#undef DIRECTIONAL_MODES
#undef MAX_ANGLE_DELTA
#undef BLOCK_SIZES
#undef CFL_JOINT_SIGNS
#undef CFL_ALPHA_CONTEXTS
#undef CFL_ALPHABET_SIZE

void av1_intra_cdfs_init(Av1IntraCdfs *cdfs) {
    if (cdfs == 0) return;
    avifdec_memory_copy(cdfs->y_mode, av1_default_intra_frame_y_mode_cdf,
                        sizeof(cdfs->y_mode));
    avifdec_memory_copy(cdfs->uv_mode_cfl_not_allowed,
                        av1_default_uv_mode_cfl_not_allowed_cdf,
                        sizeof(cdfs->uv_mode_cfl_not_allowed));
    avifdec_memory_copy(cdfs->uv_mode_cfl_allowed,
                        av1_default_uv_mode_cfl_allowed_cdf,
                        sizeof(cdfs->uv_mode_cfl_allowed));
    avifdec_memory_copy(cdfs->angle_delta, av1_default_intra_angle_delta_cdf,
                        sizeof(cdfs->angle_delta));
    avifdec_memory_copy(cdfs->filter_intra_mode,
                        av1_default_filter_intra_mode_cdf,
                        sizeof(cdfs->filter_intra_mode));
    avifdec_memory_copy(cdfs->filter_intra, av1_default_filter_intra_cdf,
                        sizeof(cdfs->filter_intra));
    avifdec_memory_copy(cdfs->tx_type_set1,
                        av1_default_intra_tx_type_set1_cdf,
                        sizeof(cdfs->tx_type_set1));
    avifdec_memory_copy(cdfs->tx_type_set2,
                        av1_default_intra_tx_type_set2_cdf,
                        sizeof(cdfs->tx_type_set2));
    avifdec_memory_copy(cdfs->cfl_sign, av1_default_cfl_sign_cdf,
                        sizeof(cdfs->cfl_sign));
    avifdec_memory_copy(cdfs->cfl_alpha, av1_default_cfl_alpha_cdf,
                        sizeof(cdfs->cfl_alpha));
}

uint64_t av1_intra_cdfs_checksum(const Av1IntraCdfs *cdfs) {
    const uint8_t *bytes = (const uint8_t *)cdfs;
    uint64_t checksum = (uint64_t)1469598103934665603ULL;
    size_t index;

    if (cdfs == 0) return 0U;
    for (index = 0U; index < sizeof(*cdfs); ++index) {
        checksum ^= bytes[index];
        checksum *= (uint64_t)1099511628211ULL;
    }
    return checksum;
}