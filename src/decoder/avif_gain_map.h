#ifndef AVIF_GAIN_MAP_H
#define AVIF_GAIN_MAP_H

#include "avifdec.h"

#define AVIF_GAIN_MAP_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | \
     (uint32_t)(unsigned char)(d))

#define AVIF_GAIN_MAP_TMAP \
    AVIF_GAIN_MAP_FOURCC('t', 'm', 'a', 'p')
#define AVIF_GAIN_MAP_DIMG \
    AVIF_GAIN_MAP_FOURCC('d', 'i', 'm', 'g')
#define AVIF_GAIN_MAP_ALTR \
    AVIF_GAIN_MAP_FOURCC('a', 'l', 't', 'r')

#define AVIF_GAIN_MAP_RGBF32 4U
#define AVIF_GAIN_MAP_RGBAF32 5U

#if AVIFDEC_VERSION_MAJOR > 1U || AVIFDEC_VERSION_MINOR >= 4U
typedef AvifdecRational AvifGainMapRational;
typedef AvifdecUnsignedRational AvifGainMapUnsignedRational;
typedef AvifdecByteView AvifGainMapByteView;
typedef AvifdecColorDescription AvifGainMapColorDescription;
#else
typedef struct {
    int32_t numerator;
    uint32_t denominator;
} AvifGainMapRational;

typedef struct {
    uint32_t numerator;
    uint32_t denominator;
} AvifGainMapUnsignedRational;

typedef struct {
    const unsigned char *data;
    size_t size;
} AvifGainMapByteView;

typedef struct {
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
    uint8_t has_nclx;
    AvifGainMapByteView icc;
} AvifGainMapColorDescription;
#endif

typedef struct {
    /* ISO wire signedness: headrooms/gamma are unsigned; other fields signed. */
    AvifGainMapUnsignedRational base_hdr_headroom;
    AvifGainMapUnsignedRational alternate_hdr_headroom;
    AvifGainMapRational gain_map_min[3];
    AvifGainMapRational gain_map_max[3];
    AvifGainMapUnsignedRational gain_map_gamma[3];
    AvifGainMapRational base_offset[3];
    AvifGainMapRational alternate_offset[3];
    uint16_t minimum_version;
    uint16_t writer_version;
    uint8_t metadata_version;
    uint8_t channel_count;
    uint8_t use_base_color_space;
    uint8_t backward_direction;
    /* Set when parsed from compact form; all stored denominators are expanded. */
    uint8_t common_denominator;
} AvifGainMapMetadata;

typedef struct {
    AvifdecImageInfo base_image;
    AvifdecImageInfo gain_map_image;
    AvifGainMapColorDescription base_color;
    AvifGainMapColorDescription alternate_color;
    AvifGainMapMetadata metadata;
    uint32_t base_item_id;
    uint32_t alternate_item_id;
    uint32_t gain_map_item_id;
    size_t workspace_required;
    uint8_t present;
    uint8_t base_is_hdr;
} AvifGainMapInfo;

typedef AvifdecStatus (*AvifGainMapSpanAt)(
    void *context,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error);

typedef struct {
    void *context;
    size_t span_count;
    size_t payload_offset;
    AvifGainMapSpanAt span_at;
} AvifGainMapSpanSource;

AvifdecStatus avif_gain_map_parse_payload(
    const AvifGainMapSpanSource *source,
    AvifGainMapMetadata *metadata,
    AvifdecError *error);

AvifdecStatus avif_gain_map_parse_spans(
    const AvifdecSpan *spans,
    size_t span_count,
    size_t payload_offset,
    AvifGainMapMetadata *metadata,
    AvifdecError *error);

typedef struct {
    uint32_t id;
    uint32_t type;
    size_t source_offset;
    uint8_t hidden;
    uint8_t has_unsupported_essential_property;
    uint8_t is_thumbnail;
    AvifdecImageInfo properties;
    AvifGainMapColorDescription color;
} AvifGainMapIndexedItem;

typedef enum {
    AVIF_GAIN_MAP_ALTERNATIVE_NONE = 0,
    AVIF_GAIN_MAP_ALTERNATIVE_FIRST_BEFORE_SECOND = 1,
    AVIF_GAIN_MAP_ALTERNATIVE_SECOND_BEFORE_FIRST = 2
} AvifGainMapAlternativeOrder;

typedef AvifdecStatus (*AvifGainMapItemAt)(
    void *context,
    size_t item_index,
    AvifGainMapIndexedItem *item,
    AvifdecError *error);

/*
 * dimg writes at most id_capacity ordered IDs and always returns the complete
 * count. alternative_order describes one direct altr group and validates that
 * neither entity belongs to conflicting altr groups.
 */
typedef AvifdecStatus (*AvifGainMapDimg)(
    void *context,
    uint32_t from_item_id,
    uint32_t *to_item_ids,
    size_t id_capacity,
    size_t *id_count,
    size_t *reference_offset,
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapAlternative)(
    void *context,
    uint32_t first_item_id,
    uint32_t second_item_id,
    AvifGainMapAlternativeOrder *order,
    size_t *group_offset,
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapItemPayload)(
    void *context,
    uint32_t item_id,
    AvifGainMapSpanSource *source,
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapQueryChild)(
    void *context,
    uint32_t item_id,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    AvifdecError *error);

typedef struct {
    void *context;
    uint32_t primary_item_id;
    size_t item_count;
    AvifGainMapItemAt item_at;
    AvifGainMapDimg dimg;
    AvifGainMapAlternative alternative_order;
    AvifGainMapItemPayload item_payload;
    AvifGainMapQueryChild query_child;
} AvifGainMapItemIndex;

typedef struct {
    AvifGainMapInfo info;
} AvifGainMapDecodePlan;

AvifdecStatus avif_gain_map_query_decode_plan(
    const AvifGainMapItemIndex *index,
    const AvifdecExecutor *executor,
    AvifGainMapDecodePlan *plan,
    AvifdecError *error);

AvifdecStatus avif_gain_map_child_workspace(
    const AvifdecImageInfo *base_info,
    const AvifdecImageInfo *gain_info,
    size_t *workspace_required);

AvifdecStatus avif_gain_map_validate_decode_images(
    const AvifGainMapDecodePlan *plan,
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapDecodeChild)(
    void *context,
    uint32_t item_id,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error);

typedef struct {
    void *context;
    AvifGainMapDecodeChild decode;
} AvifGainMapChildDecoder;

AvifdecStatus avif_gain_map_execute_decode_plan(
    const AvifGainMapDecodePlan *plan,
    const AvifGainMapChildDecoder *decoder,
    const AvifdecExecutor *executor,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *base_image,
    AvifdecImage *gain_map_image,
    AvifdecEntropyTrace *base_trace,
    AvifdecEntropyTrace *gain_map_trace,
    AvifdecError *error);

typedef struct {
    /* Positive linear HDR/SDR display ratio; metadata headrooms are log2. */
    float display_headroom;
    uint32_t flags;
} AvifGainMapApplyOptions;

/*
 * base_to_working returns straight relative-linear RGBA (1.0 = 203 nits) at
 * base presentation coordinates. gain_texel returns normalized encoded gain
 * values at gain-map presentation coordinates. validate_transform must reject
 * a transform whose source is not working_color or whose destination
 * primaries/transfer are not explicit. working_to_linear must return
 * destination-primary relative-linear extended SDR with the same 203-nit
 * anchor; this module performs no luminance-scale conversion. Output callbacks
 * consume straight working-linear RGB; this
 * module preserves and associates alpha after color conversion. Every
 * successful pixel callback must initialize every component in its output.
 */
typedef AvifdecStatus (*AvifGainMapValidateColorTransform)(
    void *context,
    const AvifGainMapColorDescription *working_color,
    uint8_t output_format,
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapBaseToWorking)(
    void *context,
    const AvifdecImage *base_image,
    const AvifdecImageInfo *base_info,
    const AvifGainMapColorDescription *working_color,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapReadTexel)(
    void *context,
    const AvifdecImage *gain_map_image,
    const AvifdecImageInfo *gain_map_info,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float gain[3],
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapWorkingToLinear)(
    void *context,
    const float working_rgb[3],
    float output_rgb[3],
    AvifdecError *error);

typedef AvifdecStatus (*AvifGainMapWorkingToEncoded16)(
    void *context,
    const float working_rgb[3],
    uint16_t output_rgb[3],
    AvifdecError *error);

typedef struct {
    void *context;
    AvifGainMapValidateColorTransform validate_transform;
    AvifGainMapBaseToWorking base_to_working;
    AvifGainMapReadTexel gain_texel;
    AvifGainMapWorkingToLinear working_to_linear;
    AvifGainMapWorkingToEncoded16 working_to_encoded16;
} AvifGainMapColorAdapter;

AvifdecStatus avif_gain_map_apply(
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    const AvifGainMapInfo *info,
    const AvifGainMapColorAdapter *color,
    const AvifGainMapApplyOptions *options,
    AvifdecRgbImage *output,
    AvifdecError *error);

/*
 * Follows avifdec_image_to_rgb_row(): output->height remains the full
 * presentation height, while output->pixels points at storage for the
 * requested row. On callback failure, completed earlier pixels remain written;
 * the failing pixel is never stored from partial callback output.
 */
AvifdecStatus avif_gain_map_apply_row(
    const AvifdecImage *base_image,
    const AvifdecImage *gain_map_image,
    const AvifGainMapInfo *info,
    const AvifGainMapColorAdapter *color,
    const AvifGainMapApplyOptions *options,
    AvifdecRgbImage *output,
    uint32_t row,
    AvifdecError *error);

/* Binary32-bounded helpers: exp2 underflows below -150 and rejects overflow;
 * pow accepts a base in [0,1] and a finite positive exponent. */
AvifdecStatus avif_gain_map_approx_exp2(
    float exponent,
    float *result);

AvifdecStatus avif_gain_map_approx_pow(
    float base,
    float exponent,
    float *result);

#endif
