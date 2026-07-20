#include "encoder/av1_tile_partition.h"

AvifencStatus avifenc_av1_tile_query(
    const AvifencAv1TileSource *source,
    AvifencAv1TileRequirements *requirements) {
    return avifenc_av1_tile_requirements(source, requirements);
}

AvifencStatus avifenc_av1_tile_write(
    AvifencAv1SymbolWriter *writer,
    const AvifencAv1TileSource *source,
    AvifencAv1TileReconstruction *reconstruction,
    void *workspace,
    size_t workspace_size) {
    AvifencAv1TileRequirements requirements;
    AvifencAv1TileState state;
    size_t cells;
    uint32_t row;
    uint32_t column;
    unsigned int plane;
    AvifencStatus status = avifenc_av1_tile_requirements(source, &requirements);

    if (status != AVIFENC_OK) return status;
    if (writer == 0) return AVIFENC_INVALID_ARGUMENT;
    if (writer->status != AVIFENC_OK) return writer->status;
    if (writer->disable_cdf_update == 0U || reconstruction == 0 ||
        workspace == 0 || workspace_size < requirements.workspace_required) {
        return workspace_size < requirements.workspace_required
            ? AVIFENC_OUT_OF_MEMORY : AVIFENC_INVALID_ARGUMENT;
    }
            if (source->statistics != 0) ++source->statistics->tile_count;
    for (plane = 0U; plane < 3U; ++plane) {
        if (reconstruction->planes[plane] == 0 ||
            reconstruction->strides[plane] <
                requirements.reconstruction_widths[plane] ||
            reconstruction->widths[plane] <
                requirements.reconstruction_widths[plane] ||
            reconstruction->heights[plane] <
                requirements.reconstruction_heights[plane]) {
            return AVIFENC_INVALID_ARGUMENT;
        }
    }
    state.writer = writer;
    state.source = source;
    state.reconstruction = reconstruction;
    state.quantizer = source->quantizer;
    state.mi_columns = 2U * ((source->width + 7U) >> 3U);
    state.mi_rows = 2U * ((source->height + 7U) >> 3U);
    cells = (size_t)state.mi_columns * state.mi_rows;
    state.block_widths = (uint8_t *)workspace;
    state.block_heights = state.block_widths + cells;
    state.block_flags = state.block_heights + cells;
    state.segment_ids = state.block_flags + cells;
    state.y_modes = state.segment_ids + cells;
    state.uv_modes = state.y_modes + cells;
    state.palette_sizes_y = state.uv_modes + cells;
    state.palette_sizes_uv = state.palette_sizes_y + cells;
    state.palette_map_y = state.palette_sizes_uv + cells;
    state.palette_map_uv = state.palette_map_y + 32U * 32U;
    state.trial_reconstruction = (uint8_t *)workspace +
        requirements.workspace_required - 32U * 32U * sizeof(uint16_t);
    avifdec_memory_fill(workspace, 0U, requirements.workspace_required);
    avifenc_av1_tile_cdfs_init(&state);
    status = avifenc_av1_transform_state_init(
        &state.transform, (uint8_t)source->quantizer,
        state.mi_columns, state.mi_rows,
        state.palette_map_uv + 16U * 16U,
        workspace_size - 8U * cells - 32U * 32U * sizeof(uint16_t) -
            32U * 32U - 16U * 16U);
    if (status != AVIFENC_OK) return status;
    status = avifenc_av1_transform_state_set_quantization(
        &state.transform, &source->quantization,
        (uint8_t)source->quantizer,
        state.transform.matrix_workspace,
        3U * AV1_QM_TOTAL_SIZE);
    if (status != AVIFENC_OK) return status;
    for (row = 0U; row < state.mi_rows; row += 16U) {
        for (column = 0U; column < state.mi_columns; column += 16U) {
            status = avifenc_av1_tile_write_partition(&state, row, column, 16U);
            if (status != AVIFENC_OK) return status;
        }
    }
    return avifenc_av1_symbol_writer_finish(writer);
}
