#ifndef AVIFDEC_AV1_TILE_INTERNAL_H
#define AVIFDEC_AV1_TILE_INTERNAL_H

#include "av1_tile.h"

const Av1BlockCell *av1_block_cell(const Av1BlockState *state,
                                   uint32_t row,
                                   uint32_t column);
Av1BlockCell *av1_block_cell_mutable(Av1BlockState *state,
                                     uint32_t row,
                                     uint32_t column);

enum {
    /* AV1 bitstream conformance bounds every tile to MAX_TILE_WIDTH (4096
       luma samples), i.e. 1024 4x4 "mi" units, regardless of superblock
       size; av1_parse_tile_info() (av1.c) enforces this when splitting
       tiles, so it is a safe fixed capacity for the "above" rolling
       context of one tile. */
    AV1_TILE_PALETTE_ABOVE_MI = 1024,
    /* "left" only ever needs to remember the current superblock row
       band (blocks never span more than one superblock), so it can be
       capped to the largest supported superblock size. */
    AV1_TILE_PALETTE_LEFT_MI = 32
};

typedef struct {
    uint16_t y[8];
    uint16_t u[8];
} Av1TilePaletteColors;

/* Tile-local rolling replacement for the palette_colors_* fields that used
   to live in every Av1BlockCell. Only the immediately adjacent above/left
   neighbor is ever consulted (av1_tile_palette_cache()), so a dense O(mi
   cells) grid of colors is unnecessary; a column-indexed "above" row and a
   row-indexed "left" column, sized like the existing above/left
   coefficient contexts, are sufficient and are reset per tile/row-band
   instead of persisting for the whole frame. */
typedef struct {
    Av1TilePaletteColors above[AV1_TILE_PALETTE_ABOVE_MI];
    Av1TilePaletteColors left[AV1_TILE_PALETTE_LEFT_MI];
    uint32_t tile_column_start;
    uint32_t row_band_start;
} Av1TilePaletteContext;

AvifdecStatus av1_tile_palette_context_init(Av1TilePaletteContext *context,
                                            uint32_t tile_row_start,
                                            uint32_t tile_column_start,
                                            uint32_t tile_column_end);
AvifdecStatus av1_tile_palette_context_new_row_band(
    Av1TilePaletteContext *context,
    uint32_t row_band_start);

AvifdecStatus av1_tile_read_palette_info(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockAvailability *availability,
    Av1BlockTraceFields *fields,
    uint8_t block_size,
    Av1TilePaletteContext *color_context);

AvifdecStatus av1_tile_read_palette_tokens(
    Av1SymbolDecoder *decoder,
    Av1TileCdfs *cdfs,
    const Av1TileModeConfig *config,
    const Av1BlockTraceFields *fields);

#endif
