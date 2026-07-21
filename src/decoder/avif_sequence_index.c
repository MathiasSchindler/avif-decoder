#include "avif_sequence_index.h"

#include "av1_avif_conformance.h"
#include "base.h"
#include "bmff_child.h"

#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef INT32_C
#define INT32_C(value) value
#endif

#define SEQ_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | \
     (uint32_t)(unsigned char)(d))

#define SEQ_WORKSPACE_MAGIC UINT64_C(0x4156494653455149)
#define SEQ_WORKSPACE_VERSION 1U
#define SEQ_MAX_TRACKS 256U
#define SEQ_SAMPLE_SYNC ((uint32_t)1U << 0)
#define SEQ_SAMPLE_FRAGMENTED ((uint32_t)1U << 1)
#define SEQ_SAMPLE_PREPEND_CONFIG ((uint32_t)1U << 2)
#define SEQ_NO_FRAGMENT ((size_t)-1)

#define SEQ_HANDLE_MAGIC ((uintptr_t)UINT32_C(0x53455149))

typedef struct {
    uint64_t magic;
    uint64_t workspace_hash;
    size_t layout_size;
    size_t generation;
    size_t track_count;
    size_t reference_count;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
    size_t tracks_offset;
    size_t references_offset;
    size_t samples_offset;
    size_t fragments_offset;
    size_t edits_offset;
    size_t presentations_offset;
    uint64_t movie_duration;
    uint32_t movie_timescale;
    uint32_t presentation_timescale;
    uint32_t version;
    size_t extension_size;
    size_t top_level_meta_offset;
    AvifdecLimits limits;
} SeqWorkspaceHeader;

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
    int32_t matrix[9];
    uint64_t media_duration;
    uint64_t timeline_duration;
    uint64_t canonical_header_semantic_id;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
    size_t config_offset;
    size_t config_size;
    size_t canonical_header_offset;
    size_t canonical_header_size;
    size_t icc_offset;
    size_t icc_size;
    uint32_t media_timescale;
    uint32_t alternate_group;
    uint32_t pixel_aspect_h_spacing;
    uint32_t pixel_aspect_v_spacing;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t transform_flags;
    uint8_t irot_angle;
    uint8_t imir_axis;
    uint8_t profile;
    uint8_t level;
    uint8_t tier;
    uint8_t av1c_marker;
    uint8_t av1c_version;
    uint8_t high_bitdepth;
    uint8_t twelve_bit;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    uint8_t color_range;
    uint8_t bitstream_color_range;
    uint8_t has_nclx;
    uint8_t has_icc;
    uint8_t alpha_color_range;
    uint8_t enabled;
} SeqTrackRecord;

typedef struct {
    uint32_t from_track_id;
    uint32_t to_track_id;
    uint32_t relationship_type;
} SeqReferenceRecord;

typedef struct {
    uint64_t offset;
    uint64_t dts;
    uint64_t duration;
    size_t size;
    size_t sample_index;
    size_t sync_sample_index;
    size_t fragment_index;
    uint32_t track_id;
    uint32_t flags;
} SeqSampleRecord;

typedef struct {
    uint64_t decode_time;
    size_t first_sample_index;
    size_t sample_count;
    size_t moof_offset;
    uint32_t track_id;
    uint32_t sequence_number;
} SeqFragmentRecord;

typedef struct {
    uint64_t movie_start;
    uint64_t segment_duration;
    int64_t media_time;
    uint32_t track_id;
} SeqEditRecord;

typedef struct {
    uint64_t start_time;
    uint64_t duration;
    size_t sample_index;
    size_t serial;
    uint32_t track_id;
    uint32_t flags;
} SeqPresentationRecord;

typedef struct {
    uint32_t track_id;
    uint32_t description_index;
    uint32_t default_duration;
    uint32_t default_size;
    uint32_t default_flags;
} SeqTrexRecord;

typedef struct {
    AvifBmffChild trak;
    AvifBmffChild tkhd;
    AvifBmffChild tref;
    AvifBmffChild edts;
    AvifBmffChild elst;
    AvifBmffChild mdia;
    AvifBmffChild mdhd;
    AvifBmffChild hdlr;
    AvifBmffChild minf;
    AvifBmffChild stbl;
    AvifBmffChild stsd;
    AvifBmffChild stts;
    AvifBmffChild ctts;
    AvifBmffChild stsc;
    AvifBmffChild stsz;
    AvifBmffChild stz2;
    AvifBmffChild stco;
    AvifBmffChild co64;
    AvifBmffChild stss;
    SeqTrackRecord record;
    uint64_t expected_dts;
    size_t classic_sample_count;
    size_t next_sample_index;
    size_t previous_sync_sample;
    size_t edit_entry_count;
    size_t matched_presentations;
    uint32_t trex_default_duration;
    uint32_t trex_default_size;
    uint32_t trex_default_flags;
    uint32_t trex_description_index;
    uint32_t tkhd_display_width;
    uint32_t tkhd_display_height;
    uint64_t tkhd_duration;
    uint8_t has_trex;
    uint8_t indexed;
    uint8_t alpha_entry;
    uint8_t has_canonical_header;
    uint8_t config_has_header;
    uint8_t explicit_nclx;
    uint8_t edit_version;
    uint8_t tkhd_version;
    AvifSequenceHeaderValidation canonical_validation;
} SeqTrackDraft;

typedef struct {
    unsigned char *workspace;
    const SeqWorkspaceHeader *layout;
    size_t reference_count;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
} SeqWriter;

typedef struct {
    const unsigned char *data;
    size_t size;
    AvifdecLimits limits;
    const AvifSequenceValidationCallbacks *validation;
    AvifdecError *error;
    AvifBmffChild moov;
    AvifBmffChild mvhd;
    AvifBmffChild mvex;
    AvifBmffChild top_level_meta;
    SeqTrackDraft tracks[SEQ_MAX_TRACKS];
    SeqTrexRecord trex[SEQ_MAX_TRACKS];
    uint32_t physical_track_ids[SEQ_MAX_TRACKS];
    size_t track_count;
    size_t trex_count;
    size_t physical_track_count;
    size_t reference_count;
    size_t sample_count;
    size_t fragment_count;
    size_t edit_count;
    size_t presentation_count;
    size_t mdat_count;
    size_t property_count;
    size_t obu_count;
    uint32_t movie_timescale;
    uint32_t presentation_timescale;
    uint32_t next_track_id;
    uint64_t movie_duration;
    uint32_t previous_mfhd_sequence;
    uint8_t have_previous_mfhd;
    uint8_t saw_avis_brand;
    SeqWriter *writer;
} SeqParseContext;

static void seq_error_clear(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus seq_fail(
    SeqParseContext *context,
    AvifdecStatus status,
    size_t offset,
    uint32_t box_type) {
    if (context->error != 0 &&
        context->error->status == AVIFDEC_OK) {
        context->error->status = status;
        context->error->offset = offset;
        context->error->context = box_type;
    }
    return status;
}

static AvifdecStatus seq_query_fail(
    AvifdecStatus status,
    size_t offset,
    uint32_t box_type,
    AvifdecError *error) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = box_type;
    }
    return status;
}

static int seq_box_set(const AvifBmffChild *box) {
    return box->size != 0U;
}

static int seq_add_count(size_t *value, size_t amount) {
    return avifdec_size_add(*value, amount, value);
}

static AvifdecLimits seq_effective_limits(const AvifdecLimits *limits) {
    return avifdec_limits_effective(limits);
}

static void seq_store(
    unsigned char *workspace,
    size_t offset,
    const void *record,
    size_t record_size) {
    avifdec_memory_copy(workspace + offset, record, record_size);
}

static void seq_load(
    const unsigned char *workspace,
    size_t offset,
    void *record,
    size_t record_size) {
    avifdec_memory_copy(record, workspace + offset, record_size);
}

static int seq_layout_add_array(
    size_t *cursor,
    size_t count,
    size_t element_size,
    size_t *offset) {
    size_t bytes;

    *offset = *cursor;
    if (!avifdec_size_multiply(count, element_size, &bytes)) return 0;
    return avifdec_size_add(*cursor, bytes, cursor);
}

static int seq_layout_build(
    size_t track_count,
    size_t reference_count,
    size_t sample_count,
    size_t fragment_count,
    size_t edit_count,
    size_t presentation_count,
    SeqWorkspaceHeader *layout) {
    size_t cursor = sizeof(*layout);

    avifdec_memory_fill(layout, 0U, sizeof(*layout));
    layout->track_count = track_count;
    layout->reference_count = reference_count;
    layout->sample_count = sample_count;
    layout->fragment_count = fragment_count;
    layout->edit_count = edit_count;
    layout->presentation_count = presentation_count;
    if (!seq_layout_add_array(
            &cursor, track_count, sizeof(SeqTrackRecord),
            &layout->tracks_offset) ||
        !seq_layout_add_array(
            &cursor, reference_count, sizeof(SeqReferenceRecord),
            &layout->references_offset) ||
        !seq_layout_add_array(
            &cursor, sample_count, sizeof(SeqSampleRecord),
            &layout->samples_offset) ||
        !seq_layout_add_array(
            &cursor, fragment_count, sizeof(SeqFragmentRecord),
            &layout->fragments_offset) ||
        !seq_layout_add_array(
            &cursor, edit_count, sizeof(SeqEditRecord),
            &layout->edits_offset) ||
        !seq_layout_add_array(
            &cursor, presentation_count,
            sizeof(SeqPresentationRecord),
            &layout->presentations_offset)) {
        return 0;
    }
    layout->layout_size = cursor;
    return 1;
}

static uint64_t seq_workspace_hash(
    const unsigned char *workspace,
    size_t size) {
    const size_t skip = offsetof(SeqWorkspaceHeader, workspace_hash);
    const size_t skip_end = skip + sizeof(uint64_t);
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned char byte =
            index >= skip && index < skip_end ? 0U : workspace[index];
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uintptr_t seq_handle_cookie(
    const AvifSequenceIndex *index) {
    uintptr_t value = SEQ_HANDLE_MAGIC;
    size_t word;

    for (word = 1U; word < AVIF_SEQUENCE_INDEX_WORDS - 1U; ++word) {
        value ^= index->opaque[word] +
                 (value << 6U) + (value >> 2U);
    }
    return value;
}

static AvifdecStatus seq_index_open(
    const AvifSequenceIndex *index,
    const unsigned char **workspace,
    const unsigned char **data,
    SeqWorkspaceHeader *header,
    AvifdecError *error) {
    SeqWorkspaceHeader expected;
    uint64_t hash;

    seq_error_clear(error);
    if (index == 0 ||
        index->opaque[0] != SEQ_HANDLE_MAGIC ||
        index->opaque[AVIF_SEQUENCE_INDEX_WORDS - 1U] !=
            seq_handle_cookie(index) ||
        index->opaque[1] == (uintptr_t)0U ||
        index->opaque[2] == (uintptr_t)0U) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    *workspace = (const unsigned char *)index->opaque[1];
    *data = (const unsigned char *)index->opaque[2];
    seq_load(*workspace, 0U, header, sizeof(*header));
    if (header->magic != SEQ_WORKSPACE_MAGIC ||
        header->version != SEQ_WORKSPACE_VERSION ||
        header->layout_size != (size_t)index->opaque[3] ||
        header->generation != (size_t)index->opaque[5] ||
        header->presentation_timescale == 0U ||
        header->layout_size < sizeof(*header)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    if (!seq_layout_build(
            header->track_count, header->reference_count,
            header->sample_count, header->fragment_count,
            header->edit_count, header->presentation_count,
            &expected) ||
        expected.layout_size != header->layout_size ||
        expected.tracks_offset != header->tracks_offset ||
        expected.references_offset != header->references_offset ||
        expected.samples_offset != header->samples_offset ||
        expected.fragments_offset != header->fragments_offset ||
        expected.edits_offset != header->edits_offset ||
        expected.presentations_offset !=
            header->presentations_offset) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    hash = seq_workspace_hash(*workspace, header->layout_size);
    if (hash != header->workspace_hash ||
        (uintptr_t)hash != index->opaque[6]) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    return AVIFDEC_OK;
}

static int seq_track_record_find(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t track_id,
    size_t *track_index,
    SeqTrackRecord *record) {
    size_t index;

    for (index = 0U; index < header->track_count; ++index) {
        SeqTrackRecord candidate;

        seq_load(
            workspace,
            header->tracks_offset + index * sizeof(candidate),
            &candidate, sizeof(candidate));
        if (candidate.track_id == track_id) {
            if (track_index != 0) *track_index = index;
            if (record != 0) *record = candidate;
            return 1;
        }
    }
    return 0;
}

static int seq_sample_record_find(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t track_id,
    size_t sample_index,
    SeqSampleRecord *record) {
    size_t index;

    for (index = 0U; index < header->sample_count; ++index) {
        SeqSampleRecord candidate;

        seq_load(
            workspace,
            header->samples_offset + index * sizeof(candidate),
            &candidate, sizeof(candidate));
        if (candidate.track_id == track_id &&
            candidate.sample_index == sample_index) {
            *record = candidate;
            return 1;
        }
    }
    return 0;
}

static int seq_presentation_record_find(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t track_id,
    size_t presentation_index,
    SeqPresentationRecord *record) {
    size_t index;
    size_t ordinal = 0U;

    for (index = 0U; index < header->presentation_count; ++index) {
        SeqPresentationRecord candidate;

        seq_load(
            workspace,
            header->presentations_offset + index * sizeof(candidate),
            &candidate, sizeof(candidate));
        if (candidate.track_id != track_id) continue;
        if (ordinal == presentation_index) {
            *record = candidate;
            return 1;
        }
        ++ordinal;
    }
    return 0;
}

static uint64_t seq_gcd_u64(uint64_t left, uint64_t right) {
    while (right != 0U) {
        uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int seq_scale_exact(
    uint64_t value,
    uint32_t numerator,
    uint32_t denominator,
    uint64_t *result) {
    uint64_t reduced_value = value;
    uint64_t reduced_numerator = numerator;
    uint64_t reduced_denominator = denominator;
    uint64_t divisor;

    if (denominator == 0U) return 0;
    divisor = seq_gcd_u64(reduced_value, reduced_denominator);
    reduced_value /= divisor;
    reduced_denominator /= divisor;
    divisor = seq_gcd_u64(reduced_numerator, reduced_denominator);
    reduced_numerator /= divisor;
    reduced_denominator /= divisor;
    if (reduced_denominator != 1U ||
        (reduced_numerator != 0U &&
         reduced_value > UINT64_MAX / reduced_numerator)) {
        return 0;
    }
    *result = reduced_value * reduced_numerator;
    return 1;
}

static int seq_scale_ceil(
    uint64_t value,
    uint32_t numerator,
    uint32_t denominator,
    uint64_t *result) {
    uint64_t quotient;
    uint64_t remainder_product;
    uint64_t rounded_remainder;

    if (numerator == 0U || denominator == 0U) return 0;
    quotient = value / denominator;
    if (quotient > UINT64_MAX / numerator) return 0;
    *result = quotient * numerator;
    remainder_product =
        (value % denominator) * (uint64_t)numerator;
    rounded_remainder = remainder_product / denominator;
    if (remainder_product % denominator != 0U) {
        ++rounded_remainder;
    }
    if (rounded_remainder > UINT64_MAX - *result) return 0;
    *result += rounded_remainder;
    return 1;
}

static int seq_u64_add_signed(
    uint64_t base,
    int32_t delta,
    uint64_t *result) {
    if (delta < 0) {
        uint64_t magnitude = (uint64_t)(-(int64_t)delta);

        if (magnitude > base) return 0;
        *result = base - magnitude;
        return 1;
    }

    if ((uint64_t)delta > UINT64_MAX - base) return 0;
    *result = base + (uint64_t)delta;
    return 1;
}

static int64_t seq_load_i64be(const unsigned char *bytes) {
    uint64_t value = avifdec_load_u64be(bytes);

    if (value <= (uint64_t)INT64_MAX) return (int64_t)value;
    return -1 - (int64_t)(UINT64_MAX - value);
}

static int32_t seq_u32_to_i32(uint32_t value) {
    if (value <= (uint32_t)INT32_MAX) return (int32_t)value;
    return -1 - (int32_t)(UINT32_MAX - value);
}

static int seq_text_equal(
    const unsigned char *bytes,
    size_t length,
    const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        if (index >= length ||
            bytes[index] != (unsigned char)text[index]) {
            return 0;
        }
        ++index;
    }
    return index == length;
}

static int seq_crop_axis(
    uint32_t image_size,
    uint32_t aperture_size,
    int32_t offset_n,
    uint32_t offset_d,
    uint32_t *start) {
    int64_t delta = (int64_t)image_size - (int64_t)aperture_size;
    int64_t scaled_delta;
    int64_t offset_twice = (int64_t)offset_n * 2;
    int64_t numerator;
    int64_t denominator;
    int64_t value;

    if (offset_d == 0U ||
        (delta != 0 &&
         (delta > INT64_MAX / (int64_t)offset_d ||
          delta < INT64_MIN / (int64_t)offset_d))) {
        return 0;
    }
    scaled_delta = delta * (int64_t)offset_d;
    if ((offset_twice > 0 &&
         scaled_delta > INT64_MAX - offset_twice) ||
        (offset_twice < 0 &&
         scaled_delta < INT64_MIN - offset_twice)) {
        return 0;
    }
    numerator = scaled_delta + offset_twice;
    denominator = (int64_t)offset_d * 2;
    if (numerator % denominator != 0) return 0;
    value = numerator / denominator;
    if (value < 0 || (uint64_t)value > UINT32_MAX) return 0;
    *start = (uint32_t)value;
    return 1;
}

static int seq_clap_to_crop(
    const AvifdecCleanAperture *clap,
    uint32_t width,
    uint32_t height,
    AvifdecCropRect *crop) {
    uint32_t crop_width;
    uint32_t crop_height;

    if (clap->width_d == 0U || clap->height_d == 0U ||
        clap->horizontal_offset_d == 0U ||
        clap->vertical_offset_d == 0U ||
        clap->width_n == 0U || clap->height_n == 0U ||
        clap->width_n % clap->width_d != 0U ||
        clap->height_n % clap->height_d != 0U) {
        return 0;
    }
    crop_width = clap->width_n / clap->width_d;
    crop_height = clap->height_n / clap->height_d;
    if (crop_width == 0U || crop_height == 0U ||
        crop_width > width || crop_height > height ||
        !seq_crop_axis(
            width, crop_width, clap->horizontal_offset_n,
            clap->horizontal_offset_d, &crop->x) ||
        !seq_crop_axis(
            height, crop_height, clap->vertical_offset_n,
            clap->vertical_offset_d, &crop->y) ||
        crop->x > width - crop_width ||
        crop->y > height - crop_height) {
        return 0;
    }
    crop->width = crop_width;
    crop->height = crop_height;
    return 1;
}

static int seq_matrix_geometry(SeqTrackRecord *track) {
    int32_t a = track->matrix[0];
    int32_t b = track->matrix[1];
    int32_t c = track->matrix[3];
    int32_t d = track->matrix[4];
    uint8_t angle;
    uint8_t mirror = 0U;

    if (track->matrix[2] != 0 ||
        track->matrix[5] != 0 ||
        track->matrix[8] != INT32_C(0x40000000) ||
        (track->matrix[6] & 0xffff) != 0 ||
        (track->matrix[7] & 0xffff) != 0) {
        return 0;
    }
    if (a == INT32_C(0x00010000) && b == 0 &&
        c == 0 && d == INT32_C(0x00010000)) {
        angle = 0U;
    } else if (a == 0 && b == -INT32_C(0x00010000) &&
               c == INT32_C(0x00010000) && d == 0) {
        angle = 1U;
    } else if (a == -INT32_C(0x00010000) && b == 0 &&
               c == 0 && d == -INT32_C(0x00010000)) {
        angle = 2U;
    } else if (a == 0 && b == INT32_C(0x00010000) &&
               c == -INT32_C(0x00010000) && d == 0) {
        angle = 3U;
    } else if (a == -INT32_C(0x00010000) && b == 0 &&
               c == 0 && d == INT32_C(0x00010000)) {
        angle = 0U;
        mirror = 1U;
    } else if (a == 0 && b == -INT32_C(0x00010000) &&
               c == -INT32_C(0x00010000) && d == 0) {
        angle = 1U;
        mirror = 1U;
    } else if (a == INT32_C(0x00010000) && b == 0 &&
               c == 0 && d == -INT32_C(0x00010000)) {
        angle = 2U;
        mirror = 1U;
    } else if (a == 0 && b == INT32_C(0x00010000) &&
               c == INT32_C(0x00010000) && d == 0) {
        angle = 3U;
        mirror = 1U;
    } else {
        return 0;
    }
    track->irot_angle = angle;
    track->imir_axis = 1U;
    if (angle != 0U) track->transform_flags |= AVIFDEC_TRANSFORM_IROT;
    if (mirror != 0U) track->transform_flags |= AVIFDEC_TRANSFORM_IMIR;
    if ((angle & 1U) != 0U) {
        track->presentation_width = track->crop.height;
        track->presentation_height = track->crop.width;
    } else {
        track->presentation_width = track->crop.width;
        track->presentation_height = track->crop.height;
    }
    return 1;
}

static SeqTrackDraft *seq_track_by_id(
    SeqParseContext *context,
    uint32_t track_id,
    size_t *track_index) {
    size_t index;

    for (index = 0U; index < context->track_count; ++index) {
        if (context->tracks[index].record.track_id == track_id) {
            if (track_index != 0) *track_index = index;
            return &context->tracks[index];
        }
    }
    return 0;
}

static int seq_physical_track_exists(
    const SeqParseContext *context,
    uint32_t track_id) {
    size_t index;

    for (index = 0U;
         index < context->physical_track_count; ++index) {
        if (context->physical_track_ids[index] == track_id) return 1;
    }
    return 0;
}

static AvifdecStatus seq_property_seen(
    SeqParseContext *context,
    size_t offset,
    uint32_t type) {
    if (!seq_add_count(&context->property_count, 1U)) {
        return seq_fail(context, AVIFDEC_OVERFLOW, offset, type);
    }
    if (context->property_count > context->limits.max_properties) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, offset, type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_child_iterator(
    SeqParseContext *context,
    const AvifBmffChild *parent,
    size_t prefix,
    AvifBmffChildIterator *iterator) {
    if (prefix > parent->payload_size) {
        return seq_fail(
            context, AVIFDEC_TRUNCATED, parent->payload_offset,
            parent->type);
    }
    return avif_bmff_child_iterator_init(
        iterator, context->data, context->size,
        parent->payload_offset + prefix,
        parent->payload_size - prefix, parent->type, context->error);
}

static AvifdecStatus seq_next_child(
    SeqParseContext *context,
    AvifBmffChildIterator *iterator,
    AvifBmffChild *child,
    int *has_child) {
    AvifdecStatus status =
        avif_bmff_child_next(iterator, child, has_child, context->error);

    if (status != AVIFDEC_OK) return status;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_unique_box(
    SeqParseContext *context,
    AvifBmffChild *destination,
    const AvifBmffChild *box) {
    if (seq_box_set(destination)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    *destination = *box;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_range_in_mdat(
    SeqParseContext *context,
    uint64_t offset,
    uint64_t size,
    uint32_t source_type) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status;

    if (offset > (uint64_t)SIZE_MAX ||
        size > (uint64_t)SIZE_MAX ||
        offset > UINT64_MAX - size) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            offset > (uint64_t)SIZE_MAX ? context->size : (size_t)offset,
            source_type);
    }
    status = avif_bmff_child_iterator_init(
        &iterator, context->data, context->size, 0U,
        context->size, 0U, context->error);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        uint64_t start;
        uint64_t end;

        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type != SEQ_FOURCC('m', 'd', 'a', 't')) continue;
        start = box.payload_offset;
        end = start + box.payload_size;
        if (offset >= start && offset <= end &&
            size <= end - offset) {
            return AVIFDEC_OK;
        }
    }
    if (status != AVIFDEC_OK) return status;
    return seq_fail(
        context, AVIFDEC_INVALID_DATA, (size_t)offset, source_type);
}

typedef struct {
    size_t sequence_header_offset;
    size_t sequence_header_size;
    size_t obu_count;
    uint8_t has_sequence_header;
} SeqObuScan;

static AvifdecStatus seq_scan_obus(
    SeqParseContext *context,
    size_t offset,
    size_t size,
    uint32_t source_type,
    SeqObuScan *scan) {
    AvifdecSpan span;
    size_t position = 0U;
    AvifdecStatus status;

    avifdec_memory_fill(scan, 0U, sizeof(*scan));
    span.data = context->data + offset;
    span.size = size;
    span.file_offset = offset;
    status = av1_avif_validate_obu_stream(
        &span, 1U, AVIFDEC_AV1_LOW_OVERHEAD, context->error);
    if (status != AVIFDEC_OK) return status;
    while (position < size) {
        size_t obu_start = position;
        uint8_t header;
        uint8_t obu_type;
        uint8_t extension_flag;
        uint64_t payload_size = 0U;
        unsigned int shift = 0U;
        unsigned int leb_bytes = 0U;

        header = context->data[offset + position++];
        obu_type = (uint8_t)((header >> 3U) & 15U);
        extension_flag = (uint8_t)((header >> 2U) & 1U);
        /* AV1-ISOBMFF v1.3 requires a size field on every config/sample OBU. */
        if ((header & 0x81U) != 0U ||
            (header & 2U) == 0U || obu_type == 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                offset + obu_start, source_type);
        }
        if (extension_flag != 0U) {
            uint8_t extension;

            if (position >= size) {
                return seq_fail(
                    context, AVIFDEC_TRUNCATED,
                    offset + position, source_type);
            }
            extension = context->data[offset + position++];
            if ((extension & 7U) != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    offset + position - 1U, source_type);
            }
        }
        for (;;) {
            uint8_t byte;
            uint64_t contribution;

            if (position >= size) {
                return seq_fail(
                    context, AVIFDEC_TRUNCATED,
                    offset + position, source_type);
            }
            if (leb_bytes >= 8U) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    offset + position, source_type);
            }
            byte = context->data[offset + position++];
            contribution = (uint64_t)(byte & 0x7fU);
            if (shift >= 64U ||
                contribution > (UINT64_MAX >> shift)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    offset + position - 1U, source_type);
            }
            payload_size |= contribution << shift;
            ++leb_bytes;
            if ((byte & 0x80U) == 0U) break;
            shift += 7U;
        }
        if (payload_size > (uint64_t)(size - position)) {
            return seq_fail(
                context, AVIFDEC_TRUNCATED,
                offset + position, source_type);
        }
        position += (size_t)payload_size;
        if (obu_type == 1U) {
            if (scan->has_sequence_header != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    offset + obu_start, source_type);
            }
            scan->has_sequence_header = 1U;
            scan->sequence_header_offset = offset + obu_start;
            scan->sequence_header_size = position - obu_start;
        }
        if (!seq_add_count(&scan->obu_count, 1U) ||
            !seq_add_count(&context->obu_count, 1U)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                offset + obu_start, source_type);
        }
        if (context->obu_count > context->limits.max_obus) {
            return seq_fail(
                context, AVIFDEC_LIMIT_EXCEEDED,
                offset + obu_start, source_type);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_validate_header(
    SeqParseContext *context,
    SeqTrackDraft *track,
    size_t offset,
    size_t size,
    uint32_t source_type) {
    AvifSequenceHeaderValidation validation;
    AvifdecStatus status;

    if (track->has_canonical_header != 0U) {
        if (size != track->record.canonical_header_size ||
            avifdec_memory_compare(
                context->data + offset,
                context->data +
                    track->record.canonical_header_offset,
                size) != 0) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, offset, source_type);
        }
    }
    avifdec_memory_fill(&validation, 0U, sizeof(validation));
    status = context->validation->validate_header(
        context->validation->user_data, track->record.track_id,
        context->data + offset, size, &context->limits,
        &validation, context->error);
    if (status != AVIFDEC_OK) {
        if (context->error != 0 &&
            context->error->status == AVIFDEC_OK) {
            context->error->status = status;
            context->error->offset = offset;
            context->error->context = source_type;
        }
        return status;
    }
    if (validation.coded_width != track->record.coded_width ||
        validation.coded_height != track->record.coded_height ||
        validation.av1c_marker != track->record.av1c_marker ||
        validation.av1c_version != track->record.av1c_version ||
        validation.profile != track->record.profile ||
        validation.level != track->record.level ||
        validation.tier != track->record.tier ||
        validation.high_bitdepth != track->record.high_bitdepth ||
        validation.twelve_bit != track->record.twelve_bit ||
        validation.bit_depth != track->record.bit_depth ||
        validation.monochrome != track->record.monochrome ||
        validation.subsampling_x != track->record.subsampling_x ||
        validation.subsampling_y != track->record.subsampling_y ||
        validation.chroma_sample_position !=
            track->record.chroma_sample_position ||
        validation.color_range > 1U ||
        (validation.color_description_present == 0U &&
         (validation.color_primaries != 0U ||
          validation.transfer_characteristics != 0U ||
          validation.matrix_coefficients != 0U)) ||
        (track->explicit_nclx != 0U &&
         (validation.color_range != track->record.color_range ||
          (validation.color_description_present != 0U &&
           ((validation.color_primaries != 2U &&
             validation.color_primaries !=
                 track->record.color_primaries) ||
            (validation.transfer_characteristics != 2U &&
             validation.transfer_characteristics !=
                 track->record.transfer_characteristics) ||
            (validation.matrix_coefficients != 2U &&
             validation.matrix_coefficients !=
                 track->record.matrix_coefficients)))))) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, offset, source_type);
    }
    if (track->has_canonical_header != 0U) {
        if (validation.coded_width !=
                track->canonical_validation.coded_width ||
            validation.coded_height !=
                track->canonical_validation.coded_height ||
            validation.semantic_id !=
                track->canonical_validation.semantic_id ||
            validation.av1c_marker !=
                track->canonical_validation.av1c_marker ||
            validation.av1c_version !=
                track->canonical_validation.av1c_version ||
            validation.profile !=
                track->canonical_validation.profile ||
            validation.level !=
                track->canonical_validation.level ||
            validation.tier !=
                track->canonical_validation.tier ||
            validation.high_bitdepth !=
                track->canonical_validation.high_bitdepth ||
            validation.twelve_bit !=
                track->canonical_validation.twelve_bit ||
            validation.bit_depth !=
                track->canonical_validation.bit_depth ||
            validation.monochrome !=
                track->canonical_validation.monochrome ||
            validation.subsampling_x !=
                track->canonical_validation.subsampling_x ||
            validation.subsampling_y !=
                track->canonical_validation.subsampling_y ||
            validation.chroma_sample_position !=
                track->canonical_validation.chroma_sample_position ||
            validation.color_description_present !=
                track->canonical_validation.color_description_present ||
            validation.color_primaries !=
                track->canonical_validation.color_primaries ||
            validation.transfer_characteristics !=
                track->canonical_validation.transfer_characteristics ||
            validation.matrix_coefficients !=
                track->canonical_validation.matrix_coefficients ||
            validation.color_range !=
                track->canonical_validation.color_range) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, offset, source_type);
        }
    } else {
        track->record.canonical_header_offset = offset;
        track->record.canonical_header_size = size;
        track->record.canonical_header_semantic_id =
            validation.semantic_id;
        track->canonical_validation = validation;
        track->record.alpha_color_range = validation.color_range;
        track->record.bitstream_color_range =
            validation.color_range;
        if (track->explicit_nclx == 0U &&
            validation.color_description_present != 0U) {
            track->record.color_primaries =
                validation.color_primaries;
            track->record.transfer_characteristics =
                validation.transfer_characteristics;
            track->record.matrix_coefficients =
                validation.matrix_coefficients;
            track->record.color_range = validation.color_range;
            track->record.has_nclx = 1U;
        }
        track->has_canonical_header = 1U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_ftyp(
    SeqParseContext *context,
    const AvifBmffChild *box) {
    const unsigned char *payload;
    size_t position;

    if (box->payload_size < 8U ||
        ((box->payload_size - 8U) & 3U) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    payload = context->data + box->payload_offset;
    if (avifdec_load_u32be(payload) ==
        SEQ_FOURCC('a', 'v', 'i', 's')) {
        context->saw_avis_brand = 1U;
    }
    for (position = 8U; position < box->payload_size;
         position += 4U) {
        if (avifdec_load_u32be(payload + position) ==
            SEQ_FOURCC('a', 'v', 'i', 's')) {
            context->saw_avis_brand = 1U;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_collect_stbl(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status;

    status = seq_child_iterator(
        context, &track->stbl, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) return status;
        if (box.type == SEQ_FOURCC('s', 't', 's', 'd')) {
            status = seq_unique_box(context, &track->stsd, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 't', 's')) {
            status = seq_unique_box(context, &track->stts, &box);
        } else if (box.type == SEQ_FOURCC('c', 't', 't', 's')) {
            status = seq_unique_box(context, &track->ctts, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 's', 'c')) {
            status = seq_unique_box(context, &track->stsc, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 's', 'z')) {
            status = seq_unique_box(context, &track->stsz, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 'z', '2')) {
            status = seq_unique_box(context, &track->stz2, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 'c', 'o')) {
            status = seq_unique_box(context, &track->stco, &box);
        } else if (box.type == SEQ_FOURCC('c', 'o', '6', '4')) {
            status = seq_unique_box(context, &track->co64, &box);
        } else if (box.type == SEQ_FOURCC('s', 't', 's', 's')) {
            status = seq_unique_box(context, &track->stss, &box);
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
}

static AvifdecStatus seq_collect_minf(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status =
        seq_child_iterator(context, &track->minf, 0U, &iterator);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('s', 't', 'b', 'l')) {
            status = seq_unique_box(context, &track->stbl, &box);
            if (status != AVIFDEC_OK) return status;
        }
    }
    if (status != AVIFDEC_OK) return status;
    if (seq_box_set(&track->stbl)) {
        return seq_collect_stbl(context, track);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_collect_mdia(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status =
        seq_child_iterator(context, &track->mdia, 0U, &iterator);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('m', 'd', 'h', 'd')) {
            status = seq_unique_box(context, &track->mdhd, &box);
        } else if (box.type == SEQ_FOURCC('h', 'd', 'l', 'r')) {
            status = seq_unique_box(context, &track->hdlr, &box);
        } else if (box.type == SEQ_FOURCC('m', 'i', 'n', 'f')) {
            status = seq_unique_box(context, &track->minf, &box);
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (status != AVIFDEC_OK) return status;
    if (seq_box_set(&track->minf)) {
        return seq_collect_minf(context, track);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_collect_edts(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status =
        seq_child_iterator(context, &track->edts, 0U, &iterator);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) return status;
        if (box.type == SEQ_FOURCC('e', 'l', 's', 't')) {
            status = seq_unique_box(context, &track->elst, &box);
            if (status != AVIFDEC_OK) return status;
        }
    }
}

static AvifdecStatus seq_collect_trak(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status =
        seq_child_iterator(context, &track->trak, 0U, &iterator);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('t', 'k', 'h', 'd')) {
            status = seq_unique_box(context, &track->tkhd, &box);
        } else if (box.type == SEQ_FOURCC('t', 'r', 'e', 'f')) {
            status = seq_unique_box(context, &track->tref, &box);
        } else if (box.type == SEQ_FOURCC('e', 'd', 't', 's')) {
            status = seq_unique_box(context, &track->edts, &box);
        } else if (box.type == SEQ_FOURCC('m', 'd', 'i', 'a')) {
            status = seq_unique_box(context, &track->mdia, &box);
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (status != AVIFDEC_OK) return status;
    if (seq_box_set(&track->edts)) {
        status = seq_collect_edts(context, track);
        if (status != AVIFDEC_OK) return status;
    }
    if (seq_box_set(&track->mdia)) {
        return seq_collect_mdia(context, track);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_collect_moov(
    SeqParseContext *context) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    size_t physical_track_count = 0U;
    int has_child;
    AvifdecStatus status =
        seq_child_iterator(context, &context->moov, 0U, &iterator);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('m', 'v', 'h', 'd')) {
            status = seq_unique_box(context, &context->mvhd, &box);
        } else if (box.type == SEQ_FOURCC('m', 'v', 'e', 'x')) {
            status = seq_unique_box(context, &context->mvex, &box);
        } else if (box.type == SEQ_FOURCC('t', 'r', 'a', 'k')) {
            if (physical_track_count >= SEQ_MAX_TRACKS ||
                physical_track_count >= context->limits.max_tracks) {
                return seq_fail(
                    context, AVIFDEC_LIMIT_EXCEEDED,
                    box.offset, box.type);
            }
            avifdec_memory_fill(
                &context->tracks[physical_track_count], 0U,
                sizeof(context->tracks[physical_track_count]));
            context->tracks[physical_track_count].trak = box;
            status = seq_collect_trak(
                context, &context->tracks[physical_track_count]);
            if (status != AVIFDEC_OK) return status;
            ++physical_track_count;
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    context->track_count = physical_track_count;
    return status;
}

static AvifdecStatus seq_collect_top_level(
    SeqParseContext *context) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    size_t ordinal = 0U;
    int has_child;
    int saw_moof = 0;
    AvifdecStatus status = avif_bmff_child_iterator_init(
        &iterator, context->data, context->size, 0U,
        context->size, 0U, context->error);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (ordinal == 0U &&
            box.type != SEQ_FOURCC('f', 't', 'y', 'p')) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box.offset, box.type);
        }
        if (box.type == SEQ_FOURCC('f', 't', 'y', 'p')) {
            if (ordinal != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    box.offset, box.type);
            }
            status = seq_parse_ftyp(context, &box);
        } else if (box.type == SEQ_FOURCC('m', 'o', 'o', 'v')) {
            if (saw_moof != 0) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    box.offset, box.type);
            }
            status = seq_unique_box(context, &context->moov, &box);
        } else if (box.type == SEQ_FOURCC('m', 'e', 't', 'a')) {
            status = seq_unique_box(
                context, &context->top_level_meta, &box);
        } else if (box.type == SEQ_FOURCC('m', 'd', 'a', 't')) {
            if (!seq_add_count(&context->mdat_count, 1U)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    box.offset, box.type);
            }
            if (context->mdat_count > context->limits.max_extents) {
                return seq_fail(
                    context, AVIFDEC_LIMIT_EXCEEDED,
                    box.offset, box.type);
            }
        } else if (box.type == SEQ_FOURCC('m', 'o', 'o', 'f')) {
            saw_moof = 1;
            if (!seq_box_set(&context->moov)) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    box.offset, box.type);
            }
        }
        if (status != AVIFDEC_OK) return status;
        ++ordinal;
    }
    if (status != AVIFDEC_OK) return status;
    if (ordinal == 0U || context->saw_avis_brand == 0U ||
        !seq_box_set(&context->moov)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, 0U,
            SEQ_FOURCC('f', 't', 'y', 'p'));
    }
    return seq_collect_moov(context);
}

static AvifdecStatus seq_parse_mvhd(SeqParseContext *context) {
    AvifdecByteReader reader;
    uint8_t version;
    size_t matrix_index;
    size_t predefined_index;
    static const uint32_t identity_matrix[9] = {
        0x00010000U, 0U, 0U,
        0U, 0x00010000U, 0U,
        0U, 0U, 0x40000000U
    };

    if (!seq_box_set(&context->mvhd)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            context->moov.offset, SEQ_FOURCC('m', 'v', 'h', 'd'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + context->mvhd.payload_offset,
        context->mvhd.payload_size, context->mvhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    if (avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        (version != 0U && version != 1U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            context->mvhd.offset, context->mvhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        context->movie_timescale = avifdec_byte_reader_u32be(&reader);
        context->movie_duration = avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        context->movie_timescale = avifdec_byte_reader_u32be(&reader);
        context->movie_duration =
            avifdec_byte_reader_u32be(&reader);
    }
    if (avifdec_byte_reader_u32be(&reader) != 0x00010000U ||
        avifdec_byte_reader_u16be(&reader) != 0x0100U ||
        avifdec_byte_reader_u16be(&reader) != 0U ||
        avifdec_byte_reader_u32be(&reader) != 0U ||
        avifdec_byte_reader_u32be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            context->mvhd.offset, context->mvhd.type);
    }
    for (matrix_index = 0U; matrix_index < 9U; ++matrix_index) {
        if (avifdec_byte_reader_u32be(&reader) !=
            identity_matrix[matrix_index]) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                context->mvhd.offset, context->mvhd.type);
        }
    }
    for (predefined_index = 0U;
         predefined_index < 6U; ++predefined_index) {
        if (avifdec_byte_reader_u32be(&reader) != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                context->mvhd.offset, context->mvhd.type);
        }
    }
    context->next_track_id =
        avifdec_byte_reader_u32be(&reader);
    if (context->next_track_id == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            context->mvhd.offset, context->mvhd.type);
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        context->movie_timescale == 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            context->mvhd.offset, context->mvhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_tkhd(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    size_t index;

    if (!seq_box_set(&track->tkhd)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->trak.offset, SEQ_FOURCC('t', 'k', 'h', 'd'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->tkhd.payload_offset,
        track->tkhd.payload_size, track->tkhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    track->tkhd_version = version;
    flags = (uint32_t)avifdec_byte_reader_u8(&reader) << 16U;
    flags |= (uint32_t)avifdec_byte_reader_u8(&reader) << 8U;
    flags |= avifdec_byte_reader_u8(&reader);
    if (version != 0U && version != 1U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->tkhd.offset, track->tkhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        track->record.track_id =
            avifdec_byte_reader_u32be(&reader);
        (void)avifdec_byte_reader_skip(&reader, 4U);
        track->tkhd_duration =
            avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        track->record.track_id =
            avifdec_byte_reader_u32be(&reader);
        (void)avifdec_byte_reader_skip(&reader, 4U);
        track->tkhd_duration =
            avifdec_byte_reader_u32be(&reader);
    }
    (void)avifdec_byte_reader_skip(&reader, 8U);
    (void)avifdec_byte_reader_u16be(&reader);
    track->record.alternate_group =
        avifdec_byte_reader_u16be(&reader);
    (void)avifdec_byte_reader_u16be(&reader);
    (void)avifdec_byte_reader_skip(&reader, 2U);
    for (index = 0U; index < 9U; ++index) {
        track->record.matrix[index] =
            seq_u32_to_i32(
                avifdec_byte_reader_u32be(&reader));
    }
    width = avifdec_byte_reader_u32be(&reader);
    height = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK ||
        track->record.track_id == 0U ||
        (width & 0xffffU) != 0U ||
        (height & 0xffffU) != 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            track->tkhd.offset, track->tkhd.type);
    }
    track->record.enabled = (uint8_t)((flags & 1U) != 0U);
    track->tkhd_display_width = width >> 16U;
    track->tkhd_display_height = height >> 16U;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_mdhd(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifdecByteReader reader;
    uint8_t version;
    uint16_t language;

    if (!seq_box_set(&track->mdhd)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->mdia.offset, SEQ_FOURCC('m', 'd', 'h', 'd'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->mdhd.payload_offset,
        track->mdhd.payload_size, track->mdhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    if (avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        (version != 0U && version != 1U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->mdhd.offset, track->mdhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        track->record.media_timescale =
            avifdec_byte_reader_u32be(&reader);
        track->record.media_duration =
            avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        track->record.media_timescale =
            avifdec_byte_reader_u32be(&reader);
        track->record.media_duration =
            avifdec_byte_reader_u32be(&reader);
    }
    language = avifdec_byte_reader_u16be(&reader);
    if ((language & 0x8000U) != 0U ||
        ((language >> 10U) & 31U) == 0U ||
        ((language >> 10U) & 31U) > 26U ||
        ((language >> 5U) & 31U) == 0U ||
        ((language >> 5U) & 31U) > 26U ||
        (language & 31U) == 0U ||
        (language & 31U) > 26U ||
        avifdec_byte_reader_u16be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->mdhd.offset, track->mdhd.type);
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        track->record.media_timescale == 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            track->mdhd.offset, track->mdhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_hdlr(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifdecByteReader reader;

    if (!seq_box_set(&track->hdlr)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->mdia.offset, SEQ_FOURCC('h', 'd', 'l', 'r'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->hdlr.payload_offset,
        track->hdlr.payload_size, track->hdlr.payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->hdlr.offset, track->hdlr.type);
    }
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->hdlr.offset, track->hdlr.type);
    }
    track->record.handler_type =
        avifdec_byte_reader_u32be(&reader);
    if (avifdec_byte_reader_u32be(&reader) != 0U ||
        avifdec_byte_reader_u32be(&reader) != 0U ||
        avifdec_byte_reader_u32be(&reader) != 0U ||
        reader.status != AVIFDEC_OK ||
        track->record.handler_type == 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            track->hdlr.offset, track->hdlr.type);
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U &&
        context->data[
            track->hdlr.payload_offset +
            track->hdlr.payload_size - 1U] != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->hdlr.offset, track->hdlr.type);
    }
    return AVIFDEC_OK;
}

static int seq_visual_handler(uint32_t handler) {
    return handler == SEQ_FOURCC('p', 'i', 'c', 't') ||
           handler == SEQ_FOURCC('v', 'i', 'd', 'e') ||
           handler == SEQ_FOURCC('a', 'u', 'x', 'v');
}

static AvifdecStatus seq_parse_av1c(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const AvifBmffChild *box) {
    const unsigned char *payload =
        context->data + box->payload_offset;

    if (box->payload_size < 4U ||
        payload[0] != 0x81U ||
        (payload[3] & 0xe0U) != 0U ||
        ((payload[3] & 0x10U) == 0U &&
         (payload[3] & 0x0fU) != 0U) ||
        ((payload[2] & 0x40U) == 0U &&
         (payload[2] & 0x20U) != 0U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    track->record.profile = (uint8_t)(payload[1] >> 5U);
    track->record.level = (uint8_t)(payload[1] & 31U);
    track->record.tier = (uint8_t)(payload[2] >> 7U);
    track->record.av1c_marker = (uint8_t)(payload[0] >> 7U);
    track->record.av1c_version = (uint8_t)(payload[0] & 0x7fU);
    track->record.high_bitdepth =
        (uint8_t)((payload[2] >> 6U) & 1U);
    track->record.twelve_bit =
        (uint8_t)((payload[2] >> 5U) & 1U);
    track->record.bit_depth =
        (payload[2] & 0x40U) == 0U ? 8U
        : (payload[2] & 0x20U) == 0U ? 10U : 12U;
    track->record.monochrome =
        (uint8_t)((payload[2] >> 4U) & 1U);
    track->record.subsampling_x =
        (uint8_t)((payload[2] >> 3U) & 1U);
    track->record.subsampling_y =
        (uint8_t)((payload[2] >> 2U) & 1U);
    track->record.chroma_sample_position =
        (uint8_t)(payload[2] & 3U);
    track->record.config_offset = box->payload_offset + 4U;
    track->record.config_size = box->payload_size - 4U;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_colr(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const AvifBmffChild *box,
    int *seen_nclx,
    int *seen_icc) {
    const unsigned char *payload =
        context->data + box->payload_offset;
    uint32_t type;

    if (box->payload_size < 4U) {
        return seq_fail(
            context, AVIFDEC_TRUNCATED, box->offset, box->type);
    }
    type = avifdec_load_u32be(payload);
    if (type == SEQ_FOURCC('n', 'c', 'l', 'x')) {
        if (*seen_nclx != 0 || box->payload_size != 11U ||
            (payload[10] & 0x7fU) != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        track->record.color_primaries =
            avifdec_load_u16be(payload + 4U);
        track->record.transfer_characteristics =
            avifdec_load_u16be(payload + 6U);
        track->record.matrix_coefficients =
            avifdec_load_u16be(payload + 8U);
        track->record.color_range = (uint8_t)(payload[10] >> 7U);
        track->record.has_nclx = 1U;
        track->explicit_nclx = 1U;
        *seen_nclx = 1;
    } else if (type == SEQ_FOURCC('r', 'I', 'C', 'C') ||
               type == SEQ_FOURCC('p', 'r', 'o', 'f')) {
        if (*seen_icc != 0 || box->payload_size == 4U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        track->record.icc_offset = box->payload_offset + 4U;
        track->record.icc_size = box->payload_size - 4U;
        track->record.has_icc = 1U;
        *seen_icc = 1;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_clap(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const AvifBmffChild *box) {
    const unsigned char *payload =
        context->data + box->payload_offset;

    if (box->payload_size != 32U ||
        (track->record.transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    track->record.clean_aperture.width_n =
        avifdec_load_u32be(payload);
    track->record.clean_aperture.width_d =
        avifdec_load_u32be(payload + 4U);
    track->record.clean_aperture.height_n =
        avifdec_load_u32be(payload + 8U);
    track->record.clean_aperture.height_d =
        avifdec_load_u32be(payload + 12U);
    track->record.clean_aperture.horizontal_offset_n =
        seq_u32_to_i32(avifdec_load_u32be(payload + 16U));
    track->record.clean_aperture.horizontal_offset_d =
        avifdec_load_u32be(payload + 20U);
    track->record.clean_aperture.vertical_offset_n =
        seq_u32_to_i32(avifdec_load_u32be(payload + 24U));
    track->record.clean_aperture.vertical_offset_d =
        avifdec_load_u32be(payload + 28U);
    track->record.transform_flags |= AVIFDEC_TRANSFORM_CLAP;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_auxi(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const AvifBmffChild *box) {
    const unsigned char *payload =
        context->data + box->payload_offset;
    size_t length = 0U;

    if (box->payload_size < 5U ||
        avifdec_load_u32be(payload) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    while (4U + length < box->payload_size &&
           payload[4U + length] != 0U) {
        ++length;
    }
    if (4U + length >= box->payload_size) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    if (seq_text_equal(
            payload + 4U, length,
            "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha")) {
        track->alpha_entry = 1U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_stsd(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifBmffChildIterator iterator;
    AvifBmffChild entry;
    AvifBmffChild child;
    AvifdecByteReader reader;
    uint32_t entry_count;
    int has_entry;
    int has_child;
    int seen_av1c = 0;
    int seen_nclx = 0;
    int seen_icc = 0;
    int seen_pasp = 0;
    int seen_auxi = 0;
    size_t constant_index;
    AvifdecStatus status;

    if (!seq_box_set(&track->stsd)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stbl.offset, SEQ_FOURCC('s', 't', 's', 'd'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->stsd.payload_offset,
        track->stsd.payload_size, track->stsd.payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsd.offset, track->stsd.type);
    }
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK || entry_count != 1U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            track->stsd.offset, track->stsd.type);
    }
    status = avif_bmff_child_iterator_init(
        &iterator, context->data, context->size,
        avifdec_byte_reader_offset(&reader),
        avifdec_byte_reader_remaining(&reader),
        track->stsd.type, context->error);
    if (status != AVIFDEC_OK) return status;
    status = seq_next_child(
        context, &iterator, &entry, &has_entry);
    if (status != AVIFDEC_OK) return status;
    if (!has_entry) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsd.offset, track->stsd.type);
    }
    status = seq_next_child(
        context, &iterator, &child, &has_child);
    if (status != AVIFDEC_OK) return status;
    if (has_child) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            child.offset, track->stsd.type);
    }
    if (entry.type != SEQ_FOURCC('a', 'v', '0', '1')) {
        track->indexed = 0U;
        return AVIFDEC_OK;
    }
    if (entry.payload_size < 78U) {
        return seq_fail(
            context, AVIFDEC_TRUNCATED,
            entry.offset, entry.type);
    }
    avifdec_byte_reader_init(
        &reader, context->data + entry.payload_offset,
        78U, entry.payload_offset);
    for (constant_index = 0U; constant_index < 6U;
         ++constant_index) {
        if (avifdec_byte_reader_u8(&reader) != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                entry.offset, entry.type);
        }
    }
    if (avifdec_byte_reader_u16be(&reader) != 1U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            entry.offset, entry.type);
    }
    if (avifdec_byte_reader_u16be(&reader) != 0U ||
        avifdec_byte_reader_u16be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            entry.offset, entry.type);
    }
    for (constant_index = 0U; constant_index < 3U;
         ++constant_index) {
        if (avifdec_byte_reader_u32be(&reader) != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                entry.offset, entry.type);
        }
    }
    track->record.coded_width =
        avifdec_byte_reader_u16be(&reader);
    track->record.coded_height =
        avifdec_byte_reader_u16be(&reader);
    if (avifdec_byte_reader_u32be(&reader) != 0x00480000U ||
        avifdec_byte_reader_u32be(&reader) != 0x00480000U ||
        avifdec_byte_reader_u32be(&reader) != 0U ||
        avifdec_byte_reader_u16be(&reader) != 1U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            entry.offset, entry.type);
    }
    {
        uint8_t compressor_length =
            avifdec_byte_reader_u8(&reader);

        if (compressor_length > 31U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                entry.offset, entry.type);
        }
        for (constant_index = 0U; constant_index < 31U;
             ++constant_index) {
            uint8_t byte = avifdec_byte_reader_u8(&reader);

            if (constant_index >= compressor_length && byte != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    entry.offset, entry.type);
            }
        }
    }
    if (avifdec_byte_reader_u16be(&reader) != 0x0018U ||
        avifdec_byte_reader_u16be(&reader) != 0xffffU ||
        avifdec_byte_reader_remaining(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            entry.offset, entry.type);
    }
    if (reader.status != AVIFDEC_OK ||
        track->record.coded_width == 0U ||
        track->record.coded_height == 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            entry.offset, entry.type);
    }
    status = avif_bmff_child_iterator_init(
        &iterator, context->data, context->size,
        entry.payload_offset + 78U,
        entry.payload_size - 78U, entry.type, context->error);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &child, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        status = seq_property_seen(
            context, child.offset, child.type);
        if (status != AVIFDEC_OK) return status;
        if (child.type == SEQ_FOURCC('a', 'v', '1', 'C')) {
            if (seen_av1c != 0) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            status = seq_parse_av1c(context, track, &child);
            seen_av1c = 1;
        } else if (child.type == SEQ_FOURCC('c', 'o', 'l', 'r')) {
            status = seq_parse_colr(
                context, track, &child, &seen_nclx, &seen_icc);
        } else if (child.type == SEQ_FOURCC('p', 'a', 's', 'p')) {
            if (seen_pasp != 0 || child.payload_size != 8U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            track->record.pixel_aspect_h_spacing =
                avifdec_load_u32be(
                    context->data + child.payload_offset);
            track->record.pixel_aspect_v_spacing =
                avifdec_load_u32be(
                    context->data + child.payload_offset + 4U);
            if (track->record.pixel_aspect_h_spacing == 0U ||
                track->record.pixel_aspect_v_spacing == 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            track->record.transform_flags |= AVIFDEC_TRANSFORM_PASP;
            seen_pasp = 1;
            status = AVIFDEC_OK;
        } else if (child.type == SEQ_FOURCC('c', 'l', 'a', 'p')) {
            status = seq_parse_clap(context, track, &child);
        } else if (child.type == SEQ_FOURCC('a', 'u', 'x', 'i')) {
            if (seen_auxi != 0) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            status = seq_parse_auxi(context, track, &child);
            seen_auxi = 1;
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (status != AVIFDEC_OK) return status;
    if (seen_av1c == 0) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsd.offset, track->stsd.type);
    }
    track->record.crop.x = 0U;
    track->record.crop.y = 0U;
    track->record.crop.width = track->record.coded_width;
    track->record.crop.height = track->record.coded_height;
    if ((track->record.transform_flags &
         AVIFDEC_TRANSFORM_CLAP) != 0U &&
        !seq_clap_to_crop(
            &track->record.clean_aperture,
            track->record.coded_width,
            track->record.coded_height,
            &track->record.crop)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsd.offset, SEQ_FOURCC('c', 'l', 'a', 'p'));
    }
    if (!seq_matrix_geometry(&track->record)) {
        return seq_fail(
            context, AVIFDEC_UNSUPPORTED,
            track->tkhd.offset, track->tkhd.type);
    }
    {
        size_t pixels;

        if (track->record.coded_width > context->limits.max_width ||
            track->record.coded_height > context->limits.max_height ||
            !avifdec_size_multiply(
                track->record.coded_width,
                track->record.coded_height, &pixels) ||
            pixels > context->limits.max_pixels) {
            return seq_fail(
                context, AVIFDEC_LIMIT_EXCEEDED,
                entry.offset, entry.type);
        }
    }
    if (track->record.config_size != 0U) {
        SeqObuScan scan;

        status = seq_scan_obus(
            context, track->record.config_offset,
            track->record.config_size,
            SEQ_FOURCC('a', 'v', '1', 'C'), &scan);
        if (status != AVIFDEC_OK) return status;
        if (scan.has_sequence_header != 0U) {
            if (scan.sequence_header_offset !=
                track->record.config_offset) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    scan.sequence_header_offset,
                    SEQ_FOURCC('a', 'v', '1', 'C'));
            }
            track->config_has_header = 1U;
            status = seq_validate_header(
                context, track,
                scan.sequence_header_offset,
                scan.sequence_header_size,
                SEQ_FOURCC('a', 'v', '1', 'C'));
            if (status != AVIFDEC_OK) return status;
        }
    }
    track->indexed = 1U;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_tracks(
    SeqParseContext *context) {
    size_t physical_count = context->track_count;
    size_t input_index;
    size_t output_count = 0U;

    context->physical_track_count = physical_count;
    for (input_index = 0U; input_index < physical_count;
         ++input_index) {
        SeqTrackDraft *track = &context->tracks[input_index];
        size_t prior;
        AvifdecStatus status = seq_parse_tkhd(context, track);

        if (status != AVIFDEC_OK) return status;
        context->physical_track_ids[input_index] =
            track->record.track_id;
        if (track->record.track_id > context->next_track_id) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->tkhd.offset, track->tkhd.type);
        }
        for (prior = 0U; prior < input_index; ++prior) {
            if (context->tracks[prior].record.track_id ==
                track->record.track_id) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->tkhd.offset, track->tkhd.type);
            }
        }
    }
    for (input_index = 0U; input_index < physical_count;
         ++input_index) {
        SeqTrackDraft *track = &context->tracks[input_index];
        AvifdecStatus status;

        if (!seq_box_set(&track->mdia)) continue;
        status = seq_parse_hdlr(context, track);
        if (status != AVIFDEC_OK) return status;
        if (!seq_visual_handler(track->record.handler_type)) continue;
        if (!seq_box_set(&track->minf) ||
            !seq_box_set(&track->stbl)) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->trak.offset, track->trak.type);
        }
        status = seq_parse_mdhd(context, track);
        if (status != AVIFDEC_OK) return status;
        status = seq_parse_stsd(context, track);
        if (status != AVIFDEC_OK) return status;
        if (track->indexed == 0U) continue;
        if (seq_box_set(&track->ctts)) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->ctts.offset, track->ctts.type);
        }
        if (track->alpha_entry != 0U ||
            track->record.handler_type ==
                SEQ_FOURCC('a', 'u', 'x', 'v')) {
            if (track->alpha_entry != 0U) {
                track->record.flags |=
                    AVIF_SEQUENCE_TRACK_ALPHA |
                    AVIF_SEQUENCE_TRACK_AUXILIARY;
            } else {
                track->record.flags |=
                    AVIF_SEQUENCE_TRACK_AUXILIARY;
            }
        } else {
            track->record.flags |= AVIF_SEQUENCE_TRACK_VISUAL;
        }
        if (track->record.alternate_group != 0U) {
            track->record.flags |= AVIF_SEQUENCE_TRACK_ALTERNATE;
        }
        if (output_count != input_index) {
            context->tracks[output_count] = *track;
        }
        ++output_count;
    }
    context->track_count = output_count;
    if (context->track_count == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            context->moov.offset, SEQ_FOURCC('t', 'r', 'a', 'k'));
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_choose_presentation_timescale(
    SeqParseContext *context) {
    uint64_t timescale = context->movie_timescale;
    size_t track_index;

    for (track_index = 0U;
         track_index < context->track_count; ++track_index) {
        uint64_t media_timescale =
            context->tracks[track_index].record.media_timescale;
        uint64_t divisor =
            seq_gcd_u64(timescale, media_timescale);

        if (timescale / divisor >
            UINT32_MAX / media_timescale) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                context->tracks[track_index].mdhd.offset,
                context->tracks[track_index].mdhd.type);
        }
        timescale =
            (timescale / divisor) * media_timescale;
    }
    context->presentation_timescale = (uint32_t)timescale;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_writer_reference(
    SeqParseContext *context,
    const SeqReferenceRecord *record) {
    if (!seq_add_count(&context->reference_count, 1U)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, 0U,
            record->relationship_type);
    }
    if (context->reference_count > context->limits.max_extents) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, 0U,
            record->relationship_type);
    }
    if (context->writer != 0) {
        SeqWriter *writer = context->writer;

        if (writer->reference_count >=
            writer->layout->reference_count) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, 0U,
                record->relationship_type);
        }
        seq_store(
            writer->workspace,
            writer->layout->references_offset +
                writer->reference_count * sizeof(*record),
            record, sizeof(*record));
        ++writer->reference_count;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_writer_edit(
    SeqParseContext *context,
    const SeqEditRecord *record) {
    if (!seq_add_count(&context->edit_count, 1U)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, 0U,
            SEQ_FOURCC('e', 'l', 's', 't'));
    }
    if (context->edit_count > context->limits.max_edits) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, 0U,
            SEQ_FOURCC('e', 'l', 's', 't'));
    }
    if (context->writer != 0) {
        SeqWriter *writer = context->writer;

        if (writer->edit_count >= writer->layout->edit_count) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, 0U,
                SEQ_FOURCC('e', 'l', 's', 't'));
        }
        seq_store(
            writer->workspace,
            writer->layout->edits_offset +
                writer->edit_count * sizeof(*record),
            record, sizeof(*record));
        ++writer->edit_count;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_writer_fragment(
    SeqParseContext *context,
    const SeqFragmentRecord *record) {
    if (!seq_add_count(&context->fragment_count, 1U)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, record->moof_offset,
            SEQ_FOURCC('t', 'r', 'a', 'f'));
    }
    if (context->fragment_count > context->limits.max_fragments) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, record->moof_offset,
            SEQ_FOURCC('t', 'r', 'a', 'f'));
    }
    if (context->writer != 0) {
        SeqWriter *writer = context->writer;

        if (writer->fragment_count >=
            writer->layout->fragment_count) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, record->moof_offset,
                SEQ_FOURCC('t', 'r', 'a', 'f'));
        }
        seq_store(
            writer->workspace,
            writer->layout->fragments_offset +
                writer->fragment_count * sizeof(*record),
            record, sizeof(*record));
        ++writer->fragment_count;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_writer_presentation(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const SeqPresentationRecord *record) {
    if (!seq_add_count(&context->presentation_count, 1U) ||
        !seq_add_count(&track->record.presentation_count, 1U)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, 0U,
            SEQ_FOURCC('e', 'l', 's', 't'));
    }
    if (track->record.presentation_count >
        context->limits.max_frames) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, 0U,
            SEQ_FOURCC('e', 'l', 's', 't'));
    }
    if (context->writer != 0) {
        SeqWriter *writer = context->writer;

        if (writer->presentation_count >=
            writer->layout->presentation_count) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA, 0U,
                SEQ_FOURCC('e', 'l', 's', 't'));
        }
        seq_store(
            writer->workspace,
            writer->layout->presentations_offset +
                writer->presentation_count * sizeof(*record),
            record, sizeof(*record));
        ++writer->presentation_count;
    }
    ++track->matched_presentations;
    return AVIFDEC_OK;
}

typedef struct {
    uint64_t movie_start;
    uint64_t segment_duration;
    int64_t media_time;
} SeqEditValue;

static int seq_read_edit(
    const SeqParseContext *context,
    const SeqTrackDraft *track,
    size_t edit_index,
    SeqEditValue *edit) {
    size_t entry_size =
        track->edit_version == 1U ? 20U : 12U;
    size_t offset;
    size_t prior;
    uint64_t movie_start = 0U;

    if (edit_index >= track->edit_entry_count) return 0;
    offset = track->elst.payload_offset + 8U;
    for (prior = 0U; prior <= edit_index; ++prior) {
        const unsigned char *entry =
            context->data + offset + prior * entry_size;
        uint64_t segment_duration =
            track->edit_version == 1U
                ? avifdec_load_u64be(entry)
                : avifdec_load_u32be(entry);

        if (prior == edit_index) {
            edit->movie_start = movie_start;
            edit->segment_duration = segment_duration;
            edit->media_time =
                track->edit_version == 1U
                    ? seq_load_i64be(entry + 8U)
                    : (int64_t)seq_u32_to_i32(
                        avifdec_load_u32be(entry + 4U));
            return 1;
        }
        if (segment_duration > UINT64_MAX - movie_start) return 0;
        movie_start += segment_duration;
    }
    return 0;
}

static AvifdecStatus seq_parse_edits(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    AvifdecByteReader reader;
    uint32_t entry_count;
    uint32_t flags;
    uint64_t movie_start = 0U;
    size_t index;

    track->record.edit_count = 0U;
    track->edit_entry_count = 0U;
    if (!seq_box_set(&track->edts)) return AVIFDEC_OK;
    if (!seq_box_set(&track->elst)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->edts.offset, SEQ_FOURCC('e', 'l', 's', 't'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->elst.payload_offset,
        track->elst.payload_size, track->elst.payload_offset);
    track->edit_version = avifdec_byte_reader_u8(&reader);
    flags = (uint32_t)avifdec_byte_reader_u8(&reader) << 16U;
    flags |= (uint32_t)avifdec_byte_reader_u8(&reader) << 8U;
    flags |= avifdec_byte_reader_u8(&reader);
    if (flags > 1U ||
        (track->edit_version != 0U &&
         track->edit_version != 1U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->elst.offset, track->elst.type);
    }
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK ||
        entry_count > context->limits.max_edits) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_LIMIT_EXCEEDED,
            track->elst.offset, track->elst.type);
    }
    track->edit_entry_count = entry_count;
    track->record.edit_count = entry_count;
    for (index = 0U; index < entry_count; ++index) {
        SeqEditRecord record;
        uint64_t segment_duration;
        int64_t media_time;
        uint16_t rate_integer;
        uint16_t rate_fraction;
        AvifdecStatus status;

        avifdec_memory_fill(&record, 0U, sizeof(record));
        if (track->edit_version == 1U) {
            segment_duration =
                avifdec_byte_reader_u64be(&reader);
            {
                uint64_t raw_media_time =
                    avifdec_byte_reader_u64be(&reader);

                media_time = raw_media_time <=
                        (uint64_t)INT64_MAX
                    ? (int64_t)raw_media_time
                    : -1 - (int64_t)(
                        UINT64_MAX - raw_media_time);
            }
        } else {
            segment_duration =
                avifdec_byte_reader_u32be(&reader);
            media_time = (int64_t)seq_u32_to_i32(
                avifdec_byte_reader_u32be(&reader));
        }
        rate_integer = avifdec_byte_reader_u16be(&reader);
        rate_fraction = avifdec_byte_reader_u16be(&reader);
        if (reader.status != AVIFDEC_OK) {
            return seq_fail(
                context, reader.status,
                track->elst.offset, track->elst.type);
        }
        if (rate_integer != 1U || rate_fraction != 0U) {
            return seq_fail(
                context, AVIFDEC_UNSUPPORTED,
                track->elst.offset, track->elst.type);
        }
        if (segment_duration == 0U || media_time < -1) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->elst.offset, track->elst.type);
        }
        if (segment_duration > UINT64_MAX - movie_start) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                track->elst.offset, track->elst.type);
        }
        record.movie_start = movie_start;
        record.segment_duration = segment_duration;
        record.media_time = media_time;
        record.track_id = track->record.track_id;
        status = seq_property_seen(
            context, track->elst.offset, track->elst.type);
        if (status != AVIFDEC_OK) return status;
        status = seq_writer_edit(context, &record);
        if (status != AVIFDEC_OK) return status;
        movie_start += segment_duration;
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            avifdec_byte_reader_offset(&reader), track->elst.type);
    }
    if (entry_count != 0U &&
        !seq_scale_exact(
            movie_start, context->presentation_timescale,
            context->movie_timescale,
            &track->record.timeline_duration)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            track->elst.offset, track->elst.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_present_sample(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const SeqSampleRecord *sample) {
    uint64_t sample_end;

    if (sample->duration > UINT64_MAX - sample->dts) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, (size_t)sample->offset,
            SEQ_FOURCC('s', 't', 't', 's'));
    }
    sample_end = sample->dts + sample->duration;
    if (track->edit_entry_count == 0U) {
        SeqPresentationRecord presentation;
        uint64_t end;

        if (!seq_scale_exact(
                sample->dts, context->presentation_timescale,
                track->record.media_timescale,
                &presentation.start_time) ||
            !seq_scale_exact(
                sample->duration, context->presentation_timescale,
                track->record.media_timescale,
                &presentation.duration)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                (size_t)sample->offset,
                SEQ_FOURCC('s', 't', 't', 's'));
        }
        if (presentation.duration == 0U ||
            presentation.duration >
                UINT64_MAX - presentation.start_time) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                (size_t)sample->offset,
                SEQ_FOURCC('s', 't', 't', 's'));
        }
        end = presentation.start_time + presentation.duration;
        if (end > track->record.timeline_duration) {
            track->record.timeline_duration = end;
        }
        presentation.sample_index = sample->sample_index;
        presentation.serial = context->presentation_count;
        presentation.track_id = sample->track_id;
        presentation.flags =
            (sample->flags & SEQ_SAMPLE_FRAGMENTED) != 0U
                ? AVIF_SEQUENCE_PRESENTATION_FRAGMENTED : 0U;
        return seq_writer_presentation(
            context, track, &presentation);
    }
    {
        size_t edit_index;

        for (edit_index = 0U;
             edit_index < track->edit_entry_count; ++edit_index) {
            SeqEditValue edit;
            uint64_t sample_start_normalized;
            uint64_t sample_end_normalized;
            uint64_t edit_media_start_normalized;
            uint64_t edit_duration_normalized;
            uint64_t edit_media_end_normalized;
            uint64_t edit_movie_start_normalized;
            uint64_t intersection_start;
            uint64_t intersection_end;
            SeqPresentationRecord presentation;

            if (!seq_read_edit(
                    context, track, edit_index, &edit)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->elst.offset, track->elst.type);
            }
            if (edit.media_time == -1) continue;
            if (!seq_scale_exact(
                    sample->dts,
                    context->presentation_timescale,
                    track->record.media_timescale,
                    &sample_start_normalized) ||
                !seq_scale_exact(
                    sample_end,
                    context->presentation_timescale,
                    track->record.media_timescale,
                    &sample_end_normalized) ||
                !seq_scale_exact(
                    (uint64_t)edit.media_time,
                    context->presentation_timescale,
                    track->record.media_timescale,
                    &edit_media_start_normalized) ||
                !seq_scale_exact(
                    edit.segment_duration,
                    context->presentation_timescale,
                    context->movie_timescale,
                    &edit_duration_normalized) ||
                !seq_scale_exact(
                    edit.movie_start,
                    context->presentation_timescale,
                    context->movie_timescale,
                    &edit_movie_start_normalized)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->elst.offset, track->elst.type);
            }
            if (edit_media_start_normalized >
                UINT64_MAX - edit_duration_normalized) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->elst.offset, track->elst.type);
            }
            edit_media_end_normalized =
                edit_media_start_normalized +
                edit_duration_normalized;
            intersection_start =
                sample_start_normalized >
                    edit_media_start_normalized
                    ? sample_start_normalized
                    : edit_media_start_normalized;
            intersection_end =
                sample_end_normalized <
                    edit_media_end_normalized
                    ? sample_end_normalized
                    : edit_media_end_normalized;
            if (intersection_start >= intersection_end) continue;
            if (intersection_start -
                    edit_media_start_normalized >
                UINT64_MAX - edit_movie_start_normalized) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->elst.offset, track->elst.type);
            }
            presentation.start_time =
                edit_movie_start_normalized +
                intersection_start -
                edit_media_start_normalized;
            presentation.duration =
                intersection_end - intersection_start;
            presentation.sample_index = sample->sample_index;
            presentation.serial = context->presentation_count;
            presentation.track_id = sample->track_id;
            presentation.flags =
                (sample->flags & SEQ_SAMPLE_FRAGMENTED) != 0U
                    ? AVIF_SEQUENCE_PRESENTATION_FRAGMENTED : 0U;
            if (presentation.duration == 0U) continue;
            if (presentation.duration >
                UINT64_MAX - presentation.start_time) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->elst.offset, track->elst.type);
            }
            {
                AvifdecStatus status = seq_writer_presentation(
                    context, track, &presentation);
                if (status != AVIFDEC_OK) return status;
            }
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_emit_sample(
    SeqParseContext *context,
    SeqTrackDraft *track,
    uint64_t offset,
    uint64_t size,
    uint64_t dts,
    uint64_t duration,
    int is_sync,
    size_t fragment_index,
    uint32_t source_type) {
    SeqSampleRecord record;
    SeqObuScan scan;
    AvifdecStatus status;

    if (size == 0U || duration == 0U ||
        size > (uint64_t)SIZE_MAX) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            offset > (uint64_t)SIZE_MAX
                ? context->size : (size_t)offset,
            source_type);
    }
    if (track->next_sample_index >= context->limits.max_frames) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED,
            offset > (uint64_t)SIZE_MAX
                ? context->size : (size_t)offset,
            source_type);
    }
    status = seq_range_in_mdat(
        context, offset, size, source_type);
    if (status != AVIFDEC_OK) return status;
    status = seq_scan_obus(
        context, (size_t)offset, (size_t)size,
        source_type, &scan);
    if (status != AVIFDEC_OK) return status;
    if (scan.has_sequence_header != 0U) {
        status = seq_validate_header(
            context, track, scan.sequence_header_offset,
            scan.sequence_header_size, source_type);
        if (status != AVIFDEC_OK) return status;
    } else if (is_sync != 0 &&
               track->config_has_header == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            (size_t)offset, source_type);
    }
    if (track->next_sample_index == 0U && is_sync == 0) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            (size_t)offset, source_type);
    }
    avifdec_memory_fill(&record, 0U, sizeof(record));
    record.offset = offset;
    record.dts = dts;
    record.duration = duration;
    record.size = (size_t)size;
    record.sample_index = track->next_sample_index;
    if (is_sync != 0) {
        record.flags |= SEQ_SAMPLE_SYNC;
        track->previous_sync_sample = record.sample_index;
    }
    record.sync_sample_index = track->previous_sync_sample;
    record.fragment_index = fragment_index;
    record.track_id = track->record.track_id;
    if (fragment_index != SEQ_NO_FRAGMENT) {
        record.flags |= SEQ_SAMPLE_FRAGMENTED;
    }
    if (scan.has_sequence_header == 0U &&
        track->config_has_header != 0U) {
        record.flags |= SEQ_SAMPLE_PREPEND_CONFIG;
    }
    if (!seq_add_count(&context->sample_count, 1U) ||
        !seq_add_count(&track->record.sample_count, 1U)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            (size_t)offset, source_type);
    }
    if (context->writer != 0) {
        SeqWriter *writer = context->writer;

        if (writer->sample_count >=
            writer->layout->sample_count) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                (size_t)offset, source_type);
        }
        seq_store(
            writer->workspace,
            writer->layout->samples_offset +
                writer->sample_count * sizeof(record),
            &record, sizeof(record));
        ++writer->sample_count;
    }
    ++track->next_sample_index;
    return seq_present_sample(context, track, &record);
}

static AvifdecStatus seq_sample_count_and_sizes(
    SeqParseContext *context,
    SeqTrackDraft *track,
    size_t *sample_count) {
    const AvifBmffChild *box;
    const unsigned char *payload;
    uint32_t count;
    size_t expected;

    if (seq_box_set(&track->stsz) ==
        seq_box_set(&track->stz2)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stbl.offset, SEQ_FOURCC('s', 't', 's', 'z'));
    }
    box = seq_box_set(&track->stsz)
        ? &track->stsz : &track->stz2;
    payload = context->data + box->payload_offset;
    if (seq_box_set(&track->stsz)) {
        uint32_t fixed_size;
        size_t bytes;

        if (box->payload_size < 12U ||
            avifdec_load_u32be(payload) != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        fixed_size = avifdec_load_u32be(payload + 4U);
        count = avifdec_load_u32be(payload + 8U);
        if (fixed_size == 0U) {
            if (!avifdec_size_multiply(
                    count, 4U, &bytes) ||
                !avifdec_size_add(12U, bytes, &expected)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    box->offset, box->type);
            }
        } else {
            expected = 12U;
        }
    } else {
        uint8_t field_size;
        size_t bytes;

        if (box->payload_size < 12U ||
            avifdec_load_u32be(payload) != 0U ||
            payload[4] != 0U || payload[5] != 0U ||
            payload[6] != 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        field_size = payload[7];
        count = avifdec_load_u32be(payload + 8U);
        if (field_size != 4U &&
            field_size != 8U && field_size != 16U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        if (field_size == 4U) {
            bytes = ((size_t)count + 1U) / 2U;
            if ((count & 1U) != 0U &&
                box->payload_size >= 12U + bytes &&
                (payload[12U + bytes - 1U] & 15U) != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    box->offset, box->type);
            }
        } else {
            if (!avifdec_size_multiply(
                    count, field_size / 8U, &bytes)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    box->offset, box->type);
            }
        }
        if (!avifdec_size_add(12U, bytes, &expected)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                box->offset, box->type);
        }
    }
    if (box->payload_size != expected) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            box->offset, box->type);
    }
    if ((size_t)count > context->limits.max_frames) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED,
            box->offset, box->type);
    }
    *sample_count = count;
    return AVIFDEC_OK;
}

static int seq_sample_size_at(
    const SeqParseContext *context,
    const SeqTrackDraft *track,
    size_t sample_index,
    uint32_t *sample_size) {
    const AvifBmffChild *box = seq_box_set(&track->stsz)
        ? &track->stsz : &track->stz2;
    const unsigned char *payload =
        context->data + box->payload_offset;

    if (seq_box_set(&track->stsz)) {
        uint32_t fixed = avifdec_load_u32be(payload + 4U);

        *sample_size = fixed != 0U
            ? fixed
            : avifdec_load_u32be(payload + 12U + sample_index * 4U);
    } else {
        uint8_t field_size = payload[7];
        const unsigned char *sizes = payload + 12U;

        if (field_size == 4U) {
            uint8_t packed = sizes[sample_index / 2U];
            *sample_size = (sample_index & 1U) == 0U
                ? packed >> 4U : packed & 15U;
        } else if (field_size == 8U) {
            *sample_size = sizes[sample_index];
        } else {
            *sample_size =
                avifdec_load_u16be(sizes + sample_index * 2U);
        }
    }
    return *sample_size != 0U;
}

typedef struct {
    AvifdecByteReader reader;
    uint32_t entries_remaining;
    uint32_t samples_remaining;
    uint32_t duration;
    uint64_t dts;
} SeqTimingCursor;

static AvifdecStatus seq_timing_init(
    SeqParseContext *context,
    SeqTrackDraft *track,
    size_t sample_count,
    SeqTimingCursor *cursor) {
    uint32_t entry_count;

    if (!seq_box_set(&track->stts)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stbl.offset, SEQ_FOURCC('s', 't', 't', 's'));
    }
    avifdec_byte_reader_init(
        &cursor->reader,
        context->data + track->stts.payload_offset,
        track->stts.payload_size, track->stts.payload_offset);
    if (avifdec_byte_reader_u32be(&cursor->reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stts.offset, track->stts.type);
    }
    entry_count = avifdec_byte_reader_u32be(&cursor->reader);
    if ((sample_count == 0U && entry_count != 0U) ||
        (sample_count != 0U && entry_count == 0U) ||
        (size_t)entry_count > sample_count) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stts.offset, track->stts.type);
    }
    cursor->entries_remaining = entry_count;
    cursor->samples_remaining = 0U;
    cursor->duration = 0U;
    cursor->dts = 0U;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_timing_next(
    SeqParseContext *context,
    SeqTrackDraft *track,
    SeqTimingCursor *cursor,
    uint64_t *dts,
    uint64_t *duration) {
    if (cursor->samples_remaining == 0U) {
        if (cursor->entries_remaining == 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->stts.offset, track->stts.type);
        }
        cursor->samples_remaining =
            avifdec_byte_reader_u32be(&cursor->reader);
        cursor->duration =
            avifdec_byte_reader_u32be(&cursor->reader);
        --cursor->entries_remaining;
        if (cursor->reader.status != AVIFDEC_OK ||
            cursor->samples_remaining == 0U ||
            cursor->duration == 0U) {
            return seq_fail(
                context,
                cursor->reader.status != AVIFDEC_OK
                    ? cursor->reader.status
                    : AVIFDEC_INVALID_DATA,
                track->stts.offset, track->stts.type);
        }
    }
    *dts = cursor->dts;
    *duration = cursor->duration;
    if ((uint64_t)cursor->duration >
        UINT64_MAX - cursor->dts) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            track->stts.offset, track->stts.type);
    }
    cursor->dts += cursor->duration;
    --cursor->samples_remaining;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_timing_finish(
    SeqParseContext *context,
    SeqTrackDraft *track,
    SeqTimingCursor *cursor) {
    if (cursor->reader.status != AVIFDEC_OK ||
        cursor->entries_remaining != 0U ||
        cursor->samples_remaining != 0U ||
        avifdec_byte_reader_remaining(&cursor->reader) != 0U) {
        return seq_fail(
            context,
            cursor->reader.status != AVIFDEC_OK
                ? cursor->reader.status : AVIFDEC_INVALID_DATA,
            track->stts.offset, track->stts.type);
    }
    track->expected_dts = cursor->dts;
    return AVIFDEC_OK;
}

typedef struct {
    AvifdecByteReader reader;
    uint32_t entries_remaining;
    uint32_t current_first_chunk;
    uint32_t current_samples_per_chunk;
    uint32_t next_first_chunk;
    uint32_t next_samples_per_chunk;
    uint8_t has_next;
} SeqChunkMapCursor;

static AvifdecStatus seq_chunk_map_read_entry(
    SeqParseContext *context,
    SeqTrackDraft *track,
    SeqChunkMapCursor *cursor,
    uint32_t *first_chunk,
    uint32_t *samples_per_chunk) {
    uint32_t description_index;

    if (cursor->entries_remaining == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    *first_chunk = avifdec_byte_reader_u32be(&cursor->reader);
    *samples_per_chunk =
        avifdec_byte_reader_u32be(&cursor->reader);
    description_index =
        avifdec_byte_reader_u32be(&cursor->reader);
    --cursor->entries_remaining;
    if (cursor->reader.status != AVIFDEC_OK ||
        *first_chunk == 0U || *samples_per_chunk == 0U ||
        description_index != 1U) {
        return seq_fail(
            context,
            cursor->reader.status != AVIFDEC_OK
                ? cursor->reader.status : AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_chunk_map_init(
    SeqParseContext *context,
    SeqTrackDraft *track,
    size_t sample_count,
    SeqChunkMapCursor *cursor) {
    uint32_t entry_count;
    AvifdecStatus status;

    if (!seq_box_set(&track->stsc)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stbl.offset, SEQ_FOURCC('s', 't', 's', 'c'));
    }
    avifdec_byte_reader_init(
        &cursor->reader,
        context->data + track->stsc.payload_offset,
        track->stsc.payload_size, track->stsc.payload_offset);
    if (avifdec_byte_reader_u32be(&cursor->reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    entry_count = avifdec_byte_reader_u32be(&cursor->reader);
    if ((sample_count == 0U && entry_count != 0U) ||
        (sample_count != 0U && entry_count == 0U) ||
        (size_t)entry_count > sample_count) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    cursor->entries_remaining = entry_count;
    cursor->has_next = 0U;
    if (entry_count == 0U) return AVIFDEC_OK;
    status = seq_chunk_map_read_entry(
        context, track, cursor,
        &cursor->current_first_chunk,
        &cursor->current_samples_per_chunk);
    if (status != AVIFDEC_OK) return status;
    if (cursor->current_first_chunk != 1U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    if (cursor->entries_remaining != 0U) {
        status = seq_chunk_map_read_entry(
            context, track, cursor,
            &cursor->next_first_chunk,
            &cursor->next_samples_per_chunk);
        if (status != AVIFDEC_OK) return status;
        if (cursor->next_first_chunk <=
            cursor->current_first_chunk) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->stsc.offset, track->stsc.type);
        }
        cursor->has_next = 1U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_chunk_map_for_chunk(
    SeqParseContext *context,
    SeqTrackDraft *track,
    SeqChunkMapCursor *cursor,
    uint32_t chunk_number,
    uint32_t *samples_per_chunk) {
    AvifdecStatus status;

    while (cursor->has_next != 0U &&
           cursor->next_first_chunk == chunk_number) {
        cursor->current_first_chunk =
            cursor->next_first_chunk;
        cursor->current_samples_per_chunk =
            cursor->next_samples_per_chunk;
        cursor->has_next = 0U;
        if (cursor->entries_remaining != 0U) {
            status = seq_chunk_map_read_entry(
                context, track, cursor,
                &cursor->next_first_chunk,
                &cursor->next_samples_per_chunk);
            if (status != AVIFDEC_OK) return status;
            if (cursor->next_first_chunk <=
                cursor->current_first_chunk) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->stsc.offset, track->stsc.type);
            }
            cursor->has_next = 1U;
        }
    }
    if (cursor->has_next != 0U &&
        cursor->next_first_chunk < chunk_number) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }
    *samples_per_chunk = cursor->current_samples_per_chunk;
    return AVIFDEC_OK;
}

typedef struct {
    AvifdecByteReader reader;
    uint32_t entries_remaining;
    uint32_t next_sample_number;
    uint32_t previous_sample_number;
    uint8_t all_sync;
} SeqSyncCursor;

static AvifdecStatus seq_sync_init(
    SeqParseContext *context,
    SeqTrackDraft *track,
    size_t sample_count,
    SeqSyncCursor *cursor) {
    uint32_t entry_count;

    avifdec_memory_fill(cursor, 0U, sizeof(*cursor));
    if (!seq_box_set(&track->stss)) {
        cursor->all_sync = 1U;
        return AVIFDEC_OK;
    }
    avifdec_byte_reader_init(
        &cursor->reader,
        context->data + track->stss.payload_offset,
        track->stss.payload_size, track->stss.payload_offset);
    if (avifdec_byte_reader_u32be(&cursor->reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stss.offset, track->stss.type);
    }
    entry_count = avifdec_byte_reader_u32be(&cursor->reader);
    if ((sample_count != 0U && entry_count == 0U) ||
        (size_t)entry_count > sample_count) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stss.offset, track->stss.type);
    }
    cursor->entries_remaining = entry_count;
    if (entry_count != 0U) {
        cursor->next_sample_number =
            avifdec_byte_reader_u32be(&cursor->reader);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_sync_for_sample(
    SeqParseContext *context,
    SeqTrackDraft *track,
    SeqSyncCursor *cursor,
    size_t sample_index,
    int *is_sync) {
    uint32_t sample_number = (uint32_t)sample_index + 1U;

    if (cursor->all_sync != 0U) {
        *is_sync = 1;
        return AVIFDEC_OK;
    }
    if (cursor->reader.status != AVIFDEC_OK ||
        (cursor->entries_remaining != 0U &&
         (cursor->next_sample_number == 0U ||
          cursor->next_sample_number <=
              cursor->previous_sample_number))) {
        return seq_fail(
            context,
            cursor->reader.status != AVIFDEC_OK
                ? cursor->reader.status : AVIFDEC_INVALID_DATA,
            track->stss.offset, track->stss.type);
    }
    *is_sync =
        cursor->entries_remaining != 0U &&
        cursor->next_sample_number == sample_number;
    if (*is_sync != 0) {
        cursor->previous_sample_number =
            cursor->next_sample_number;
        --cursor->entries_remaining;
        if (cursor->entries_remaining != 0U) {
            cursor->next_sample_number =
                avifdec_byte_reader_u32be(&cursor->reader);
        }
    } else if (cursor->entries_remaining != 0U &&
               cursor->next_sample_number < sample_number) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stss.offset, track->stss.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_classic_samples(
    SeqParseContext *context,
    SeqTrackDraft *track) {
    const AvifBmffChild *offset_box;
    AvifdecByteReader offsets;
    SeqTimingCursor timing;
    SeqChunkMapCursor chunk_map;
    SeqSyncCursor sync;
    size_t sample_count;
    size_t sample_index = 0U;
    uint32_t chunk_count;
    uint32_t chunk;
    int offsets_64;
    AvifdecStatus status;

    status = seq_sample_count_and_sizes(
        context, track, &sample_count);
    if (status != AVIFDEC_OK) return status;
    track->classic_sample_count = sample_count;
    status = seq_timing_init(
        context, track, sample_count, &timing);
    if (status != AVIFDEC_OK) return status;
    status = seq_chunk_map_init(
        context, track, sample_count, &chunk_map);
    if (status != AVIFDEC_OK) return status;
    if (seq_box_set(&track->stco) ==
        seq_box_set(&track->co64)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->stbl.offset, SEQ_FOURCC('s', 't', 'c', 'o'));
    }
    offset_box = seq_box_set(&track->stco)
        ? &track->stco : &track->co64;
    offsets_64 =
        offset_box->type == SEQ_FOURCC('c', 'o', '6', '4');
    avifdec_byte_reader_init(
        &offsets, context->data + offset_box->payload_offset,
        offset_box->payload_size, offset_box->payload_offset);
    if (avifdec_byte_reader_u32be(&offsets) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            offset_box->offset, offset_box->type);
    }
    chunk_count = avifdec_byte_reader_u32be(&offsets);
    if ((sample_count == 0U && chunk_count != 0U) ||
        (sample_count != 0U && chunk_count == 0U) ||
        (size_t)chunk_count > sample_count) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            offset_box->offset, offset_box->type);
    }
    status = seq_sync_init(
        context, track, sample_count, &sync);
    if (status != AVIFDEC_OK) return status;
    for (chunk = 1U; chunk <= chunk_count; ++chunk) {
        uint64_t offset = offsets_64
            ? avifdec_byte_reader_u64be(&offsets)
            : avifdec_byte_reader_u32be(&offsets);
        uint32_t samples_per_chunk;
        uint32_t in_chunk;

        status = seq_chunk_map_for_chunk(
            context, track, &chunk_map, chunk,
            &samples_per_chunk);
        if (status != AVIFDEC_OK) return status;
        for (in_chunk = 0U; in_chunk < samples_per_chunk;
             ++in_chunk) {
            uint32_t sample_size;
            uint64_t dts;
            uint64_t duration;
            int is_sync;

            if (sample_index >= sample_count ||
                !seq_sample_size_at(
                    context, track, sample_index, &sample_size)) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    offset_box->offset, offset_box->type);
            }
            status = seq_timing_next(
                context, track, &timing, &dts, &duration);
            if (status != AVIFDEC_OK) return status;
            status = seq_sync_for_sample(
                context, track, &sync, sample_index, &is_sync);
            if (status != AVIFDEC_OK) return status;
            status = seq_emit_sample(
                context, track, offset, sample_size,
                dts, duration, is_sync, SEQ_NO_FRAGMENT,
                offset_box->type);
            if (status != AVIFDEC_OK) return status;
            if ((uint64_t)sample_size > UINT64_MAX - offset) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    offset_box->offset, offset_box->type);
            }
            offset += sample_size;
            ++sample_index;
        }
    }
    if (offsets.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&offsets) != 0U ||
        sample_index != sample_count ||
        chunk_map.entries_remaining != 0U ||
        chunk_map.has_next != 0U ||
        avifdec_byte_reader_remaining(&chunk_map.reader) != 0U ||
        (sync.all_sync == 0U &&
         (sync.entries_remaining != 0U ||
          avifdec_byte_reader_remaining(&sync.reader) != 0U))) {
        return seq_fail(
            context,
            offsets.status != AVIFDEC_OK
                ? offsets.status : AVIFDEC_INVALID_DATA,
            offset_box->offset, offset_box->type);
    }
    status = seq_timing_finish(context, track, &timing);
    if (status != AVIFDEC_OK) return status;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_tref_contains(
    SeqParseContext *context,
    const SeqTrackDraft *track,
    uint32_t relationship_type,
    uint32_t target_id,
    int *found) {
    AvifBmffChildIterator iterator;
    AvifBmffChild relationship;
    int has_child;
    AvifdecStatus status;

    *found = 0;
    if (!seq_box_set(&track->tref)) return AVIFDEC_OK;
    status = seq_child_iterator(
        context, &track->tref, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        size_t position;

        status = seq_next_child(
            context, &iterator, &relationship, &has_child);
        if (status != AVIFDEC_OK || !has_child) return status;
        if (relationship.type != relationship_type ||
            (relationship.payload_size & 3U) != 0U) {
            continue;
        }
        for (position = 0U;
             position < relationship.payload_size;
             position += 4U) {
            if (avifdec_load_u32be(
                    context->data + relationship.payload_offset +
                    position) == target_id) {
                *found = 1;
                return AVIFDEC_OK;
            }
        }
    }
}

static AvifdecStatus seq_parse_references(
    SeqParseContext *context) {
    size_t track_index;

    for (track_index = 0U;
         track_index < context->track_count; ++track_index) {
        SeqTrackDraft *track = &context->tracks[track_index];
        AvifBmffChildIterator iterator;
        AvifBmffChild relationship;
        int has_child;
        AvifdecStatus status;

        if (!seq_box_set(&track->tref)) continue;
        status = seq_child_iterator(
            context, &track->tref, 0U, &iterator);
        if (status != AVIFDEC_OK) return status;
        for (;;) {
            size_t position;

            status = seq_next_child(
                context, &iterator, &relationship, &has_child);
            if (status != AVIFDEC_OK || !has_child) break;
            if (relationship.payload_size == 0U ||
                (relationship.payload_size & 3U) != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    relationship.offset, relationship.type);
            }
            if ((relationship.type ==
                     SEQ_FOURCC('a', 'u', 'x', 'l') ||
                 relationship.type ==
                     SEQ_FOURCC('p', 'r', 'e', 'm')) &&
                relationship.payload_size != 4U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    relationship.offset, relationship.type);
            }
            for (position = 0U;
                 position < relationship.payload_size;
                 position += 4U) {
                SeqReferenceRecord record;
                SeqTrackDraft *target;

                record.from_track_id = track->record.track_id;
                record.to_track_id = avifdec_load_u32be(
                    context->data + relationship.payload_offset +
                    position);
                record.relationship_type = relationship.type;
                target = seq_track_by_id(
                    context, record.to_track_id, 0);
                if (record.to_track_id == 0U ||
                    target == 0 ||
                    record.to_track_id == record.from_track_id) {
                    return seq_fail(
                        context, AVIFDEC_INVALID_DATA,
                        relationship.payload_offset + position,
                        relationship.type);
                }
                if (relationship.type ==
                        SEQ_FOURCC('a', 'u', 'x', 'l') &&
                    (((track->record.flags &
                       AVIF_SEQUENCE_TRACK_AUXILIARY) == 0U) ||
                     ((target->record.flags &
                       AVIF_SEQUENCE_TRACK_VISUAL) == 0U))) {
                    return seq_fail(
                        context, AVIFDEC_INVALID_DATA,
                        relationship.offset, relationship.type);
                }
                if (relationship.type ==
                        SEQ_FOURCC('p', 'r', 'e', 'm') &&
                    (((track->record.flags &
                       AVIF_SEQUENCE_TRACK_VISUAL) == 0U) ||
                     ((target->record.flags &
                       AVIF_SEQUENCE_TRACK_ALPHA) == 0U))) {
                    return seq_fail(
                        context, AVIFDEC_INVALID_DATA,
                        relationship.offset, relationship.type);
                }
                if (relationship.type ==
                    SEQ_FOURCC('p', 'r', 'e', 'm')) {
                    int reciprocal;

                    status = seq_tref_contains(
                        context, target,
                        SEQ_FOURCC('a', 'u', 'x', 'l'),
                        track->record.track_id, &reciprocal);
                    if (status != AVIFDEC_OK) return status;
                    if (!reciprocal) {
                        return seq_fail(
                            context, AVIFDEC_INVALID_DATA,
                            relationship.offset, relationship.type);
                    }
                }
                status = seq_property_seen(
                    context, relationship.offset,
                    relationship.type);
                if (status != AVIFDEC_OK) return status;
                status = seq_writer_reference(context, &record);
                if (status != AVIFDEC_OK) return status;
                if (relationship.type ==
                    SEQ_FOURCC('a', 'l', 't', 'r')) {
                    track->record.flags |=
                        AVIF_SEQUENCE_TRACK_ALTERNATE;
                    target->record.flags |=
                        AVIF_SEQUENCE_TRACK_ALTERNATE;
                }
            }
        }
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

static SeqTrexRecord *seq_trex_by_id(
    SeqParseContext *context,
    uint32_t track_id) {
    size_t index;

    for (index = 0U; index < context->trex_count; ++index) {
        if (context->trex[index].track_id == track_id) {
            return &context->trex[index];
        }
    }
    return 0;
}

static AvifdecStatus seq_validate_sample_flags(
    SeqParseContext *context,
    uint32_t flags,
    int av1_track,
    size_t offset,
    uint32_t source_type) {
    uint32_t is_leading = (flags >> 26U) & 3U;
    uint32_t depends_on = (flags >> 24U) & 3U;
    uint32_t is_depended_on = (flags >> 22U) & 3U;
    uint32_t has_redundancy = (flags >> 20U) & 3U;

    if ((flags & UINT32_C(0xf0000000)) != 0U ||
        depends_on == 3U || is_depended_on == 3U ||
        has_redundancy == 3U ||
        (av1_track != 0 &&
         (is_leading == 1U || is_leading == 3U))) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, offset, source_type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_trex(
    SeqParseContext *context,
    const AvifBmffChild *box) {
    AvifdecByteReader reader;
    uint32_t track_id;
    uint32_t description_index;
    uint32_t duration;
    uint32_t size;
    uint32_t flags;
    SeqTrackDraft *track;
    SeqTrexRecord *record;
    AvifdecStatus status;

    avifdec_byte_reader_init(
        &reader, context->data + box->payload_offset,
        box->payload_size, box->payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    track_id = avifdec_byte_reader_u32be(&reader);
    description_index = avifdec_byte_reader_u32be(&reader);
    duration = avifdec_byte_reader_u32be(&reader);
    size = avifdec_byte_reader_u32be(&reader);
    flags = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        track_id == 0U || description_index == 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            box->offset, box->type);
    }
    if (!seq_physical_track_exists(context, track_id)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    if (seq_trex_by_id(context, track_id) != 0 ||
        context->trex_count >= SEQ_MAX_TRACKS ||
        context->trex_count >= context->limits.max_tracks) {
        return seq_fail(
            context,
            seq_trex_by_id(context, track_id) != 0
                ? AVIFDEC_INVALID_DATA : AVIFDEC_LIMIT_EXCEEDED,
            box->offset, box->type);
    }
    track = seq_track_by_id(context, track_id, 0);
    status = seq_validate_sample_flags(
        context, flags, track != 0, box->offset, box->type);
    if (status != AVIFDEC_OK) return status;
    record = &context->trex[context->trex_count++];
    record->track_id = track_id;
    record->description_index = description_index;
    record->default_duration = duration;
    record->default_size = size;
    record->default_flags = flags;
    if (track == 0) {
        return seq_property_seen(context, box->offset, box->type);
    }
    if (track->has_trex != 0U || description_index != 1U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    track->has_trex = 1U;
    track->trex_description_index = description_index;
    track->trex_default_duration = duration;
    track->trex_default_size = size;
    track->trex_default_flags = flags;
    return seq_property_seen(context, box->offset, box->type);
}

static AvifdecStatus seq_parse_mvex(
    SeqParseContext *context) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    AvifdecStatus status;

    if (!seq_box_set(&context->mvex)) return AVIFDEC_OK;
    status = seq_child_iterator(
        context, &context->mvex, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('t', 'r', 'e', 'x')) {
            status = seq_parse_trex(context, &box);
            if (status != AVIFDEC_OK) return status;
        }
    }
    return status;
}

static AvifdecStatus seq_parse_initial_tracks(
    SeqParseContext *context) {
    size_t track_index;
    AvifdecStatus status;

    for (track_index = 0U;
         track_index < context->track_count; ++track_index) {
        SeqTrackDraft *track = &context->tracks[track_index];

        status = seq_parse_edits(context, track);
        if (status != AVIFDEC_OK) return status;
        status = seq_parse_classic_samples(context, track);
        if (status != AVIFDEC_OK) return status;
    }
    return AVIFDEC_OK;
}

typedef struct {
    uint64_t base_data_offset;
    uint32_t default_duration;
    uint32_t default_size;
    uint32_t default_flags;
    uint32_t track_id;
    uint32_t description_index;
    uint32_t flags;
    uint8_t duration_is_empty;
} SeqTfhd;

static AvifdecStatus seq_parse_tfhd(
    SeqParseContext *context,
    const AvifBmffChild *box,
    uint64_t moof_offset,
    uint64_t implicit_base,
    SeqTfhd *tfhd) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t flags;
    SeqTrackDraft *track;
    SeqTrexRecord *trex;
    AvifdecStatus status;

    avifdec_memory_fill(tfhd, 0U, sizeof(*tfhd));
    avifdec_byte_reader_init(
        &reader, context->data + box->payload_offset,
        box->payload_size, box->payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    flags = (uint32_t)avifdec_byte_reader_u8(&reader) << 16U;
    flags |= (uint32_t)avifdec_byte_reader_u8(&reader) << 8U;
    flags |= avifdec_byte_reader_u8(&reader);
    if (version != 0U || (flags & ~UINT32_C(0x03003b)) != 0U ||
        ((flags & UINT32_C(0x000001)) != 0U &&
         (flags & UINT32_C(0x020000)) != 0U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    tfhd->track_id = avifdec_byte_reader_u32be(&reader);
    if (!seq_physical_track_exists(context, tfhd->track_id)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    track = seq_track_by_id(context, tfhd->track_id, 0);
    trex = seq_trex_by_id(context, tfhd->track_id);
    if (trex == 0 || (track != 0 && track->has_trex == 0U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    tfhd->description_index = trex->description_index;
    tfhd->default_duration = trex->default_duration;
    tfhd->default_size = trex->default_size;
    tfhd->default_flags = trex->default_flags;
    if ((flags & UINT32_C(0x000001)) != 0U) {
        tfhd->base_data_offset =
            avifdec_byte_reader_u64be(&reader);
    } else if ((flags & UINT32_C(0x020000)) != 0U) {
        tfhd->base_data_offset = moof_offset;
    } else {
        tfhd->base_data_offset = implicit_base;
    }
    if ((flags & UINT32_C(0x000002)) != 0U) {
        tfhd->description_index =
            avifdec_byte_reader_u32be(&reader);
    }
    if ((flags & UINT32_C(0x000008)) != 0U) {
        tfhd->default_duration =
            avifdec_byte_reader_u32be(&reader);
    }
    if ((flags & UINT32_C(0x000010)) != 0U) {
        tfhd->default_size =
            avifdec_byte_reader_u32be(&reader);
    }
    if ((flags & UINT32_C(0x000020)) != 0U) {
        tfhd->default_flags =
            avifdec_byte_reader_u32be(&reader);
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        tfhd->track_id == 0U ||
        tfhd->description_index == 0U ||
        (track != 0 && tfhd->description_index != 1U)) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            box->offset, box->type);
    }
    tfhd->flags = flags;
    tfhd->duration_is_empty =
        (uint8_t)((flags & UINT32_C(0x010000)) != 0U);
    status = seq_validate_sample_flags(
        context, tfhd->default_flags, track != 0,
        box->offset, box->type);
    if (status != AVIFDEC_OK) return status;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_parse_tfdt(
    SeqParseContext *context,
    const AvifBmffChild *box,
    uint64_t *decode_time) {
    AvifdecByteReader reader;
    uint8_t version;

    avifdec_byte_reader_init(
        &reader, context->data + box->payload_offset,
        box->payload_size, box->payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    if (avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        avifdec_byte_reader_u8(&reader) != 0U ||
        (version != 0U && version != 1U)) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    *decode_time = version == 1U
        ? avifdec_byte_reader_u64be(&reader)
        : avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U) {
        return seq_fail(
            context,
            reader.status != AVIFDEC_OK
                ? reader.status : AVIFDEC_INVALID_DATA,
            box->offset, box->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_trun_count(
    SeqParseContext *context,
    const AvifBmffChild *box,
    size_t *sample_count) {
    const unsigned char *payload =
        context->data + box->payload_offset;
    uint8_t version;
    uint32_t flags;
    uint32_t count;

    if (box->payload_size < 8U) {
        return seq_fail(
            context, AVIFDEC_TRUNCATED, box->offset, box->type);
    }
    version = payload[0];
    flags = (uint32_t)payload[1] << 16U |
            (uint32_t)payload[2] << 8U |
            (uint32_t)payload[3];
    count = avifdec_load_u32be(payload + 4U);
    if ((version != 0U && version != 1U) ||
        (flags & ~UINT32_C(0x000f05)) != 0U ||
        ((flags & UINT32_C(0x000004)) != 0U &&
         (flags & UINT32_C(0x000400)) != 0U) ||
        count == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    if (!seq_add_count(sample_count, count)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW, box->offset, box->type);
    }
    return AVIFDEC_OK;
}

static int seq_sample_flags_sync(uint32_t flags) {
    uint32_t depends_on = (flags >> 24U) & 3U;

    return (flags & UINT32_C(0x00010000)) == 0U &&
           depends_on != 1U;
}

static AvifdecStatus seq_process_trun(
    SeqParseContext *context,
    SeqTrackDraft *track,
    const SeqTfhd *tfhd,
    const AvifBmffChild *box,
    size_t fragment_index,
    uint64_t *data_cursor,
    uint64_t *dts) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t flags;
    uint32_t sample_count;
    int32_t data_offset = 0;
    uint32_t first_sample_flags = 0U;
    uint32_t sample;

    avifdec_byte_reader_init(
        &reader, context->data + box->payload_offset,
        box->payload_size, box->payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    flags = (uint32_t)avifdec_byte_reader_u8(&reader) << 16U;
    flags |= (uint32_t)avifdec_byte_reader_u8(&reader) << 8U;
    flags |= avifdec_byte_reader_u8(&reader);
    sample_count = avifdec_byte_reader_u32be(&reader);
    if ((version != 0U && version != 1U) ||
        (flags & ~UINT32_C(0x000f05)) != 0U ||
        (track != 0 &&
         (flags & UINT32_C(0x000800)) != 0U) ||
        ((flags & UINT32_C(0x000004)) != 0U &&
         (flags & UINT32_C(0x000400)) != 0U) ||
        sample_count == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA, box->offset, box->type);
    }
    if ((flags & UINT32_C(0x000001)) != 0U) {
        data_offset =
            seq_u32_to_i32(
                avifdec_byte_reader_u32be(&reader));
        if (!seq_u64_add_signed(
                tfhd->base_data_offset,
                data_offset, data_cursor)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                box->offset, box->type);
        }
    }
    if ((flags & UINT32_C(0x000004)) != 0U) {
        first_sample_flags =
            avifdec_byte_reader_u32be(&reader);
    }
    for (sample = 0U; sample < sample_count; ++sample) {
        uint32_t duration = tfhd->default_duration;
        uint32_t size = tfhd->default_size;
        uint32_t sample_flags = tfhd->default_flags;
        AvifdecStatus status;

        if ((flags & UINT32_C(0x000100)) != 0U) {
            duration = avifdec_byte_reader_u32be(&reader);
        }
        if ((flags & UINT32_C(0x000200)) != 0U) {
            size = avifdec_byte_reader_u32be(&reader);
        }
        if ((flags & UINT32_C(0x000400)) != 0U) {
            sample_flags = avifdec_byte_reader_u32be(&reader);
        } else if (sample == 0U &&
                   (flags & UINT32_C(0x000004)) != 0U) {
            sample_flags = first_sample_flags;
        }
        if ((flags & UINT32_C(0x000800)) != 0U) {
            (void)avifdec_byte_reader_u32be(&reader);
        }
        if (reader.status != AVIFDEC_OK ||
            duration == 0U || size == 0U) {
            return seq_fail(
                context,
                reader.status != AVIFDEC_OK
                    ? reader.status : AVIFDEC_INVALID_DATA,
                box->offset, box->type);
        }
        status = seq_validate_sample_flags(
            context, sample_flags, track != 0,
            box->offset, box->type);
        if (status != AVIFDEC_OK) return status;
        if (track != 0) {
            status = seq_emit_sample(
                context, track, *data_cursor, size, *dts,
                duration, seq_sample_flags_sync(sample_flags),
                fragment_index, box->type);
        } else {
            status = seq_range_in_mdat(
                context, *data_cursor, size, box->type);
        }
        if (status != AVIFDEC_OK) return status;
        if ((uint64_t)size > UINT64_MAX - *data_cursor ||
            (uint64_t)duration > UINT64_MAX - *dts) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                box->offset, box->type);
        }
        *data_cursor += size;
        *dts += duration;
    }
    if (avifdec_byte_reader_remaining(&reader) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            avifdec_byte_reader_offset(&reader), box->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_process_traf(
    SeqParseContext *context,
    const AvifBmffChild *moof,
    const AvifBmffChild *traf,
    uint32_t sequence_number,
    uint64_t *implicit_base) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    AvifBmffChild tfhd_box;
    AvifBmffChild tfdt_box;
    SeqTfhd tfhd;
    SeqTrackDraft *track;
    SeqFragmentRecord fragment;
    uint64_t dts;
    uint64_t data_cursor;
    size_t traf_sample_count = 0U;
    size_t fragment_index;
    int has_child;
    AvifdecStatus status;

    avifdec_memory_fill(&tfhd_box, 0U, sizeof(tfhd_box));
    avifdec_memory_fill(&tfdt_box, 0U, sizeof(tfdt_box));
    status = seq_child_iterator(context, traf, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('t', 'f', 'h', 'd')) {
            status = seq_unique_box(context, &tfhd_box, &box);
        } else if (box.type == SEQ_FOURCC('t', 'f', 'd', 't')) {
            status = seq_unique_box(context, &tfdt_box, &box);
        } else if (box.type == SEQ_FOURCC('t', 'r', 'u', 'n')) {
            status = seq_trun_count(
                context, &box, &traf_sample_count);
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (status != AVIFDEC_OK) return status;
    if (!seq_box_set(&tfhd_box) ||
        !seq_box_set(&tfdt_box) ||
        traf_sample_count == 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            traf->offset, traf->type);
    }
    if (traf_sample_count > context->limits.max_frames) {
        return seq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED,
            traf->offset, traf->type);
    }
    status = seq_parse_tfhd(
        context, &tfhd_box, moof->offset,
        *implicit_base, &tfhd);
    if (status != AVIFDEC_OK) return status;
    if (tfhd.duration_is_empty != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            tfhd_box.offset, tfhd_box.type);
    }
    track = seq_track_by_id(context, tfhd.track_id, 0);
    status = seq_parse_tfdt(context, &tfdt_box, &dts);
    if (status != AVIFDEC_OK) return status;
    fragment_index = SEQ_NO_FRAGMENT;
    if (track != 0) {
        if (dts != track->expected_dts ||
            traf_sample_count >
                context->limits.max_frames -
                track->next_sample_index) {
            return seq_fail(
                context,
                dts != track->expected_dts
                    ? AVIFDEC_INVALID_DATA
                    : AVIFDEC_LIMIT_EXCEEDED,
                tfdt_box.offset, tfdt_box.type);
        }
        fragment_index = context->fragment_count;
        avifdec_memory_fill(&fragment, 0U, sizeof(fragment));
        fragment.decode_time = dts;
        fragment.first_sample_index = track->next_sample_index;
        fragment.sample_count = traf_sample_count;
        fragment.moof_offset = moof->offset;
        fragment.track_id = track->record.track_id;
        fragment.sequence_number = sequence_number;
        status = seq_writer_fragment(context, &fragment);
        if (status != AVIFDEC_OK) return status;
        if (!seq_add_count(&track->record.fragment_count, 1U)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                traf->offset, traf->type);
        }
        track->record.flags |= AVIF_SEQUENCE_TRACK_FRAGMENTED;
    }
    data_cursor = tfhd.base_data_offset;
    status = seq_child_iterator(context, traf, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('t', 'r', 'u', 'n')) {
            status = seq_process_trun(
                context, track, &tfhd, &box,
                fragment_index, &data_cursor, &dts);
            if (status != AVIFDEC_OK) return status;
        }
    }
    if (status != AVIFDEC_OK) return status;
    if (track != 0) track->expected_dts = dts;
    *implicit_base = data_cursor;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_process_moof(
    SeqParseContext *context,
    const AvifBmffChild *moof) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    AvifBmffChild mfhd;
    uint32_t sequence_number;
    uint64_t implicit_base = moof->offset;
    size_t traf_count = 0U;
    int has_child;
    AvifdecStatus status;

    avifdec_memory_fill(&mfhd, 0U, sizeof(mfhd));
    status = seq_child_iterator(context, moof, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('m', 'f', 'h', 'd')) {
            status = seq_unique_box(context, &mfhd, &box);
        } else if (box.type == SEQ_FOURCC('t', 'r', 'a', 'f')) {
            if (!seq_add_count(&traf_count, 1U)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    box.offset, box.type);
            }
        } else {
            status = AVIFDEC_OK;
        }
        if (status != AVIFDEC_OK) return status;
    }
    if (status != AVIFDEC_OK) return status;
    if (!seq_box_set(&mfhd) || traf_count == 0U ||
        mfhd.payload_size != 8U ||
        avifdec_load_u32be(
            context->data + mfhd.payload_offset) != 0U) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            moof->offset, moof->type);
    }
    sequence_number = avifdec_load_u32be(
        context->data + mfhd.payload_offset + 4U);
    if (context->have_previous_mfhd != 0U &&
        sequence_number <= context->previous_mfhd_sequence) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            mfhd.offset, mfhd.type);
    }
    context->previous_mfhd_sequence = sequence_number;
    context->have_previous_mfhd = 1U;
    status = seq_child_iterator(context, moof, 0U, &iterator);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('t', 'r', 'a', 'f')) {
            status = seq_process_traf(
                context, moof, &box, sequence_number,
                &implicit_base);
            if (status != AVIFDEC_OK) return status;
        }
    }
    return status;
}

static AvifdecStatus seq_parse_fragments(
    SeqParseContext *context) {
    AvifBmffChildIterator iterator;
    AvifBmffChild box;
    int has_child;
    int saw_moof = 0;
    AvifdecStatus status = avif_bmff_child_iterator_init(
        &iterator, context->data, context->size, 0U,
        context->size, 0U, context->error);

    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = seq_next_child(
            context, &iterator, &box, &has_child);
        if (status != AVIFDEC_OK || !has_child) break;
        if (box.type == SEQ_FOURCC('m', 'o', 'o', 'f')) {
            if (!seq_box_set(&context->mvex)) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    box.offset, box.type);
            }
            saw_moof = 1;
            status = seq_process_moof(context, &box);
            if (status != AVIFDEC_OK) return status;
        }
    }
    if (status != AVIFDEC_OK) return status;
    if (saw_moof == 0 && seq_box_set(&context->mvex)) {
        size_t track_index;

        for (track_index = 0U;
             track_index < context->track_count; ++track_index) {
            if (context->tracks[track_index].classic_sample_count == 0U &&
                context->tracks[track_index].has_trex != 0U) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    context->mvex.offset, context->mvex.type);
            }
        }
    }
    return AVIFDEC_OK;
}

static size_t seq_context_track_order(
    const SeqParseContext *context,
    uint32_t track_id) {
    size_t index;

    for (index = 0U; index < context->track_count; ++index) {
        if (context->tracks[index].record.track_id == track_id) {
            return index;
        }
    }
    return context->track_count;
}

static int seq_presentation_after(
    const SeqParseContext *context,
    const SeqPresentationRecord *left,
    const SeqPresentationRecord *right) {
    size_t left_track =
        seq_context_track_order(context, left->track_id);
    size_t right_track =
        seq_context_track_order(context, right->track_id);

    if (left_track != right_track) return left_track > right_track;
    if (left->start_time != right->start_time) {
        return left->start_time > right->start_time;
    }
    return left->serial > right->serial;
}

static void seq_sort_presentations(SeqParseContext *context) {
    SeqWriter *writer = context->writer;
    size_t index;

    if (writer == 0) return;
    for (index = 1U; index < writer->presentation_count; ++index) {
        SeqPresentationRecord value;
        size_t position = index;

        seq_load(
            writer->workspace,
            writer->layout->presentations_offset +
                index * sizeof(value),
            &value, sizeof(value));
        while (position != 0U) {
            SeqPresentationRecord previous;

            seq_load(
                writer->workspace,
                writer->layout->presentations_offset +
                    (position - 1U) * sizeof(previous),
                &previous, sizeof(previous));
            if (!seq_presentation_after(
                    context, &previous, &value)) {
                break;
            }
            seq_store(
                writer->workspace,
                writer->layout->presentations_offset +
                    position * sizeof(previous),
                &previous, sizeof(previous));
            --position;
        }
        seq_store(
            writer->workspace,
            writer->layout->presentations_offset +
                position * sizeof(value),
            &value, sizeof(value));
    }
}

static AvifdecStatus seq_validate_presentation_order(
    SeqParseContext *context) {
    size_t track_index;

    if (context->writer == 0) return AVIFDEC_OK;
    for (track_index = 0U;
         track_index < context->track_count; ++track_index) {
        SeqTrackDraft *track = &context->tracks[track_index];
        uint64_t previous_end = 0U;
        size_t index;
        int have_previous = 0;

        for (index = 0U;
             index < context->writer->presentation_count; ++index) {
            SeqPresentationRecord presentation;

            seq_load(
                context->writer->workspace,
                context->writer->layout->presentations_offset +
                    index * sizeof(presentation),
                &presentation, sizeof(presentation));
            if (presentation.track_id !=
                track->record.track_id) {
                continue;
            }
            if (presentation.duration >
                UINT64_MAX - presentation.start_time) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW, 0U,
                    SEQ_FOURCC('e', 'l', 's', 't'));
            }
            if (have_previous != 0 &&
                presentation.start_time < previous_end) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->elst.offset,
                    SEQ_FOURCC('e', 'l', 's', 't'));
            }
            previous_end =
                presentation.start_time + presentation.duration;
            have_previous = 1;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_validate_edit_coverage(
    SeqParseContext *context,
    const SeqTrackDraft *track) {
    uint64_t media_end;
    size_t edit_index;

    if (!seq_scale_exact(
            track->expected_dts,
            context->presentation_timescale,
            track->record.media_timescale, &media_end)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            track->mdhd.offset, track->mdhd.type);
    }
    for (edit_index = 0U;
         edit_index < track->edit_entry_count; ++edit_index) {
        SeqEditValue edit;
        uint64_t start;
        uint64_t duration;

        if (!seq_read_edit(context, track, edit_index, &edit)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                track->elst.offset, track->elst.type);
        }
        if (edit.media_time == -1) continue;
        if (!seq_scale_exact(
                (uint64_t)edit.media_time,
                context->presentation_timescale,
                track->record.media_timescale, &start) ||
            !seq_scale_exact(
                edit.segment_duration,
                context->presentation_timescale,
                context->movie_timescale, &duration)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                track->elst.offset, track->elst.type);
        }
        if (duration > UINT64_MAX - start ||
            start + duration > media_end) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->elst.offset, track->elst.type);
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_validate_tkhd_duration(
    SeqParseContext *context,
    const SeqTrackDraft *track) {
    uint64_t sentinel =
        track->tkhd_version == 1U
            ? UINT64_MAX : UINT32_MAX;
    uint64_t expected;
    int fragmented =
        track->has_trex != 0U ||
        (track->record.flags &
         AVIF_SEQUENCE_TRACK_FRAGMENTED) != 0U;

    if (track->tkhd_duration == sentinel) {
        if (fragmented || track->edit_entry_count != 0U) {
            return AVIFDEC_OK;
        }
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->tkhd.offset, track->tkhd.type);
    }
    if (track->edit_entry_count != 0U) {
        if (!seq_scale_exact(
                track->record.timeline_duration,
                context->movie_timescale,
                context->presentation_timescale, &expected)) {
            return seq_fail(
                context, AVIFDEC_OVERFLOW,
                track->tkhd.offset, track->tkhd.type);
        }
    } else if (!seq_scale_ceil(
                   track->expected_dts,
                   context->movie_timescale,
                   track->record.media_timescale, &expected)) {
        return seq_fail(
            context, AVIFDEC_OVERFLOW,
            track->tkhd.offset, track->tkhd.type);
    }
    if ((track->tkhd_version == 0U && expected > UINT32_MAX) ||
        track->tkhd_duration != expected) {
        return seq_fail(
            context, AVIFDEC_INVALID_DATA,
            track->tkhd.offset, track->tkhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus seq_finalize_tracks(
    SeqParseContext *context) {
    size_t track_index;

    for (track_index = 0U;
         track_index < context->track_count; ++track_index) {
        SeqTrackDraft *track = &context->tracks[track_index];
        int fragmented =
            (track->record.flags &
             AVIF_SEQUENCE_TRACK_FRAGMENTED) != 0U;
        int duration_unspecified =
            fragmented != 0 &&
            (track->record.media_duration == 0U ||
             track->record.media_duration == UINT32_MAX ||
             track->record.media_duration == UINT64_MAX);
        int movie_duration_unspecified =
            context->movie_duration == UINT32_MAX ||
            context->movie_duration == UINT64_MAX ||
            (fragmented != 0 && context->movie_duration == 0U);
        uint64_t normalized_movie_duration = 0U;
        AvifdecStatus status =
            seq_validate_edit_coverage(context, track);

        if (status != AVIFDEC_OK) return status;
        status = seq_validate_tkhd_duration(context, track);
        if (status != AVIFDEC_OK) return status;
        if (track->record.sample_count == 0U ||
            track->record.presentation_count == 0U ||
            track->has_canonical_header == 0U) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->trak.offset, track->trak.type);
        }
        if (!duration_unspecified &&
            track->record.media_duration !=
                track->expected_dts) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->mdhd.offset, track->mdhd.type);
        }
        if (duration_unspecified) {
            track->record.media_duration = track->expected_dts;
        }
        if (!movie_duration_unspecified) {
            if (!seq_scale_exact(
                    context->movie_duration,
                    context->presentation_timescale,
                    context->movie_timescale,
                    &normalized_movie_duration)) {
                return seq_fail(
                    context, AVIFDEC_OVERFLOW,
                    context->mvhd.offset, context->mvhd.type);
            }
            if (track->record.timeline_duration >
                normalized_movie_duration) {
                return seq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    context->mvhd.offset, context->mvhd.type);
            }
        }
        if ((track->record.flags &
             AVIF_SEQUENCE_TRACK_ALPHA) != 0U &&
            (track->record.monochrome == 0U ||
             track->canonical_validation.monochrome == 0U ||
             track->canonical_validation.color_range != 1U)) {
            return seq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->stsd.offset, track->stsd.type);
        }
        if (context->writer != 0) {
            seq_store(
                context->writer->workspace,
                context->writer->layout->tracks_offset +
                    track_index * sizeof(track->record),
                &track->record, sizeof(track->record));
        }
    }
    seq_sort_presentations(context);
    return seq_validate_presentation_order(context);
}

static AvifdecStatus seq_parse_all(
    SeqParseContext *context) {
    AvifdecStatus status = seq_collect_top_level(context);

    if (status != AVIFDEC_OK) return status;
    status = seq_parse_mvhd(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_parse_tracks(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_choose_presentation_timescale(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_parse_references(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_parse_mvex(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_parse_initial_tracks(context);
    if (status != AVIFDEC_OK) return status;
    status = seq_parse_fragments(context);
    if (status != AVIFDEC_OK) return status;
    return seq_finalize_tracks(context);
}

static AvifdecStatus seq_run_parse(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifSequenceValidationCallbacks *validation,
    SeqWriter *writer,
    SeqParseContext *context,
    AvifdecError *error) {
    if ((data == 0 && size != 0U) ||
        validation == 0 ||
        validation->validate_header == 0) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    if (size == 0U) {
        return seq_query_fail(
            AVIFDEC_TRUNCATED, 0U, 0U, error);
    }
    avifdec_memory_fill(context, 0U, sizeof(*context));
    context->data = (const unsigned char *)data;
    context->size = size;
    context->limits = seq_effective_limits(limits);
    context->validation = validation;
    context->error = error;
    context->writer = writer;
    return seq_parse_all(context);
}

AvifdecStatus avif_sequence_index_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifSequenceValidationCallbacks *validation,
    AvifSequenceIndexInfo *info,
    AvifdecError *error) {
    SeqParseContext context;
    SeqWorkspaceHeader layout;
    AvifdecStatus status;

    seq_error_clear(error);
    if (info == 0) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = seq_run_parse(
        data, size, limits, validation, 0, &context, error);
    if (status != AVIFDEC_OK) return status;
    if (!seq_layout_build(
            context.track_count, context.reference_count,
            context.sample_count, context.fragment_count,
            context.edit_count, context.presentation_count,
            &layout)) {
        return seq_query_fail(
            AVIFDEC_OVERFLOW, 0U, 0U, error);
    }
    info->workspace_required = layout.layout_size;
    info->track_count = context.track_count;
    info->track_reference_count = context.reference_count;
    info->sample_count = context.sample_count;
    info->fragment_count = context.fragment_count;
    info->edit_count = context.edit_count;
    info->presentation_count = context.presentation_count;
    info->movie_timescale = context.movie_timescale;
    info->movie_duration = context.movie_duration;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_index_init(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifSequenceValidationCallbacks *validation,
    void *workspace,
    size_t workspace_size,
    AvifSequenceIndex *index,
    AvifSequenceIndexInfo *info,
    AvifdecError *error) {
    AvifSequenceIndexInfo queried;
    SeqWorkspaceHeader layout;
    SeqWorkspaceHeader previous;
    SeqWriter writer;
    SeqParseContext context;
    size_t generation = 1U;
    uint64_t hash;
    AvifdecStatus status;

    seq_error_clear(error);
    if (index == 0 || info == 0 ||
        (workspace == 0 && workspace_size != 0U)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    avifdec_memory_fill(index, 0U, sizeof(*index));
    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = avif_sequence_index_query(
        data, size, limits, validation, &queried, error);
    *info = queried;
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < queried.workspace_required ||
        workspace == 0) {
        return seq_query_fail(
            AVIFDEC_OUT_OF_MEMORY, 0U, 0U, error);
    }
    if (workspace_size >= sizeof(previous)) {
        seq_load(
            (const unsigned char *)workspace, 0U,
            &previous, sizeof(previous));
        if (previous.magic == SEQ_WORKSPACE_MAGIC &&
            previous.version == SEQ_WORKSPACE_VERSION &&
            previous.layout_size <= workspace_size &&
            previous.generation != SIZE_MAX) {
            generation = previous.generation + 1U;
        }
    }
    if (!seq_layout_build(
            queried.track_count,
            queried.track_reference_count,
            queried.sample_count, queried.fragment_count,
            queried.edit_count, queried.presentation_count,
            &layout)) {
        return seq_query_fail(
            AVIFDEC_OVERFLOW, 0U, 0U, error);
    }
    avifdec_memory_fill(
        workspace, 0U, queried.workspace_required);
    writer.workspace = (unsigned char *)workspace;
    writer.layout = &layout;
    writer.reference_count = 0U;
    writer.sample_count = 0U;
    writer.fragment_count = 0U;
    writer.edit_count = 0U;
    writer.presentation_count = 0U;
    seq_error_clear(error);
    status = seq_run_parse(
        data, size, limits, validation,
        &writer, &context, error);
    if (status != AVIFDEC_OK) {
        avifdec_memory_fill(index, 0U, sizeof(*index));
        avifdec_memory_fill(
            workspace, 0U, queried.workspace_required);
        return status;
    }
    if (writer.reference_count != layout.reference_count ||
        writer.sample_count != layout.sample_count ||
        writer.fragment_count != layout.fragment_count ||
        writer.edit_count != layout.edit_count ||
        writer.presentation_count != layout.presentation_count ||
        context.movie_timescale != queried.movie_timescale ||
        context.movie_duration != queried.movie_duration) {
        avifdec_memory_fill(index, 0U, sizeof(*index));
        avifdec_memory_fill(
            workspace, 0U, queried.workspace_required);
        return seq_query_fail(
            AVIFDEC_INVALID_DATA, 0U, 0U, error);
    }
    layout.magic = SEQ_WORKSPACE_MAGIC;
    layout.version = SEQ_WORKSPACE_VERSION;
    layout.generation = generation;
    layout.movie_timescale = context.movie_timescale;
    layout.presentation_timescale =
        context.presentation_timescale;
    layout.movie_duration = context.movie_duration;
    layout.top_level_meta_offset =
        seq_box_set(&context.top_level_meta)
            ? context.top_level_meta.offset : SIZE_MAX;
    layout.limits = context.limits;
    layout.workspace_hash = 0U;
    seq_store(
        (unsigned char *)workspace, 0U,
        &layout, sizeof(layout));
    hash = seq_workspace_hash(
        (const unsigned char *)workspace, layout.layout_size);
    layout.workspace_hash = hash;
    seq_store(
        (unsigned char *)workspace, 0U,
        &layout, sizeof(layout));
    index->opaque[0] = SEQ_HANDLE_MAGIC;
    index->opaque[1] = (uintptr_t)workspace;
    index->opaque[2] = (uintptr_t)data;
    index->opaque[3] = (uintptr_t)layout.layout_size;
    index->opaque[4] = (uintptr_t)size;
    index->opaque[5] = (uintptr_t)generation;
    index->opaque[6] = (uintptr_t)hash;
    index->opaque[AVIF_SEQUENCE_INDEX_WORDS - 1U] =
        seq_handle_cookie(index);
    return AVIFDEC_OK;
}

static void seq_track_info_fill(
    const SeqTrackRecord *record,
    const unsigned char *data,
    AvifSequenceTrackInfo *track) {
    avifdec_memory_fill(track, 0U, sizeof(*track));
    track->track_id = record->track_id;
    track->handler_type = record->handler_type;
    track->flags = record->flags;
    track->coded_width = record->coded_width;
    track->coded_height = record->coded_height;
    track->presentation_width = record->presentation_width;
    track->presentation_height = record->presentation_height;
    track->crop = record->crop;
    track->clean_aperture = record->clean_aperture;
    track->color.color_primaries = record->color_primaries;
    track->color.transfer_characteristics =
        record->transfer_characteristics;
    track->color.matrix_coefficients =
        record->matrix_coefficients;
    track->color.color_range = record->color_range;
    track->color.has_nclx = record->has_nclx;
    if (record->has_icc != 0U) {
        track->color.icc.data = data + record->icc_offset;
        track->color.icc.size = record->icc_size;
    }
    avifdec_memory_copy(
        track->matrix, record->matrix, sizeof(track->matrix));
    track->media_timescale = record->media_timescale;
    track->media_duration = record->media_duration;
    track->sample_count = record->sample_count;
    track->fragment_count = record->fragment_count;
    track->edit_count = record->edit_count;
    track->presentation_count = record->presentation_count;
    track->transform_flags = record->transform_flags;
    track->irot_angle = record->irot_angle;
    track->imir_axis = record->imir_axis;
    track->bit_depth = record->bit_depth;
    track->alpha_color_range = record->alpha_color_range;
}

AvifdecStatus avif_sequence_track_query(
    const AvifSequenceIndex *index,
    size_t track_index,
    AvifSequenceTrackInfo *track,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    SeqTrackRecord record;
    AvifSequenceTrackInfo result;
    AvifdecStatus status;

    if (track == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    if (status != AVIFDEC_OK) return status;
    if (track_index >= header.track_count) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, track_index, 0U, error);
    }
    seq_load(
        workspace,
        header.tracks_offset + track_index * sizeof(record),
        &record, sizeof(record));
    seq_track_info_fill(&record, data, &result);
    *track = result;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_track_reference_query(
    const AvifSequenceIndex *index,
    size_t reference_index,
    AvifSequenceTrackReferenceInfo *reference,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    SeqReferenceRecord record;
    AvifSequenceTrackReferenceInfo result;
    AvifdecStatus status;

    if (reference == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    (void)data;
    if (status != AVIFDEC_OK) return status;
    if (reference_index >= header.reference_count) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, reference_index, 0U, error);
    }
    seq_load(
        workspace,
        header.references_offset +
            reference_index * sizeof(record),
        &record, sizeof(record));
    result.from_track_id = record.from_track_id;
    result.to_track_id = record.to_track_id;
    result.relationship_type = record.relationship_type;
    *reference = result;
    return AVIFDEC_OK;
}

static int seq_has_reference(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t from_track_id,
    uint32_t to_track_id,
    uint32_t type) {
    size_t index;

    for (index = 0U; index < header->reference_count; ++index) {
        SeqReferenceRecord record;

        seq_load(
            workspace,
            header->references_offset + index * sizeof(record),
            &record, sizeof(record));
        if (record.from_track_id == from_track_id &&
            record.to_track_id == to_track_id &&
            record.relationship_type == type) {
            return 1;
        }
    }
    return 0;
}

static int seq_geometry_equal(
    const SeqTrackRecord *main_track,
    const SeqTrackRecord *alpha_track) {
    return main_track->coded_width == alpha_track->coded_width &&
           main_track->coded_height == alpha_track->coded_height &&
           main_track->presentation_width ==
               alpha_track->presentation_width &&
           main_track->presentation_height ==
               alpha_track->presentation_height &&
           main_track->crop.x == alpha_track->crop.x &&
           main_track->crop.y == alpha_track->crop.y &&
           main_track->crop.width == alpha_track->crop.width &&
           main_track->crop.height == alpha_track->crop.height &&
           main_track->transform_flags ==
               alpha_track->transform_flags &&
           main_track->irot_angle == alpha_track->irot_angle &&
           main_track->imir_axis == alpha_track->imir_axis &&
           main_track->pixel_aspect_h_spacing ==
               alpha_track->pixel_aspect_h_spacing &&
           main_track->pixel_aspect_v_spacing ==
               alpha_track->pixel_aspect_v_spacing &&
           avifdec_memory_compare(
               main_track->matrix, alpha_track->matrix,
               sizeof(main_track->matrix)) == 0;
}

static int seq_alpha_covers_main(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t main_track_id,
    uint32_t alpha_track_id) {
    size_t main_index;

    for (main_index = 0U;
         main_index < header->presentation_count; ++main_index) {
        SeqPresentationRecord main_presentation;
        uint64_t main_end;
        size_t alpha_index;
        size_t covering = 0U;

        seq_load(
            workspace,
            header->presentations_offset +
                main_index * sizeof(main_presentation),
            &main_presentation, sizeof(main_presentation));
        if (main_presentation.track_id != main_track_id) continue;
        if (main_presentation.duration >
            UINT64_MAX - main_presentation.start_time) {
            return 0;
        }
        main_end =
            main_presentation.start_time +
            main_presentation.duration;
        for (alpha_index = 0U;
             alpha_index < header->presentation_count;
             ++alpha_index) {
            SeqPresentationRecord alpha_presentation;
            uint64_t alpha_end;

            seq_load(
                workspace,
                header->presentations_offset +
                    alpha_index * sizeof(alpha_presentation),
                &alpha_presentation, sizeof(alpha_presentation));
            if (alpha_presentation.track_id !=
                alpha_track_id) {
                continue;
            }
            if (alpha_presentation.duration >
                UINT64_MAX - alpha_presentation.start_time) {
                return 0;
            }
            alpha_end =
                alpha_presentation.start_time +
                alpha_presentation.duration;
            if (alpha_presentation.start_time <=
                    main_presentation.start_time &&
                alpha_end >= main_end) {
                ++covering;
            }
        }
        if (covering != 1U) return 0;
    }
    return 1;
}

AvifdecStatus avif_sequence_select(
    const AvifSequenceIndex *index,
    const AvifSequenceSelectOptions *options,
    AvifSequenceSelection *selection,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    AvifSequenceSelectOptions requested;
    SeqTrackRecord main_track;
    SeqTrackRecord alpha_track;
    AvifSequenceSelection result;
    size_t track_index;
    size_t main_count = 0U;
    size_t alpha_count = 0U;
    uint32_t inferred_main = 0U;
    uint32_t inferred_alpha = 0U;
    AvifdecStatus status;

    if (selection == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    (void)data;
    if (status != AVIFDEC_OK) return status;
    avifdec_memory_fill(&requested, 0U, sizeof(requested));
    if (options != 0) requested = *options;
    if ((requested.flags &
         ~AVIF_SEQUENCE_SELECT_DISABLE_ALPHA) != 0U ||
        ((requested.flags &
          AVIF_SEQUENCE_SELECT_DISABLE_ALPHA) != 0U &&
         requested.alpha_track_id != 0U)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    if (requested.main_track_id == 0U) {
        for (track_index = 0U;
             track_index < header.track_count; ++track_index) {
            SeqTrackRecord candidate;

            seq_load(
                workspace,
                header.tracks_offset +
                    track_index * sizeof(candidate),
                &candidate, sizeof(candidate));
            if ((candidate.flags &
                 AVIF_SEQUENCE_TRACK_VISUAL) != 0U &&
                candidate.enabled != 0U) {
                inferred_main = candidate.track_id;
                ++main_count;
            }
        }
        if (main_count != 1U) {
            return seq_query_fail(
                AVIFDEC_UNSUPPORTED, 0U,
                SEQ_FOURCC('t', 'r', 'a', 'k'), error);
        }
        requested.main_track_id = inferred_main;
    }
    if (!seq_track_record_find(
            workspace, &header, requested.main_track_id,
            0, &main_track) ||
        (main_track.flags & AVIF_SEQUENCE_TRACK_VISUAL) == 0U) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U,
            SEQ_FOURCC('t', 'r', 'a', 'k'), error);
    }
    avifdec_memory_fill(&alpha_track, 0U, sizeof(alpha_track));
    if ((requested.flags &
         AVIF_SEQUENCE_SELECT_DISABLE_ALPHA) == 0U) {
        if (requested.alpha_track_id == 0U) {
            for (track_index = 0U;
                 track_index < header.track_count; ++track_index) {
                SeqTrackRecord candidate;

                seq_load(
                    workspace,
                    header.tracks_offset +
                        track_index * sizeof(candidate),
                    &candidate, sizeof(candidate));
                if ((candidate.flags &
                     AVIF_SEQUENCE_TRACK_ALPHA) != 0U &&
                    seq_has_reference(
                        workspace, &header,
                        candidate.track_id, main_track.track_id,
                        SEQ_FOURCC('a', 'u', 'x', 'l'))) {
                    inferred_alpha = candidate.track_id;
                    ++alpha_count;
                }
            }
            if (alpha_count > 1U) {
                return seq_query_fail(
                    AVIFDEC_UNSUPPORTED, 0U,
                    SEQ_FOURCC('a', 'u', 'x', 'l'), error);
            }
            requested.alpha_track_id = inferred_alpha;
        }
        if (requested.alpha_track_id != 0U) {
            if (!seq_track_record_find(
                    workspace, &header,
                    requested.alpha_track_id, 0, &alpha_track) ||
                (alpha_track.flags &
                 AVIF_SEQUENCE_TRACK_ALPHA) == 0U ||
                !seq_has_reference(
                    workspace, &header,
                    alpha_track.track_id, main_track.track_id,
                    SEQ_FOURCC('a', 'u', 'x', 'l')) ||
                !seq_geometry_equal(&main_track, &alpha_track) ||
                !seq_alpha_covers_main(
                    workspace, &header,
                    main_track.track_id, alpha_track.track_id)) {
                return seq_query_fail(
                    AVIFDEC_INVALID_DATA, 0U,
                    SEQ_FOURCC('a', 'u', 'x', 'l'), error);
            }
        }
    }
    avifdec_memory_fill(&result, 0U, sizeof(result));
    result.main_track_id = main_track.track_id;
    result.alpha_track_id = requested.alpha_track_id;
    result.flags = requested.flags;
    result.timescale = header.presentation_timescale;
    result.duration = main_track.timeline_duration;
    result.presentation_count = main_track.presentation_count;
    result.has_alpha =
        (uint8_t)(requested.alpha_track_id != 0U);
    result.alpha_premultiplied =
        (uint8_t)(result.has_alpha != 0U &&
                  seq_has_reference(
                      workspace, &header,
                      main_track.track_id,
                      requested.alpha_track_id,
                      SEQ_FOURCC('p', 'r', 'e', 'm')));
    *selection = result;
    return AVIFDEC_OK;
}

static AvifdecStatus seq_selection_validate(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    AvifSequenceSelection *validated,
    AvifdecError *error) {
    AvifSequenceSelectOptions options;
    AvifdecStatus status;

    if (selection == 0) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    options.main_track_id = selection->main_track_id;
    options.alpha_track_id = selection->alpha_track_id;
    options.flags = selection->flags;
    if (selection->has_alpha == 0U) {
        options.alpha_track_id = 0U;
        options.flags |= AVIF_SEQUENCE_SELECT_DISABLE_ALPHA;
    }
    status = avif_sequence_select(
        index, &options, validated, error);
    if (status != AVIFDEC_OK) return status;
    if (validated->main_track_id != selection->main_track_id ||
        validated->alpha_track_id != selection->alpha_track_id ||
        validated->timescale != selection->timescale ||
        validated->duration != selection->duration ||
        validated->presentation_count !=
            selection->presentation_count ||
        validated->has_alpha != selection->has_alpha ||
        validated->alpha_premultiplied !=
            selection->alpha_premultiplied) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    return AVIFDEC_OK;
}

static void seq_image_info_fill(
    const SeqTrackRecord *record,
    const unsigned char *data,
    AvifdecImageInfo *image) {
    avifdec_memory_fill(image, 0U, sizeof(*image));
    image->primary_item_id = record->track_id;
    image->primary_item_type = SEQ_FOURCC('a', 'v', '0', '1');
    image->width = record->coded_width;
    image->height = record->coded_height;
    image->presentation_width = record->presentation_width;
    image->presentation_height = record->presentation_height;
    image->render_width = record->presentation_width;
    image->render_height = record->presentation_height;
    image->crop = record->crop;
    image->clean_aperture = record->clean_aperture;
    image->transform_flags = record->transform_flags;
    image->irot_angle = record->irot_angle;
    image->imir_axis = record->imir_axis;
    image->pixel_aspect_h_spacing =
        record->pixel_aspect_h_spacing;
    image->pixel_aspect_v_spacing =
        record->pixel_aspect_v_spacing;
    image->color_primaries = record->color_primaries;
    image->transfer_characteristics =
        record->transfer_characteristics;
    image->matrix_coefficients = record->matrix_coefficients;
    image->color_range = record->bitstream_color_range;
    image->has_nclx = record->has_nclx;
    if (record->has_icc != 0U) {
        image->icc_data = data + record->icc_offset;
        image->icc_size = record->icc_size;
    }
    image->profile = record->profile;
    image->level = record->level;
    image->tier = record->tier;
    image->bit_depth = record->bit_depth;
    image->monochrome = record->monochrome;
    image->subsampling_x = record->subsampling_x;
    image->subsampling_y = record->subsampling_y;
    image->chroma_sample_position =
        record->chroma_sample_position;
    image->channel_count = record->monochrome != 0U ? 1U : 3U;
}

AvifdecStatus avif_sequence_track_image_query(
    const AvifSequenceIndex *index,
    uint32_t track_id,
    AvifdecImageInfo *image,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    SeqTrackRecord record;
    AvifdecImageInfo result;
    AvifdecStatus status;

    if (image == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    if (status != AVIFDEC_OK) return status;
    if (!seq_track_record_find(
            workspace, &header, track_id, 0, &record)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, track_id,
            SEQ_FOURCC('t', 'r', 'a', 'k'), error);
    }
    seq_image_info_fill(&record, data, &result);
    *image = result;
    return AVIFDEC_OK;
}

static int seq_find_covering_alpha(
    const unsigned char *workspace,
    const SeqWorkspaceHeader *header,
    uint32_t alpha_track_id,
    const SeqPresentationRecord *main_presentation,
    SeqPresentationRecord *alpha_presentation) {
    uint64_t main_end =
        main_presentation->start_time +
        main_presentation->duration;
    size_t index;
    size_t count = 0U;

    for (index = 0U; index < header->presentation_count; ++index) {
        SeqPresentationRecord candidate;
        uint64_t candidate_end;

        seq_load(
            workspace,
            header->presentations_offset + index * sizeof(candidate),
            &candidate, sizeof(candidate));
        if (candidate.track_id != alpha_track_id) continue;
        candidate_end = candidate.start_time + candidate.duration;
        if (candidate.start_time <=
                main_presentation->start_time &&
            candidate_end >= main_end) {
            *alpha_presentation = candidate;
            ++count;
        }
    }
    return count == 1U;
}

AvifdecStatus avif_sequence_presentation_query(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    size_t presentation_index,
    AvifSequencePresentationInfo *presentation,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    AvifSequenceSelection validated;
    SeqTrackRecord main_track;
    SeqTrackRecord alpha_track;
    SeqPresentationRecord main_presentation;
    SeqPresentationRecord alpha_presentation;
    SeqSampleRecord main_sample;
    SeqSampleRecord alpha_sample;
    AvifSequencePresentationInfo result;
    AvifdecStatus status;

    if (presentation == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_selection_validate(
        index, selection, &validated, error);
    if (status != AVIFDEC_OK) return status;
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    if (status != AVIFDEC_OK) return status;
    if (presentation_index >= validated.presentation_count ||
        !seq_track_record_find(
            workspace, &header, validated.main_track_id,
            0, &main_track) ||
        !seq_presentation_record_find(
            workspace, &header, validated.main_track_id,
            presentation_index, &main_presentation) ||
        !seq_sample_record_find(
            workspace, &header, validated.main_track_id,
            main_presentation.sample_index, &main_sample)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, presentation_index,
            SEQ_FOURCC('t', 'r', 'a', 'k'), error);
    }
    avifdec_memory_fill(&result, 0U, sizeof(result));
    seq_image_info_fill(&main_track, data, &result.image);
    result.presentation_index = presentation_index;
    result.main_sample_index = main_sample.sample_index;
    result.main_sync_sample_index =
        main_sample.sync_sample_index;
    result.alpha_sample_index = SIZE_MAX;
    result.alpha_sync_sample_index = SIZE_MAX;
    result.start_time = main_presentation.start_time;
    result.duration = main_presentation.duration;
    result.timescale = header.presentation_timescale;
    result.flags = main_presentation.flags;
    if (validated.has_alpha != 0U) {
        if (!seq_track_record_find(
                workspace, &header, validated.alpha_track_id,
                0, &alpha_track) ||
            !seq_find_covering_alpha(
                workspace, &header, validated.alpha_track_id,
                &main_presentation, &alpha_presentation) ||
            !seq_sample_record_find(
                workspace, &header, validated.alpha_track_id,
                alpha_presentation.sample_index, &alpha_sample)) {
            return seq_query_fail(
                AVIFDEC_INVALID_DATA, presentation_index,
                SEQ_FOURCC('a', 'u', 'x', 'l'), error);
        }
        result.image.has_alpha = 1U;
        result.image.alpha_premultiplied =
            validated.alpha_premultiplied;
        result.image.alpha_bit_depth = alpha_track.bit_depth;
        result.image.alpha_color_range =
            alpha_track.alpha_color_range;
        result.image.alpha_item_id = alpha_track.track_id;
        result.alpha_sample_index = alpha_sample.sample_index;
        result.alpha_sync_sample_index =
            alpha_sample.sync_sample_index;
        result.flags |= alpha_presentation.flags;
    }
    *presentation = result;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_sample_query(
    const AvifSequenceIndex *index,
    uint32_t track_id,
    size_t sample_index,
    AvifSequenceSampleInfo *sample,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    SeqTrackRecord track;
    SeqSampleRecord record;
    AvifSequenceSampleInfo result;
    AvifdecStatus status;

    if (sample == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    (void)data;
    if (status != AVIFDEC_OK) return status;
    if (!seq_track_record_find(
            workspace, &header, track_id, 0, &track) ||
        sample_index >= track.sample_count ||
        !seq_sample_record_find(
            workspace, &header, track_id,
            sample_index, &record)) {
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, sample_index,
            SEQ_FOURCC('t', 'r', 'a', 'k'), error);
    }
    avifdec_memory_fill(&result, 0U, sizeof(result));
    result.track_id = track_id;
    result.sample_index = record.sample_index;
    result.sync_sample_index = record.sync_sample_index;
    result.offset = record.offset;
    result.size = record.size;
    result.dts = record.dts;
    result.duration = record.duration;
    result.config_offset = track.config_offset;
    result.config_size = track.config_size;
    result.is_sync =
        (uint8_t)((record.flags & SEQ_SAMPLE_SYNC) != 0U);
    result.prepend_config =
        (uint8_t)((record.flags &
                   SEQ_SAMPLE_PREPEND_CONFIG) != 0U);
    result.fragmented =
        (uint8_t)((record.flags &
                   SEQ_SAMPLE_FRAGMENTED) != 0U);
    *sample = result;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_index_source(
    const AvifSequenceIndex *index,
    const unsigned char **data,
    size_t *size,
    AvifdecLimits *limits,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *source;
    SeqWorkspaceHeader header;
    AvifdecStatus status;

    if (data == 0 || size == 0 || limits == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &source, &header, error);
    (void)workspace;
    if (status != AVIFDEC_OK) return status;
    *data = source;
    *size = (size_t)index->opaque[4];
    *limits = header.limits;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_index_attach_extension(
    AvifSequenceIndex *index,
    size_t extension_size,
    AvifdecError *error) {
    const unsigned char *constant_workspace;
    const unsigned char *data;
    unsigned char *workspace;
    SeqWorkspaceHeader header;
    size_t total_size;
    uint64_t hash;
    AvifdecStatus status;

    status = seq_index_open(
        index, &constant_workspace, &data, &header, error);
    (void)data;
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(
            header.layout_size, extension_size, &total_size)) {
        return seq_query_fail(
            AVIFDEC_OVERFLOW, 0U, 0U, error);
    }
    (void)total_size;
    workspace = (unsigned char *)constant_workspace;
    header.extension_size = extension_size;
    header.workspace_hash = 0U;
    seq_store(workspace, 0U, &header, sizeof(header));
    hash = seq_workspace_hash(workspace, header.layout_size);
    header.workspace_hash = hash;
    seq_store(workspace, 0U, &header, sizeof(header));
    index->opaque[6] = (uintptr_t)hash;
    index->opaque[AVIF_SEQUENCE_INDEX_WORDS - 1U] =
        seq_handle_cookie(index);
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_index_extension(
    const AvifSequenceIndex *index,
    const unsigned char **workspace,
    size_t *base_size,
    size_t *extension_size,
    AvifdecError *error) {
    const unsigned char *source;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    AvifdecStatus status;

    if (workspace == 0 || base_size == 0 || extension_size == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &source, &data, &header, error);
    (void)data;
    if (status != AVIFDEC_OK) return status;
    *workspace = source;
    *base_size = header.layout_size;
    *extension_size = header.extension_size;
    return AVIFDEC_OK;
}

AvifdecStatus avif_sequence_index_top_level_meta(
    const AvifSequenceIndex *index,
    size_t *meta_offset,
    AvifdecError *error) {
    const unsigned char *workspace;
    const unsigned char *data;
    SeqWorkspaceHeader header;
    AvifdecStatus status;

    if (meta_offset == 0) {
        seq_error_clear(error);
        return seq_query_fail(
            AVIFDEC_INVALID_ARGUMENT, 0U, 0U, error);
    }
    status = seq_index_open(
        index, &workspace, &data, &header, error);
    (void)workspace;
    (void)data;
    if (status != AVIFDEC_OK) return status;
    *meta_offset = header.top_level_meta_offset;
    return AVIFDEC_OK;
}
