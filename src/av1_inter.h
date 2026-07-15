#ifndef AVIFDEC_AV1_INTER_H
#define AVIFDEC_AV1_INTER_H

#include "avifdec.h"
#include "av1_symbol.h"

#define AV1_MAX_MV_STACK_SIZE 8U

typedef struct {
    int32_t row;
    int32_t column;
} Av1MotionVector;

typedef struct {
    Av1MotionVector mv[2];
    uint16_t weight;
} Av1MvCandidate;

typedef struct {
    int16_t row;
    int16_t column;
} Av1TemporalMotionVector;

typedef struct {
    Av1TemporalMotionVector mv;
    uint8_t ref_frame;
    uint8_t valid;
} Av1SavedMotion;

typedef struct {
    Av1TemporalMotionVector mv;
    int16_t ref_frame_offset;
    uint8_t valid;
} Av1TemporalMotion;

typedef struct {
    Av1MvCandidate candidates[AV1_MAX_MV_STACK_SIZE];
    uint8_t count;
} Av1MvStack;

typedef struct {
    uint16_t class_cdf[12];
    uint16_t class0_fr[2][5];
    uint16_t fr[5];
    uint16_t sign[3];
    uint16_t class0_hp[3];
    uint16_t hp[3];
    uint16_t class0_bit[3];
    uint16_t bits[10][3];
} Av1MvComponentCdfs;

typedef struct {
    uint16_t is_inter[4][3];
    uint16_t skip_mode[3][3];
    uint16_t compound_reference[5][3];
    uint16_t compound_reference_type[5][3];
    uint16_t single_reference[3][6][3];
    uint16_t unidirectional_reference[3][3][3];
    uint16_t compound_forward_reference[3][3][3];
    uint16_t compound_backward_reference[3][2][3];
    uint16_t new_mv[6][3];
    uint16_t zero_mv[2][3];
    uint16_t reference_mv[6][3];
    uint16_t drl_mode[3][3];
    uint16_t compound_mode[8][9];
    uint16_t switchable_interp[16][4];
    uint16_t motion_mode[22][4];
    uint16_t obmc[22][3];
    uint16_t interintra[4][3];
    uint16_t interintra_mode[4][5];
    uint16_t wedge_interintra[22][3];
    uint16_t compound_type[22][3];
    uint16_t wedge_index[22][17];
    uint16_t compound_index[6][3];
    uint16_t compound_group_index[6][3];
    uint16_t tx_type_set1[2][17];
    uint16_t tx_type_set2[13];
    uint16_t tx_type_set3[4][3];
    uint16_t mv_joint[5];
    Av1MvComponentCdfs mv_component[2];
} Av1InterCdfs;

void av1_inter_cdfs_init(Av1InterCdfs *cdfs);
AvifdecStatus av1_mv_read(Av1SymbolDecoder *decoder,
                          Av1InterCdfs *cdfs,
                          int force_integer_mv,
                          int allow_high_precision_mv,
                          Av1MotionVector prediction,
                          Av1MotionVector *mv);
AvifdecStatus av1_mv_read_cdfs(Av1SymbolDecoder *decoder,
                               uint16_t joint[5],
                               Av1MvComponentCdfs components[2],
                               int force_integer_mv,
                               int allow_high_precision_mv,
                               Av1MotionVector prediction,
                               Av1MotionVector *mv);

AvifdecStatus av1_mv_project(Av1MotionVector mv,
                             int32_t numerator,
                             uint32_t denominator,
                             Av1MotionVector *projected);
AvifdecStatus av1_mv_global(uint8_t type,
                            const int32_t params[6],
                            uint32_t mi_row,
                            uint32_t mi_column,
                            uint32_t block_width,
                            uint32_t block_height,
                            int force_integer_mv,
                            int allow_high_precision_mv,
                            Av1MotionVector *mv);
void av1_mv_lower_precision(Av1MotionVector *mv,
                            int force_integer_mv,
                            int allow_high_precision_mv);
void av1_mv_clamp(Av1MotionVector *mv,
                  uint32_t mi_row,
                  uint32_t mi_column,
                  uint32_t block_width4,
                  uint32_t block_height4,
                  uint32_t mi_rows,
                  uint32_t mi_columns,
                  uint32_t border);
AvifdecStatus av1_mv_stack_add(Av1MvStack *stack,
                               Av1MotionVector first,
                               Av1MotionVector second,
                               int compound,
                               uint16_t weight);
void av1_mv_stack_sort(Av1MvStack *stack, uint8_t start);

#endif