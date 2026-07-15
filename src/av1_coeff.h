#ifndef AVIFDEC_AV1_COEFF_H
#define AVIFDEC_AV1_COEFF_H

#include "av1_symbol.h"

enum {
    AV1_COEFF_Q_CONTEXTS = 4,
    AV1_TX_SIZE_CONTEXTS = 5,
    AV1_PLANE_TYPES = 2,
    AV1_TXB_SKIP_CONTEXTS = 13,
    AV1_EOB_COEF_CONTEXTS = 9,
    AV1_DC_SIGN_CONTEXTS = 3,
    AV1_SIG_COEF_CONTEXTS_EOB = 4,
    AV1_SIG_COEF_CONTEXTS = 42,
    AV1_LEVEL_CONTEXTS = 21,
    AV1_BR_CDF_SIZE = 4
};

typedef enum {
    AV1_TX_4X4 = 0,
    AV1_TX_8X8,
    AV1_TX_16X16,
    AV1_TX_32X32,
    AV1_TX_64X64,
    AV1_TX_4X8,
    AV1_TX_8X4,
    AV1_TX_8X16,
    AV1_TX_16X8,
    AV1_TX_16X32,
    AV1_TX_32X16,
    AV1_TX_32X64,
    AV1_TX_64X32,
    AV1_TX_4X16,
    AV1_TX_16X4,
    AV1_TX_8X32,
    AV1_TX_32X8,
    AV1_TX_16X64,
    AV1_TX_64X16,
    AV1_TX_SIZES_ALL
} Av1TxSize;

typedef enum {
    AV1_TX_DCT_DCT = 0,
    AV1_TX_ADST_DCT,
    AV1_TX_DCT_ADST,
    AV1_TX_ADST_ADST,
    AV1_TX_FLIPADST_DCT,
    AV1_TX_DCT_FLIPADST,
    AV1_TX_FLIPADST_FLIPADST,
    AV1_TX_ADST_FLIPADST,
    AV1_TX_FLIPADST_ADST,
    AV1_TX_IDTX,
    AV1_TX_V_DCT,
    AV1_TX_H_DCT,
    AV1_TX_V_ADST,
    AV1_TX_H_ADST,
    AV1_TX_V_FLIPADST,
    AV1_TX_H_FLIPADST,
    AV1_TX_TYPES
} Av1TxType;

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t width_log2;
    uint8_t height_log2;
} Av1TxSizeInfo;

typedef struct {
    uint8_t *above_level_context;
    uint8_t *left_level_context;
    uint8_t *above_dc_context;
    uint8_t *left_dc_context;
    size_t above_capacity;
    size_t left_capacity;
    size_t width4;
    size_t height4;
} Av1CoeffPlaneContext;

typedef struct {
    Av1CoeffPlaneContext plane[3];
} Av1CoeffContextState;

typedef struct {
    uint16_t eob;
    uint8_t cul_level;
    uint8_t dc_category;
} Av1CoeffBlockResult;

typedef struct {
    uint16_t txb_skip[AV1_TX_SIZE_CONTEXTS][AV1_TXB_SKIP_CONTEXTS][3];
    uint16_t eob_pt_16[AV1_PLANE_TYPES][2][6];
    uint16_t eob_pt_32[AV1_PLANE_TYPES][2][7];
    uint16_t eob_pt_64[AV1_PLANE_TYPES][2][8];
    uint16_t eob_pt_128[AV1_PLANE_TYPES][2][9];
    uint16_t eob_pt_256[AV1_PLANE_TYPES][2][10];
    uint16_t eob_pt_512[AV1_PLANE_TYPES][11];
    uint16_t eob_pt_1024[AV1_PLANE_TYPES][12];
    uint16_t eob_extra[AV1_TX_SIZE_CONTEXTS][AV1_PLANE_TYPES]
                      [AV1_EOB_COEF_CONTEXTS][3];
    uint16_t dc_sign[AV1_PLANE_TYPES][AV1_DC_SIGN_CONTEXTS][3];
    uint16_t coeff_base_eob[AV1_TX_SIZE_CONTEXTS][AV1_PLANE_TYPES]
                           [AV1_SIG_COEF_CONTEXTS_EOB][4];
    uint16_t coeff_base[AV1_TX_SIZE_CONTEXTS][AV1_PLANE_TYPES]
                       [AV1_SIG_COEF_CONTEXTS][5];
    uint16_t coeff_br[AV1_TX_SIZE_CONTEXTS][AV1_PLANE_TYPES]
                     [AV1_LEVEL_CONTEXTS][AV1_BR_CDF_SIZE + 1];
} Av1CoeffCdfs;

typedef AvifdecStatus (*Av1CoeffSelectTxType)(void *user_data,
                                               Av1SymbolDecoder *decoder,
                                               Av1TxSize tx_size,
                                               Av1TxType *tx_type);

extern const Av1TxSizeInfo av1_tx_size_info[AV1_TX_SIZES_ALL];

unsigned int av1_coeff_q_context(uint8_t base_q_index);
void av1_coeff_cdfs_init(Av1CoeffCdfs *cdfs, uint8_t base_q_index);
uint64_t av1_coeff_cdfs_checksum(const Av1CoeffCdfs *cdfs);
AvifdecStatus av1_coeff_context_init(Av1CoeffContextState *state,
                                     const Av1CoeffPlaneContext plane[3]);
AvifdecStatus av1_coeff_parse_block(Av1SymbolDecoder *decoder,
                                    Av1CoeffCdfs *cdfs,
                                    Av1CoeffContextState *contexts,
                                    unsigned int plane,
                                    Av1TxSize tx_size,
                                    Av1TxType tx_type,
                                    size_t plane_block_width,
                                    size_t plane_block_height,
                                    size_t x4,
                                    size_t y4,
                                    Av1CoeffBlockResult *result);
                AvifdecStatus av1_coeff_parse_block_values(Av1SymbolDecoder *decoder,
                                         Av1CoeffCdfs *cdfs,
                                         Av1CoeffContextState *contexts,
                                         unsigned int plane,
                                         Av1TxSize tx_size,
                                         Av1TxType tx_type,
                                         size_t plane_block_width,
                                         size_t plane_block_height,
                                         size_t x4,
                                         size_t y4,
                                         int32_t *coefficients,
                                         size_t coefficient_capacity,
                                         Av1CoeffBlockResult *result);
AvifdecStatus av1_coeff_parse_block_select(
    Av1SymbolDecoder *decoder,
    Av1CoeffCdfs *cdfs,
    Av1CoeffContextState *contexts,
    unsigned int plane,
    Av1TxSize tx_size,
    Av1TxType tx_type,
    size_t plane_block_width,
    size_t plane_block_height,
    size_t x4,
    size_t y4,
    Av1CoeffSelectTxType select_tx_type,
    void *select_user_data,
    int32_t *coefficients,
    size_t coefficient_capacity,
    Av1CoeffBlockResult *result);

#endif