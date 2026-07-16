#ifndef AVIFDEC_H
#define AVIFDEC_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AVIFDEC_OK = 0,
    AVIFDEC_INVALID_ARGUMENT,
    AVIFDEC_TRUNCATED,
    AVIFDEC_INVALID_DATA,
    AVIFDEC_OVERFLOW,
    AVIFDEC_LIMIT_EXCEEDED,
    AVIFDEC_OUT_OF_MEMORY,
    AVIFDEC_IO_ERROR,
    AVIFDEC_UNSUPPORTED
} AvifdecStatus;

#define AVIFDEC_CAP_AV1_LOW_OVERHEAD ((uint64_t)1U << 0)
#define AVIFDEC_CAP_AV1_ANNEX_B ((uint64_t)1U << 1)
#define AVIFDEC_CAP_AV1_OPERATING_POINTS ((uint64_t)1U << 2)
#define AVIFDEC_CAP_AV1_METADATA ((uint64_t)1U << 3)
#define AVIFDEC_CAP_AV1_PROFILE_LEVELS ((uint64_t)1U << 4)
#define AVIFDEC_CAP_AV1_FILM_GRAIN ((uint64_t)1U << 5)
#define AVIFDEC_CAP_AV1_TILE_LIST ((uint64_t)1U << 6)
#define AVIFDEC_CAP_AV1_LARGE_SCALE_TILE ((uint64_t)1U << 7)
#define AVIFDEC_CAP_AVIF_PRESENTATION ((uint64_t)1U << 8)
#define AVIFDEC_CAP_AVIF_ALPHA ((uint64_t)1U << 9)
#define AVIFDEC_CAP_AVIF_GRID ((uint64_t)1U << 10)
#define AVIFDEC_CAP_AVIF_LAYERED ((uint64_t)1U << 11)
#define AVIFDEC_CAP_AVIF_SAMPLE_TRANSFORM ((uint64_t)1U << 12)
#define AVIFDEC_CAP_RGB_CONVERSION ((uint64_t)1U << 13)
#define AVIFDEC_CAP_AVIF_TONE_MAP_METADATA ((uint64_t)1U << 14)
#define AVIFDEC_CAP_AVIF_SEQUENCE ((uint64_t)1U << 15)
#define AVIFDEC_CAP_PARALLEL_EXECUTOR ((uint64_t)1U << 16)

#define AVIFDEC_VERSION_MAJOR 1U
#define AVIFDEC_VERSION_MINOR 3U
#define AVIFDEC_VERSION_PATCH 0U

typedef struct {
    AvifdecStatus status;
    size_t offset;
    uint32_t context;
} AvifdecError;

#define AVIFDEC_DEFAULT_MAX_ITEMS 256U
#define AVIFDEC_DEFAULT_MAX_EXTENTS 64U
#define AVIFDEC_DEFAULT_MAX_PROPERTIES 128U
#define AVIFDEC_DEFAULT_MAX_OBUS 4096U
#define AVIFDEC_DEFAULT_MAX_FRAMES 256U

typedef enum {
    AVIFDEC_AV1_LOW_OVERHEAD = 0,
    AVIFDEC_AV1_ANNEX_B = 1
} AvifdecAv1Framing;

typedef struct {
    uint32_t max_width;
    uint32_t max_height;
    size_t max_pixels;
    size_t max_items;
    size_t max_extents;
    size_t max_properties;
    size_t max_obus;
    size_t max_frames;
    uint8_t operating_point;
    uint8_t av1_framing;
    uint8_t spatial_layer;
    uint8_t spatial_layer_set;
} AvifdecLimits;

#define AVIFDEC_EXECUTOR_MAX_WORKERS 32U

/*
 * Optional structured parallel execution.
 *
 * parallel_for must invoke body exactly once for every index in [0, count),
 * wait for all invocations before returning, and pass a worker_index smaller
 * than worker_count. Calls sharing a worker_index must not overlap. The
 * decoder never retains the executor or callback arguments after return.
 */
typedef AvifdecStatus (*AvifdecParallelBody)(
    size_t begin,
    size_t end,
    size_t worker_index,
    void *arg);

typedef AvifdecStatus (*AvifdecParallelFor)(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg);

typedef struct {
    void *user_data;
    size_t worker_count;
    AvifdecParallelFor parallel_for;
} AvifdecExecutor;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t file_offset;
} AvifdecSpan;

typedef struct {
    uint16_t max_cll;
    uint16_t max_fall;
} AvifdecHdrCll;

typedef struct {
    uint16_t primary_x[3];
    uint16_t primary_y[3];
    uint16_t white_point_x;
    uint16_t white_point_y;
    uint32_t luminance_max;
    uint32_t luminance_min;
} AvifdecHdrMdcv;

#define AVIFDEC_TRANSFORM_CLAP ((uint8_t)1U << 0)
#define AVIFDEC_TRANSFORM_IROT ((uint8_t)1U << 1)
#define AVIFDEC_TRANSFORM_IMIR ((uint8_t)1U << 2)
#define AVIFDEC_TRANSFORM_PASP ((uint8_t)1U << 3)
#define AVIFDEC_AUXILIARY_NONE 0U
#define AVIFDEC_AUXILIARY_ALPHA 1U
#define AVIFDEC_AUXILIARY_DEPTH 2U

typedef struct {
    uint32_t width_n;
    uint32_t width_d;
    uint32_t height_n;
    uint32_t height_d;
    int32_t horizontal_offset_n;
    uint32_t horizontal_offset_d;
    int32_t vertical_offset_n;
    uint32_t vertical_offset_d;
} AvifdecCleanAperture;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} AvifdecCropRect;

typedef struct {
    uint8_t counting_type;
    uint8_t full_timestamp;
    uint8_t discontinuity;
    uint8_t count_dropped;
    uint16_t n_frames;
    uint8_t seconds_present;
    uint8_t minutes_present;
    uint8_t hours_present;
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t time_offset_length;
    uint32_t time_offset_value;
} AvifdecTimecode;

typedef struct {
    uint32_t primary_item_id;
    uint32_t primary_item_type;
    uint32_t width;
    uint32_t height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    uint32_t render_width;
    uint32_t render_height;
    size_t payload_size;
    size_t extent_count;
    size_t workspace_required;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t profile;
    uint8_t level;
    uint8_t tier;
    uint8_t operating_point;
    uint8_t operating_point_count;
    uint16_t operating_point_idc;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    uint8_t color_range;
    uint8_t channel_count;
    uint8_t has_nclx;
    uint8_t auxiliary_type;
    uint8_t transform_flags;
    uint8_t irot_angle;
    uint8_t imir_axis;
    uint32_t pixel_aspect_h_spacing;
    uint32_t pixel_aspect_v_spacing;
    AvifdecCleanAperture clean_aperture;
    AvifdecCropRect crop;
    uint8_t item_hdr_cll_present;
    uint8_t item_hdr_mdcv_present;
    AvifdecHdrCll item_hdr_cll;
    AvifdecHdrMdcv item_hdr_mdcv;
    uint8_t has_alpha;
    uint8_t alpha_premultiplied;
    uint8_t alpha_bit_depth;
    uint8_t alpha_color_range;
    uint32_t alpha_item_id;
    uint8_t is_grid;
    uint16_t grid_rows;
    uint16_t grid_columns;
    uint32_t grid_tile_width;
    uint32_t grid_tile_height;
    uint8_t is_layered;
    uint8_t layer_count;
    uint8_t selected_layer;
    uint8_t has_lsel;
    uint16_t lsel_layer_id;
    uint8_t has_a1op;
    uint8_t a1op_index;
    size_t layer_sizes[4];
    uint8_t gain_map_present;
    uint32_t tone_map_base_item_id;
    uint32_t tone_map_gain_item_id;
    size_t tone_map_metadata_size;
    uint64_t tone_map_metadata_checksum;
    uint8_t sample_transform_present;
    uint8_t sample_transform_intermediate_bits;
    uint8_t sample_transform_token_count;
    uint8_t sample_transform_input_count;
    uint8_t timing_info_present;
    uint8_t equal_picture_interval;
    uint8_t decoder_model_info_present;
    uint8_t buffer_delay_length;
    uint8_t buffer_removal_time_length;
    uint8_t frame_presentation_time_length;
    uint8_t operating_point_decoder_model_present;
    uint8_t low_delay_mode;
    uint8_t initial_display_delay_present;
    uint8_t initial_display_delay_minus_1;
    uint8_t buffer_removal_time_present;
    uint8_t frame_presentation_time_present;
    uint32_t decoder_buffer_delay;
    uint32_t encoder_buffer_delay;
    uint32_t buffer_removal_time;
    uint32_t frame_presentation_time;
    uint32_t num_units_in_display_tick;
    uint32_t time_scale;
    uint32_t num_ticks_per_picture_minus_1;
    uint32_t num_units_in_decoding_tick;
    uint8_t reduced_still_picture_header;
    uint8_t workspace_plane_buffer_count;
    uint8_t film_grain_params_present;
    uint8_t film_grain_applied;
    uint8_t film_grain_update;
    uint8_t film_grain_overlap;
    uint8_t film_grain_clip_restricted;
    uint16_t film_grain_seed;
    uint8_t frame_type;
    uint8_t base_q_index;
    uint8_t coded_lossless;
    uint8_t allow_screen_content_tools;
    uint8_t allow_intrabc;
    uint8_t enable_filter_intra;
    uint8_t enable_intra_edge_filter;
    uint8_t segmentation_enabled;
    uint8_t delta_q_present;
    uint8_t delta_lf_present;
    uint8_t tx_mode;
    uint8_t reduced_tx_set;
    uint8_t superblock_size;
    uint16_t tile_columns;
    uint16_t tile_rows;
    size_t tile_count;
    size_t tile_data_size;
    size_t obu_count;
    size_t metadata_obu_count;
    uint8_t metadata_present_mask;
    AvifdecHdrCll hdr_cll;
    AvifdecHdrMdcv hdr_mdcv;
    uint8_t scalability_mode_idc;
    uint8_t scalability_flags;
    uint8_t spatial_layer_count;
    uint8_t temporal_group_size;
    uint16_t spatial_layer_width[4];
    uint16_t spatial_layer_height[4];
    uint8_t spatial_layer_ref_id[4];
    uint64_t scalability_checksum;
    uint8_t itu_t35_country_code;
    uint8_t itu_t35_country_code_extension;
    size_t itu_t35_payload_size;
    uint64_t itu_t35_payload_checksum;
    AvifdecTimecode timecode;
    const unsigned char *icc_data;
    size_t icc_size;
} AvifdecImageInfo;

typedef struct {
    size_t frame_count;
    size_t show_existing_frame_count;
    size_t tile_count;
    size_t partition_nodes;
    size_t block_count;
    size_t inter_block_count;
    size_t compound_block_count;
    size_t transform_count;
    size_t nonzero_transform_count;
    size_t coefficient_count;
    uint32_t transform_size_mask;
    uint32_t transform_type_mask;
    uint64_t checksum;
    uint64_t reference_state_checksum;
    uint64_t mode_checksum;
    uint64_t inter_mode_checksum;
    uint64_t mv_stack_checksum;
    uint64_t mv_checksum;
    uint64_t predictor_checksum;
    uint64_t quantized_checksum;
    uint64_t dequantized_checksum;
    uint64_t residual_checksum;
    uint64_t reconstruction_checksum;
    uint64_t deblocked_checksum;
    uint64_t cdef_checksum;
    uint64_t superres_checksum;
    uint64_t restoration_checksum;
} AvifdecEntropyTrace;

typedef struct {
    uint16_t *planes[3];
    size_t strides[3];
    uint32_t widths[3];
    uint32_t heights[3];
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint16_t *alpha_plane;
    size_t alpha_stride;
    uint32_t alpha_width;
    uint32_t alpha_height;
    uint8_t alpha_bit_depth;
    uint8_t alpha_color_range;
    uint8_t alpha_premultiplied;
} AvifdecImage;

typedef enum {
    AVIFDEC_RGB8 = 0,
    AVIFDEC_RGBA8 = 1,
    AVIFDEC_RGB16 = 2,
    AVIFDEC_RGBA16 = 3
} AvifdecRgbFormat;

typedef enum {
    AVIFDEC_ALPHA_STRAIGHT = 0,
    AVIFDEC_ALPHA_PREMULTIPLIED = 1
} AvifdecAlphaMode;

typedef struct {
    void *pixels;
    size_t stride;
    uint32_t width;
    uint32_t height;
    uint8_t format;
    uint8_t alpha_mode;
} AvifdecRgbImage;

const char *avifdec_status_string(AvifdecStatus status);
uint64_t avifdec_capabilities(void);
int avifdec_clap_to_crop_rect(const AvifdecCleanAperture *clap,
                              uint32_t width,
                              uint32_t height,
                              AvifdecCropRect *crop);

AvifdecStatus avifdec_query(const void *data,
                            size_t size,
                            const AvifdecLimits *limits,
                            AvifdecSpan *spans,
                            size_t span_capacity,
                            AvifdecImageInfo *info,
                            AvifdecError *error);

AvifdecStatus avifdec_query_ex(const void *data,
                               size_t size,
                               const AvifdecLimits *limits,
                               const AvifdecExecutor *executor,
                               AvifdecSpan *spans,
                               size_t span_capacity,
                               AvifdecImageInfo *info,
                               AvifdecError *error);

AvifdecStatus avifdec_trace(const void *data,
                            size_t size,
                            const AvifdecLimits *limits,
                            void *workspace,
                            size_t workspace_size,
                            AvifdecEntropyTrace *trace,
                            AvifdecError *error);

AvifdecStatus avifdec_decode(const void *data,
                             size_t size,
                             const AvifdecLimits *limits,
                             void *workspace,
                             size_t workspace_size,
                             AvifdecImage *image,
                             AvifdecEntropyTrace *trace,
                             AvifdecError *error);

AvifdecStatus avifdec_decode_ex(const void *data,
                                size_t size,
                                const AvifdecLimits *limits,
                                const AvifdecExecutor *executor,
                                void *workspace,
                                size_t workspace_size,
                                AvifdecImage *image,
                                AvifdecEntropyTrace *trace,
                                AvifdecError *error);

AvifdecStatus avifdec_image_to_rgb(const AvifdecImage *image,
                                   const AvifdecImageInfo *info,
                                   AvifdecRgbImage *rgb,
                                   AvifdecError *error);
AvifdecStatus avifdec_image_to_rgb_row(const AvifdecImage *image,
                                       const AvifdecImageInfo *info,
                                       AvifdecRgbImage *rgb,
                                       uint32_t row,
                                       AvifdecError *error);

const char *avifdec_version_string(void);

/*
 * AVIF image sequence ("avis") support.
 *
 * A sequence is described by exactly one non-auxiliary 'pict'/'av01' track
 * (the main track) and, optionally, one auxiliary alpha track linked to it
 * through 'tref' 'auxl'/'prem' boxes. Frames are numbered 0..frame_count-1
 * in sample/decoding order. Random access to frame N re-decodes every
 * sample from the nearest preceding sync sample through N; this keeps the
 * core allocation- and I/O-free at the cost of repeated work for long
 * gaps between sync samples, bounded by AvifdecLimits.max_frames.
 */
typedef struct {
    uint32_t main_track_id;
    uint32_t alpha_track_id;
    uint32_t width;
    uint32_t height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    AvifdecCropRect crop;
    uint8_t transform_flags;
    AvifdecCleanAperture clean_aperture;
    uint32_t pixel_aspect_h_spacing;
    uint32_t pixel_aspect_v_spacing;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
    uint8_t has_nclx;
    const unsigned char *icc_data;
    size_t icc_size;
    uint8_t profile;
    uint8_t level;
    uint8_t tier;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    uint32_t timescale;
    uint64_t duration;
    size_t frame_count;
    size_t workspace_required;
    uint8_t has_alpha;
    uint8_t alpha_premultiplied;
    uint8_t repeat_forever;
    uint8_t repeat_count_present;
    int32_t repeat_count;
} AvifdecSequenceInfo;

typedef struct {
    AvifdecImageInfo image;
    size_t frame_index;
    size_t sync_frame_index;
    uint64_t dts;
    int64_t pts;
    uint32_t duration;
    uint8_t is_sync;
    size_t sample_size;
} AvifdecFrameInfo;

AvifdecStatus avifdec_sequence_query(const void *data,
                                     size_t size,
                                     const AvifdecLimits *limits,
                                     AvifdecSequenceInfo *info,
                                     AvifdecError *error);

AvifdecStatus avifdec_sequence_frame_query(const void *data,
                                           size_t size,
                                           const AvifdecLimits *limits,
                                           size_t frame_index,
                                           AvifdecFrameInfo *frame,
                                           AvifdecError *error);

AvifdecStatus avifdec_sequence_decode_frame(const void *data,
                                            size_t size,
                                            const AvifdecLimits *limits,
                                            size_t frame_index,
                                            void *workspace,
                                            size_t workspace_size,
                                            AvifdecImage *image,
                                            AvifdecEntropyTrace *trace,
                                            AvifdecFrameInfo *frame,
                                            AvifdecError *error);

#endif