#ifndef AVIFDEC_AV1_INTRA_H
#define AVIFDEC_AV1_INTRA_H

#include "avifdec.h"

enum {
    AV1_INTRA_MODES = 13,
    AV1_INTRA_MODE_CONTEXTS = 5,
    AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED = 13,
    AV1_UV_INTRA_MODES_CFL_ALLOWED = 14,
    AV1_DIRECTIONAL_MODES = 8,
    AV1_MAX_ANGLE_DELTA = 3,
    AV1_BLOCK_SIZES = 22,
    AV1_CFL_JOINT_SIGNS = 8,
    AV1_CFL_ALPHA_CONTEXTS = 6,
    AV1_CFL_ALPHABET_SIZE = 16
};

typedef struct {
    uint16_t y_mode[AV1_INTRA_MODE_CONTEXTS][AV1_INTRA_MODE_CONTEXTS]
                   [AV1_INTRA_MODES + 1];
    uint16_t uv_mode_cfl_not_allowed[AV1_INTRA_MODES]
                                    [AV1_UV_INTRA_MODES_CFL_NOT_ALLOWED + 1];
    uint16_t uv_mode_cfl_allowed[AV1_INTRA_MODES]
                                [AV1_UV_INTRA_MODES_CFL_ALLOWED + 1];
    uint16_t angle_delta[AV1_DIRECTIONAL_MODES]
                        [2 * AV1_MAX_ANGLE_DELTA + 2];
    uint16_t filter_intra_mode[6];
    uint16_t filter_intra[AV1_BLOCK_SIZES][3];
    uint16_t tx_type_set1[2][AV1_INTRA_MODES][8];
    uint16_t tx_type_set2[3][AV1_INTRA_MODES][6];
    uint16_t cfl_sign[AV1_CFL_JOINT_SIGNS + 1];
    uint16_t cfl_alpha[AV1_CFL_ALPHA_CONTEXTS][AV1_CFL_ALPHABET_SIZE + 1];
} Av1IntraCdfs;

void av1_intra_cdfs_init(Av1IntraCdfs *cdfs);
uint64_t av1_intra_cdfs_checksum(const Av1IntraCdfs *cdfs);

#endif