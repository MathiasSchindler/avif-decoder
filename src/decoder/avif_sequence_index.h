#ifndef AVIFDEC_SEQUENCE_INDEX_PRIVATE_H
#define AVIFDEC_SEQUENCE_INDEX_PRIVATE_H

#include "avifdec.h"

#if AVIFDEC_VERSION_MAJOR > 1U || AVIFDEC_VERSION_MINOR >= 4U
#define AVIF_SEQUENCE_INDEX_WORDS AVIFDEC_SEQUENCE_INDEX_WORDS
#define AVIF_SEQUENCE_TRACK_VISUAL AVIFDEC_SEQUENCE_TRACK_VISUAL
#define AVIF_SEQUENCE_TRACK_ALPHA AVIFDEC_SEQUENCE_TRACK_ALPHA
#define AVIF_SEQUENCE_TRACK_AUXILIARY AVIFDEC_SEQUENCE_TRACK_AUXILIARY
#define AVIF_SEQUENCE_TRACK_ALTERNATE AVIFDEC_SEQUENCE_TRACK_ALTERNATE
#define AVIF_SEQUENCE_TRACK_FRAGMENTED AVIFDEC_SEQUENCE_TRACK_FRAGMENTED
#define AVIF_SEQUENCE_SELECT_DISABLE_ALPHA \
    AVIFDEC_SEQUENCE_SELECT_DISABLE_ALPHA
#define AVIF_SEQUENCE_PRESENTATION_FRAGMENTED \
    AVIFDEC_SEQUENCE_PRESENTATION_FRAGMENTED

typedef AvifdecSequenceIndex AvifSequenceIndex;
typedef AvifdecByteView AvifSequenceByteView;
typedef AvifdecColorDescription AvifSequenceColorDescription;
typedef AvifdecSequenceIndexInfo AvifSequenceIndexInfo;
typedef AvifdecSequenceTrackInfo AvifSequenceTrackInfo;
typedef AvifdecSequenceTrackReferenceInfo AvifSequenceTrackReferenceInfo;
typedef AvifdecSequenceSelectOptions AvifSequenceSelectOptions;
typedef AvifdecSequenceSelection AvifSequenceSelection;
typedef AvifdecSequencePresentationInfo AvifSequencePresentationInfo;
#else
#define AVIF_SEQUENCE_INDEX_WORDS 8U

#define AVIF_SEQUENCE_TRACK_VISUAL ((uint32_t)1U << 0)
#define AVIF_SEQUENCE_TRACK_ALPHA ((uint32_t)1U << 1)
#define AVIF_SEQUENCE_TRACK_AUXILIARY ((uint32_t)1U << 2)
#define AVIF_SEQUENCE_TRACK_ALTERNATE ((uint32_t)1U << 3)
#define AVIF_SEQUENCE_TRACK_FRAGMENTED ((uint32_t)1U << 4)

#define AVIF_SEQUENCE_SELECT_DISABLE_ALPHA ((uint32_t)1U << 0)
#define AVIF_SEQUENCE_PRESENTATION_FRAGMENTED ((uint32_t)1U << 0)

typedef struct {
    uintptr_t opaque[AVIF_SEQUENCE_INDEX_WORDS];
} AvifSequenceIndex;

typedef struct {
    const unsigned char *data;
    size_t size;
} AvifSequenceByteView;

typedef struct {
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
    uint8_t has_nclx;
    AvifSequenceByteView icc;
} AvifSequenceColorDescription;

typedef struct {
    size_t workspace_required;
    size_t track_count;
    size_t track_reference_count;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
    uint32_t movie_timescale;
    uint64_t movie_duration;
} AvifSequenceIndexInfo;

typedef struct {
    uint32_t track_id;
    uint32_t handler_type;
    uint32_t flags;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t presentation_width;
    uint32_t presentation_height;
    AvifdecCropRect crop;
    AvifdecCleanAperture clean_aperture;
    AvifSequenceColorDescription color;
    int32_t matrix[9];
    uint32_t media_timescale;
    uint64_t media_duration;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
    uint8_t transform_flags;
    uint8_t irot_angle;
    uint8_t imir_axis;
    uint8_t bit_depth;
    uint8_t alpha_color_range;
} AvifSequenceTrackInfo;

typedef struct {
    uint32_t from_track_id;
    uint32_t to_track_id;
    uint32_t relationship_type;
} AvifSequenceTrackReferenceInfo;

typedef struct {
    uint32_t main_track_id;
    uint32_t alpha_track_id;
    uint32_t flags;
} AvifSequenceSelectOptions;

typedef struct {
    uint32_t main_track_id;
    uint32_t alpha_track_id;
    uint32_t flags;
    uint32_t timescale;
    uint64_t duration;
    size_t presentation_count;
    uint8_t has_alpha;
    uint8_t alpha_premultiplied;
} AvifSequenceSelection;

typedef struct {
    AvifdecImageInfo image;
    size_t presentation_index;
    size_t main_sample_index;
    size_t alpha_sample_index;
    size_t main_sync_sample_index;
    size_t alpha_sync_sample_index;
    uint64_t start_time;
    uint64_t duration;
    uint32_t timescale;
    uint32_t flags;
} AvifSequencePresentationInfo;
#endif

typedef struct {
    uint32_t coded_width;
    uint32_t coded_height;
    uint64_t semantic_id;
    uint8_t av1c_marker;
    uint8_t av1c_version;
    uint8_t profile;
    uint8_t level;
    uint8_t tier;
    uint8_t high_bitdepth;
    uint8_t twelve_bit;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    uint8_t color_description_present;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
} AvifSequenceHeaderValidation;

typedef AvifdecStatus (*AvifSequenceValidateHeader)(
    void *user_data,
    uint32_t track_id,
    const unsigned char *sequence_header_obu,
    size_t sequence_header_obu_size,
    const AvifdecLimits *limits,
    AvifSequenceHeaderValidation *validation,
    AvifdecError *error);

typedef struct {
    void *user_data;
    AvifSequenceValidateHeader validate_header;
} AvifSequenceValidationCallbacks;

typedef struct {
    uint32_t track_id;
    size_t sample_index;
    size_t sync_sample_index;
    uint64_t offset;
    size_t size;
    uint64_t dts;
    uint64_t duration;
    size_t config_offset;
    size_t config_size;
    uint8_t is_sync;
    uint8_t prepend_config;
    uint8_t fragmented;
} AvifSequenceSampleInfo;

AvifdecStatus avif_sequence_index_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifSequenceValidationCallbacks *validation,
    AvifSequenceIndexInfo *info,
    AvifdecError *error);

AvifdecStatus avif_sequence_index_init(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifSequenceValidationCallbacks *validation,
    void *workspace,
    size_t workspace_size,
    AvifSequenceIndex *index,
    AvifSequenceIndexInfo *info,
    AvifdecError *error);

AvifdecStatus avif_sequence_track_query(
    const AvifSequenceIndex *index,
    size_t track_index,
    AvifSequenceTrackInfo *track,
    AvifdecError *error);

AvifdecStatus avif_sequence_track_reference_query(
    const AvifSequenceIndex *index,
    size_t reference_index,
    AvifSequenceTrackReferenceInfo *reference,
    AvifdecError *error);

AvifdecStatus avif_sequence_select(
    const AvifSequenceIndex *index,
    const AvifSequenceSelectOptions *options,
    AvifSequenceSelection *selection,
    AvifdecError *error);

AvifdecStatus avif_sequence_presentation_query(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    size_t presentation_index,
    AvifSequencePresentationInfo *presentation,
    AvifdecError *error);

AvifdecStatus avif_sequence_track_image_query(
    const AvifSequenceIndex *index,
    uint32_t track_id,
    AvifdecImageInfo *image,
    AvifdecError *error);

AvifdecStatus avif_sequence_sample_query(
    const AvifSequenceIndex *index,
    uint32_t track_id,
    size_t sample_index,
    AvifSequenceSampleInfo *sample,
    AvifdecError *error);

AvifdecStatus avif_sequence_index_source(
    const AvifSequenceIndex *index,
    const unsigned char **data,
    size_t *size,
    AvifdecLimits *limits,
    AvifdecError *error);

AvifdecStatus avif_sequence_index_attach_extension(
    AvifSequenceIndex *index,
    size_t extension_size,
    AvifdecError *error);

AvifdecStatus avif_sequence_index_extension(
    const AvifSequenceIndex *index,
    const unsigned char **workspace,
    size_t *base_size,
    size_t *extension_size,
    AvifdecError *error);

AvifdecStatus avif_sequence_index_top_level_meta(
    const AvifSequenceIndex *index,
    size_t *meta_offset,
    AvifdecError *error);

#endif
