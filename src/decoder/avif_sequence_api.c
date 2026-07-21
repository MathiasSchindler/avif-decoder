#include "avif_sequence_decode.h"

#include "av1.h"
#include "av1_avif_conformance.h"
#include "av1_bitstream.h"
#include "av1_parse.h"
#include "avif_metadata_items.h"
#include "base.h"
#include "bmff.h"
#include "bmff_child.h"

#define AVIF_SEQUENCE_METADATA_MAGIC 0x41564946534d4554ULL
#define AVIF_SEQUENCE_METADATA_VERSION 1U
#define AVIF_SEQUENCE_METADATA_MAX_TRACKS 256U

typedef struct {
    uint64_t magic;
    uint64_t hash;
    size_t total_size;
    size_t metadata_offset;
    size_t metadata_count;
    size_t thumbnail_offset;
    size_t thumbnail_count;
    size_t span_offset;
    size_t span_count;
    uint32_t primary_item_id;
    uint32_t version;
} AvifSequenceMetadataHeader;

typedef struct {
    const unsigned char *data;
    size_t size;
} AvifSequenceApiValidationContext;

typedef struct {
    size_t meta_offset;
    uint32_t track_id;
} AvifSequenceTrackMetadataSource;

typedef struct {
    AvifSequenceTrackMetadataSource
        tracks[AVIF_SEQUENCE_METADATA_MAX_TRACKS];
    size_t track_count;
    size_t top_level_meta_offset;
} AvifSequenceMetadataSources;

static void avif_sequence_api_error_clear(AvifdecError *error) {
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
}

static AvifdecStatus avif_sequence_api_fail(
    AvifdecError *error,
    AvifdecStatus status,
    size_t offset,
    uint32_t context) {
    if (error != 0 && error->status == AVIFDEC_OK) {
        error->status = status;
        error->offset = offset;
        error->context = context;
    }
    return status;
}

static int avif_sequence_api_executor_valid(
    const AvifdecExecutor *executor) {
    return executor == 0 ||
           (executor->parallel_for != 0 &&
            executor->worker_count != 0U &&
            executor->worker_count <= AVIFDEC_EXECUTOR_MAX_WORKERS);
}

static AvifdecStatus avif_sequence_api_validate_header(
    void *user_data,
    uint32_t track_id,
    const unsigned char *sequence_header_obu,
    size_t sequence_header_obu_size,
    const AvifdecLimits *limits,
    AvifSequenceHeaderValidation *validation,
    AvifdecError *error) {
    AvifdecSpan span;
    Av1Stream stream;
    Av1Bits bits;
    Av1Sequence sequence;
    size_t payload_size;
    size_t index;
    uint8_t header;
    uint8_t extension_flag;
    uint8_t extension;
    AvifdecStatus status;
    uint64_t semantic_id;
    size_t file_offset = 0U;

    (void)track_id;
    if (validation == 0 ||
        (sequence_header_obu == 0 && sequence_header_obu_size != 0U)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (user_data != 0) {
        const AvifSequenceApiValidationContext *context =
            (const AvifSequenceApiValidationContext *)user_data;
        uintptr_t input_address = (uintptr_t)context->data;
        uintptr_t header_address =
            (uintptr_t)sequence_header_obu;

        if (header_address < input_address ||
            header_address - input_address >
                (uintptr_t)SIZE_MAX ||
            (size_t)(header_address - input_address) >
                context->size ||
            sequence_header_obu_size >
                context->size -
                    (size_t)(header_address - input_address)) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
        }
        file_offset =
            (size_t)(header_address - input_address);
    }
    avifdec_memory_fill(validation, 0U, sizeof(*validation));
    span.data = sequence_header_obu;
    span.size = sequence_header_obu_size;
    span.file_offset = file_offset;
    status = av1_avif_validate_obu_stream(
        &span, 1U, AVIFDEC_AV1_LOW_OVERHEAD, error);
    if (status != AVIFDEC_OK) return status;

    avifdec_memory_fill(&stream, 0U, sizeof(stream));
    stream.spans = &span;
    stream.span_count = 1U;
    stream.size = sequence_header_obu_size;
    stream.status = AVIFDEC_OK;
    header = av1_stream_read(&stream);
    extension_flag = (uint8_t)((header >> 2U) & 1U);
    if (((header >> 3U) & 15U) != 1U ||
        (header & 2U) == 0U) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, 0U,
            AVIFDEC_FOURCC('O', 'B', 'U', 1));
    }
    if (extension_flag != 0U) {
        extension = av1_stream_read(&stream);
        if ((extension >> 3U) != 0U) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 1U,
                AVIFDEC_FOURCC('O', 'B', 'U', 1));
        }
    }
    status = av1_leb128(&stream, &payload_size);
    if (status != AVIFDEC_OK) {
        return avif_sequence_api_fail(
            error, status, stream.position,
            AVIFDEC_FOURCC('O', 'B', 'U', 1));
    }
    if (payload_size != stream.size - stream.position) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, stream.position,
            AVIFDEC_FOURCC('O', 'B', 'U', 1));
    }
    av1_bits_init(&bits, &stream, stream.position, payload_size);
    status = av1_parse_sequence_header(
        &bits, limits == 0 ? 0U : limits->operating_point, &sequence);
    if (status != AVIFDEC_OK) {
        return avif_sequence_api_fail(
            error, status, av1_bits_offset(&bits),
            AVIFDEC_FOURCC('O', 'B', 'U', 1));
    }

    semantic_id = 1469598103934665603ULL;
    for (index = 0U; index < sequence_header_obu_size; ++index) {
        semantic_id ^= sequence_header_obu[index];
        semantic_id *= 1099511628211ULL;
    }
    validation->coded_width = sequence.max_width;
    validation->coded_height = sequence.max_height;
    validation->semantic_id = semantic_id;
    validation->av1c_marker = 1U;
    validation->av1c_version = 1U;
    validation->profile = sequence.profile;
    validation->level = sequence.level;
    validation->tier = sequence.tier;
    validation->high_bitdepth = (uint8_t)(sequence.bit_depth > 8U);
    validation->twelve_bit = (uint8_t)(sequence.bit_depth == 12U);
    validation->bit_depth = sequence.bit_depth;
    validation->monochrome = sequence.monochrome;
    validation->subsampling_x = sequence.subsampling_x;
    validation->subsampling_y = sequence.subsampling_y;
    validation->chroma_sample_position =
        sequence.chroma_sample_position;
    validation->color_description_present = 1U;
    validation->color_primaries = sequence.color_primaries;
    validation->transfer_characteristics =
        sequence.transfer_characteristics;
    validation->matrix_coefficients = sequence.matrix_coefficients;
    validation->color_range = sequence.color_range;
    return AVIFDEC_OK;
}

static AvifSequenceValidationCallbacks
avif_sequence_api_validation_callbacks(
    AvifSequenceApiValidationContext *context) {
    AvifSequenceValidationCallbacks callbacks;

    callbacks.user_data = context;
    callbacks.validate_header = avif_sequence_api_validate_header;
    return callbacks;
}

static AvifdecStatus avif_sequence_api_span_at(
    void *opaque,
    size_t span_index,
    AvifdecSpan *span,
    AvifdecError *error) {
    const AvifSequenceReplay *replay =
        (const AvifSequenceReplay *)opaque;
    size_t sample_index;

    return avif_sequence_replay_span_query(
        replay, span_index, span, &sample_index, error);
}

static AvifdecStatus avif_sequence_api_descriptor_requirement(
    size_t span_count,
    size_t *required) {
    size_t bytes;

    if (!avifdec_size_multiply(
            span_count, sizeof(AvifdecSpan), &bytes) ||
        !avifdec_size_add(
            bytes, _Alignof(AvifdecSpan) - 1U, required)) {
        return AVIFDEC_OVERFLOW;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_api_replay_query(
    void *user_data,
    const AvifSequenceReplay *replay,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    AvifdecError *error) {
    Av1SpanSource source;
    AvifdecLimits replay_limits;
    size_t descriptor_required;
    AvifdecStatus status;

    (void)user_data;
    if (info == 0 || !avif_sequence_api_executor_valid(executor)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (replay == 0 || replay->span_count == 0U) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_sequence_api_descriptor_requirement(
        replay->span_count, &descriptor_required);
    if (status != AVIFDEC_OK) return status;
    replay_limits = avifdec_limits_effective(limits);
    replay_limits.av1_framing = AVIFDEC_AV1_LOW_OVERHEAD;
    source.context = (void *)replay;
    source.span_count = replay->span_count;
    source.span_at = avif_sequence_api_span_at;
    source.error = error;
    status = avifdec_av1_query_source_ex(
        &source, &replay_limits,
        executor == 0 ? 1U : executor->worker_count,
        info, error);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_add(
            info->workspace_required, descriptor_required,
            &info->workspace_required)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_api_replay_decode(
    void *user_data,
    const AvifSequenceReplay *replay,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    const AvifdecImageInfo *info,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error) {
    AvifdecArena arena;
    AvifdecSpan *spans;
    AvifdecLimits replay_limits;
    AvifdecImageInfo decode_info;
    size_t descriptor_bytes;
    size_t descriptor_required;
    size_t av1_required;
    size_t index;
    AvifdecStatus status;

    (void)user_data;
    if (info == 0 || image == 0 ||
        !avif_sequence_api_executor_valid(executor)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (replay == 0 || replay->span_count == 0U ||
        (workspace == 0 && workspace_size != 0U)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (!avifdec_size_multiply(
            replay->span_count, sizeof(*spans),
            &descriptor_bytes)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    status = avif_sequence_api_descriptor_requirement(
        replay->span_count, &descriptor_required);
    if (status != AVIFDEC_OK ||
        info->workspace_required < descriptor_required) {
        return avif_sequence_api_fail(
            error, status != AVIFDEC_OK ? status : AVIFDEC_INVALID_DATA,
            0U, 0U);
    }
    av1_required = info->workspace_required - descriptor_required;
    avifdec_arena_init(&arena, workspace, workspace_size);
    spans = (AvifdecSpan *)avifdec_arena_allocate(
        &arena, descriptor_bytes, _Alignof(AvifdecSpan));
    if (arena.status != AVIFDEC_OK || spans == 0) {
        return avif_sequence_api_fail(
            error, arena.status, 0U, 0U);
    }
    for (index = 0U; index < replay->span_count; ++index) {
        status = avif_sequence_api_span_at(
            (void *)replay, index, &spans[index], error);
        if (status != AVIFDEC_OK) return status;
    }
    if (arena.size - arena.used < av1_required) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    replay_limits = avifdec_limits_effective(limits);
    replay_limits.av1_framing = AVIFDEC_AV1_LOW_OVERHEAD;
    decode_info = *info;
    if (replay->alpha != 0U) {
        decode_info.has_nclx = 0U;
        decode_info.icc_data = 0;
        decode_info.icc_size = 0U;
    }
    decode_info.workspace_required = av1_required;
    return avifdec_av1_decode_ex(
        spans, replay->span_count, &replay_limits, executor,
        &decode_info, arena.data + arena.used,
        arena.size - arena.used, image, trace, error);
}

static AvifSequenceDecodeCallbacks
avif_sequence_api_decode_callbacks(void) {
    AvifSequenceDecodeCallbacks callbacks;

    callbacks.user_data = 0;
    callbacks.query = avif_sequence_api_replay_query;
    callbacks.decode = avif_sequence_api_replay_decode;
    return callbacks;
}

static uint64_t avif_sequence_metadata_hash(
    const unsigned char *data,
    size_t size) {
    const size_t skip =
        offsetof(AvifSequenceMetadataHeader, hash);
    const size_t skip_end = skip + sizeof(uint64_t);
    uint64_t hash = 1469598103934665603ULL;
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned char value =
            index >= skip && index < skip_end ? 0U : data[index];

        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static AvifdecStatus avif_sequence_metadata_track_id(
    const unsigned char *data,
    const AvifBmffChild *tkhd,
    uint32_t *track_id,
    AvifdecError *error) {
    uint8_t version;
    size_t offset;

    if (data == 0 || tkhd == 0 || track_id == 0 ||
        tkhd->payload_size < 4U) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    version = data[tkhd->payload_offset];
    if (version == 0U) {
        offset = tkhd->payload_offset + 12U;
    } else if (version == 1U) {
        offset = tkhd->payload_offset + 20U;
    } else {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, tkhd->offset, tkhd->type);
    }
    if (offset > tkhd->payload_offset + tkhd->payload_size ||
        tkhd->payload_offset + tkhd->payload_size - offset < 4U) {
        return avif_sequence_api_fail(
            error, AVIFDEC_TRUNCATED, tkhd->offset, tkhd->type);
    }
    *track_id = avifdec_load_u32be(data + offset);
    if (*track_id == 0U) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, offset, tkhd->type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_metadata_sources(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifSequenceMetadataSources *sources,
    AvifdecError *error) {
    AvifBmffChildIterator top_iterator;
    AvifBmffChildIterator movie_iterator;
    AvifBmffChild child;
    AvifBmffChild movie;
    AvifdecLimits effective;
    int has_child;
    AvifdecStatus status;

    if (sources == 0 || (data == 0 && size != 0U)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avifdec_memory_fill(sources, 0U, sizeof(*sources));
    sources->top_level_meta_offset = SIZE_MAX;
    avifdec_memory_fill(&movie, 0U, sizeof(movie));
    effective = avifdec_limits_effective(limits);
    status = avif_bmff_child_iterator_init(
        &top_iterator, data, size, 0U, size, 0U, error);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        status = avif_bmff_child_next(
            &top_iterator, &child, &has_child, error);
        if (status != AVIFDEC_OK || !has_child) break;
        if (child.type == AVIFDEC_FOURCC('m', 'e', 't', 'a')) {
            if (sources->top_level_meta_offset != SIZE_MAX) {
                return avif_sequence_api_fail(
                    error, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            sources->top_level_meta_offset = child.offset;
        } else if (child.type ==
                   AVIFDEC_FOURCC('m', 'o', 'o', 'v')) {
            if (movie.size != 0U) {
                return avif_sequence_api_fail(
                    error, AVIFDEC_INVALID_DATA,
                    child.offset, child.type);
            }
            movie = child;
        }
    }
    if (status != AVIFDEC_OK || movie.size == 0U) return status;
    status = avif_bmff_child_iterator_init(
        &movie_iterator, data, size, movie.payload_offset,
        movie.payload_size, movie.type, error);
    if (status != AVIFDEC_OK) return status;
    for (;;) {
        AvifBmffChildIterator track_iterator;
        AvifBmffChild track_child;
        AvifBmffChild tkhd;
        AvifBmffChild meta;
        uint32_t track_id;

        status = avif_bmff_child_next(
            &movie_iterator, &child, &has_child, error);
        if (status != AVIFDEC_OK || !has_child) break;
        if (child.type != AVIFDEC_FOURCC('t', 'r', 'a', 'k')) {
            continue;
        }
        avifdec_memory_fill(&tkhd, 0U, sizeof(tkhd));
        avifdec_memory_fill(&meta, 0U, sizeof(meta));
        status = avif_bmff_child_iterator_init(
            &track_iterator, data, size, child.payload_offset,
            child.payload_size, child.type, error);
        if (status != AVIFDEC_OK) return status;
        for (;;) {
            status = avif_bmff_child_next(
                &track_iterator, &track_child, &has_child, error);
            if (status != AVIFDEC_OK || !has_child) break;
            if (track_child.type ==
                AVIFDEC_FOURCC('t', 'k', 'h', 'd')) {
                if (tkhd.size != 0U) {
                    return avif_sequence_api_fail(
                        error, AVIFDEC_INVALID_DATA,
                        track_child.offset, track_child.type);
                }
                tkhd = track_child;
            } else if (track_child.type ==
                       AVIFDEC_FOURCC('m', 'e', 't', 'a')) {
                if (meta.size != 0U) {
                    return avif_sequence_api_fail(
                        error, AVIFDEC_INVALID_DATA,
                        track_child.offset, track_child.type);
                }
                meta = track_child;
            }
        }
        if (status != AVIFDEC_OK || meta.size == 0U) {
            if (status != AVIFDEC_OK) return status;
            continue;
        }
        if (tkhd.size == 0U) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA,
                child.offset, child.type);
        }
        status = avif_sequence_metadata_track_id(
            (const unsigned char *)data, &tkhd, &track_id, error);
        if (status != AVIFDEC_OK) return status;
        if (sources->track_count >=
                AVIF_SEQUENCE_METADATA_MAX_TRACKS ||
            sources->track_count >= effective.max_tracks) {
            return avif_sequence_api_fail(
                error, AVIFDEC_LIMIT_EXCEEDED,
                meta.offset, meta.type);
        }
        sources->tracks[sources->track_count].meta_offset =
            meta.offset;
        sources->tracks[sources->track_count].track_id = track_id;
        ++sources->track_count;
    }
    return status;
}

static AvifdecStatus avif_sequence_metadata_count(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifdecMetadataResult *result,
    AvifdecError *error) {
    AvifSequenceMetadataSources sources;
    AvifItemIndexLimits item_limits;
    AvifdecLimits effective;
    AvifdecMetadataResult part;
    size_t source_index;
    size_t row_count;
    AvifdecStatus status;

    avifdec_memory_fill(result, 0U, sizeof(*result));
    status = avif_sequence_metadata_sources(
        data, size, limits, &sources, error);
    if (status != AVIFDEC_OK) return status;
    avif_item_index_limits_from_public(limits, &item_limits);
    effective = avifdec_limits_effective(limits);
    if (sources.top_level_meta_offset != SIZE_MAX) {
        status = avif_metadata_items_query_meta(
            data, size, &item_limits,
            sources.top_level_meta_offset,
            0, 0U, 0, 0U, 0, 0U,
            &part, error);
        if (status != AVIFDEC_OK) return status;
        *result = part;
    }
    for (source_index = 0U;
         source_index < sources.track_count;
         ++source_index) {
        status = avif_metadata_items_query_meta(
            data, size, &item_limits,
            sources.tracks[source_index].meta_offset,
            0, 0U, 0, 0U, 0, 0U, &part, error);
        if (status != AVIFDEC_OK) return status;
        if (!avifdec_size_add(
                result->metadata_count, part.metadata_count,
                &result->metadata_count) ||
            !avifdec_size_add(
                result->thumbnail_count, part.thumbnail_count,
                &result->thumbnail_count) ||
            !avifdec_size_add(
                result->span_count, part.span_count,
                &result->span_count)) {
            return avif_sequence_api_fail(
                error, AVIFDEC_OVERFLOW, 0U, 0U);
        }
    }
    if (!avifdec_size_add(
            result->metadata_count, result->thumbnail_count,
            &row_count)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (row_count > effective.max_metadata_items ||
        result->span_count > effective.max_metadata_spans) {
        return avif_sequence_api_fail(
            error, AVIFDEC_LIMIT_EXCEEDED, 0U, 0U);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_metadata_layout(
    const AvifdecMetadataResult *metadata,
    size_t *required,
    AvifSequenceMetadataHeader *layout) {
    AvifdecArena sizing;
    size_t metadata_bytes;
    size_t thumbnail_bytes;
    size_t span_bytes;
    size_t maximum_alignment = _Alignof(AvifdecMetadataInfo);

    if (metadata == 0 || required == 0 || layout == 0) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    if (_Alignof(AvifdecThumbnailInfo) > maximum_alignment) {
        maximum_alignment = _Alignof(AvifdecThumbnailInfo);
    }
    if (_Alignof(AvifdecSpan) > maximum_alignment) {
        maximum_alignment = _Alignof(AvifdecSpan);
    }
    if (!avifdec_size_multiply(
            metadata->metadata_count,
            sizeof(AvifdecMetadataInfo), &metadata_bytes) ||
        !avifdec_size_multiply(
            metadata->thumbnail_count,
            sizeof(AvifdecThumbnailInfo), &thumbnail_bytes) ||
        !avifdec_size_multiply(
            metadata->span_count,
            sizeof(AvifdecSpan), &span_bytes)) {
        return AVIFDEC_OVERFLOW;
    }
    avifdec_memory_fill(layout, 0U, sizeof(*layout));
    avifdec_arena_init_sizing(&sizing);
    (void)avifdec_arena_allocate(
        &sizing, sizeof(*layout), 1U);
    layout->metadata_offset = avifdec_arena_required(&sizing);
    (void)avifdec_arena_allocate(
        &sizing, metadata_bytes, _Alignof(AvifdecMetadataInfo));
    layout->thumbnail_offset = avifdec_arena_required(&sizing);
    (void)avifdec_arena_allocate(
        &sizing, thumbnail_bytes, _Alignof(AvifdecThumbnailInfo));
    layout->span_offset = avifdec_arena_required(&sizing);
    (void)avifdec_arena_allocate(
        &sizing, span_bytes, _Alignof(AvifdecSpan));
    if (sizing.status != AVIFDEC_OK ||
        !avifdec_size_add(
            avifdec_arena_required(&sizing),
            maximum_alignment - 1U, required)) {
        return AVIFDEC_OVERFLOW;
    }
    layout->metadata_count = metadata->metadata_count;
    layout->thumbnail_count = metadata->thumbnail_count;
    layout->span_count = metadata->span_count;
    layout->primary_item_id = metadata->primary_item_id;
    layout->total_size = *required;
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_metadata_fill(
    const AvifdecSequenceIndex *sequence_index,
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    unsigned char *workspace,
    size_t workspace_size,
    const AvifdecMetadataResult *count,
    AvifdecError *error) {
    AvifSequenceMetadataHeader layout;
    AvifSequenceMetadataHeader stored;
    AvifdecMetadataInfo *metadata;
    AvifdecThumbnailInfo *thumbnails;
    AvifdecSpan *spans;
    AvifSequenceMetadataSources sources;
    AvifItemIndexLimits item_limits;
    AvifdecMetadataResult part;
    AvifdecMetadataResult result;
    AvifdecArena arena;
    size_t required;
    size_t metadata_cursor = 0U;
    size_t thumbnail_cursor = 0U;
    size_t span_cursor = 0U;
    size_t source_index;
    size_t index;
    size_t normalized_top_level_meta;
    AvifdecStatus status;

    status = avif_sequence_metadata_layout(
        count, &required, &layout);
    if (status != AVIFDEC_OK) return status;
    if (workspace == 0 || workspace_size < required) {
        return AVIFDEC_OUT_OF_MEMORY;
    }
    avifdec_memory_fill(workspace, 0U, required);
    avifdec_arena_init(&arena, workspace, required);
    (void)avifdec_arena_allocate(&arena, sizeof(layout), 1U);
    metadata = (AvifdecMetadataInfo *)avifdec_arena_allocate(
        &arena, count->metadata_count * sizeof(*metadata),
        _Alignof(AvifdecMetadataInfo));
    thumbnails = (AvifdecThumbnailInfo *)avifdec_arena_allocate(
        &arena, count->thumbnail_count * sizeof(*thumbnails),
        _Alignof(AvifdecThumbnailInfo));
    spans = (AvifdecSpan *)avifdec_arena_allocate(
        &arena, count->span_count * sizeof(*spans),
        _Alignof(AvifdecSpan));
    if (arena.status != AVIFDEC_OK ||
        (count->metadata_count != 0U && metadata == 0) ||
        (count->thumbnail_count != 0U && thumbnails == 0) ||
        (count->span_count != 0U && spans == 0)) {
        return arena.status;
    }
    layout.metadata_offset =
        (size_t)((unsigned char *)metadata - workspace);
    layout.thumbnail_offset =
        (size_t)((unsigned char *)thumbnails - workspace);
    layout.span_offset =
        (size_t)((unsigned char *)spans - workspace);
    status = avif_sequence_metadata_sources(
        data, size, limits, &sources, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_index_top_level_meta(
        sequence_index, &normalized_top_level_meta, error);
    if (status != AVIFDEC_OK) return status;
    if (normalized_top_level_meta !=
        sources.top_level_meta_offset) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, 0U,
            AVIFDEC_FOURCC('m', 'e', 't', 'a'));
    }
    sources.top_level_meta_offset = normalized_top_level_meta;
    avif_item_index_limits_from_public(limits, &item_limits);
    if (sources.top_level_meta_offset != SIZE_MAX) {
        status = avif_metadata_items_query_meta(
            data, size, &item_limits,
            sources.top_level_meta_offset,
            0, 0U, 0, 0U, 0, 0U,
            &part, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_metadata_items_query_meta(
            data, size, &item_limits,
            sources.top_level_meta_offset,
            part.metadata_count == 0U
                ? 0 : metadata + metadata_cursor,
            part.metadata_count,
            part.thumbnail_count == 0U
                ? 0 : thumbnails + thumbnail_cursor,
            part.thumbnail_count,
            part.span_count == 0U ? 0 : spans + span_cursor,
            part.span_count, &result, error);
        if (status != AVIFDEC_OK) return status;
        if (result.primary_item_id != part.primary_item_id ||
            result.metadata_count != part.metadata_count ||
            result.thumbnail_count != part.thumbnail_count ||
            result.span_count != part.span_count) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        for (index = 0U; index < part.metadata_count; ++index) {
            AvifdecMetadataInfo *row =
                &metadata[metadata_cursor + index];

            row->span_index += span_cursor;
            row->target_item_id = 0U;
            row->target_track_id = 0U;
            row->relationship_type = 0U;
            row->scope = AVIFDEC_METADATA_SCOPE_UNSCOPED;
            row->flags |= AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE;
        }
        for (index = 0U; index < part.thumbnail_count; ++index) {
            AvifdecThumbnailInfo *row =
                &thumbnails[thumbnail_cursor + index];

            row->span_index += span_cursor;
            row->target_item_id = 0U;
            row->target_track_id = 0U;
            row->flags |= AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE;
        }
        metadata_cursor += part.metadata_count;
        thumbnail_cursor += part.thumbnail_count;
        span_cursor += part.span_count;
    }
    for (source_index = 0U;
         source_index < sources.track_count;
         ++source_index) {
        const AvifSequenceTrackMetadataSource *source =
            &sources.tracks[source_index];

        status = avif_metadata_items_query_meta(
            data, size, &item_limits, source->meta_offset,
            0, 0U, 0, 0U, 0, 0U, &part, error);
        if (status != AVIFDEC_OK) return status;
        status = avif_metadata_items_query_meta(
            data, size, &item_limits, source->meta_offset,
            part.metadata_count == 0U
                ? 0 : metadata + metadata_cursor,
            part.metadata_count,
            part.thumbnail_count == 0U
                ? 0 : thumbnails + thumbnail_cursor,
            part.thumbnail_count,
            part.span_count == 0U ? 0 : spans + span_cursor,
            part.span_count, &result, error);
        if (status != AVIFDEC_OK) return status;
        if (result.metadata_count != part.metadata_count ||
            result.thumbnail_count != part.thumbnail_count ||
            result.span_count != part.span_count) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        for (index = 0U; index < part.metadata_count; ++index) {
            AvifdecMetadataInfo *row =
                &metadata[metadata_cursor + index];

            row->span_index += span_cursor;
            row->target_item_id = 0U;
            row->target_track_id = source->track_id;
            row->relationship_type = 0U;
            row->scope = AVIFDEC_METADATA_SCOPE_TRACK;
            row->flags |= AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE;
        }
        for (index = 0U; index < part.thumbnail_count; ++index) {
            AvifdecThumbnailInfo *row =
                &thumbnails[thumbnail_cursor + index];

            row->span_index += span_cursor;
            row->target_item_id = 0U;
            row->target_track_id = source->track_id;
            row->flags |= AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE;
        }
        metadata_cursor += part.metadata_count;
        thumbnail_cursor += part.thumbnail_count;
        span_cursor += part.span_count;
    }
    if (metadata_cursor != count->metadata_count ||
        thumbnail_cursor != count->thumbnail_count ||
        span_cursor != count->span_count) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    layout.magic = AVIF_SEQUENCE_METADATA_MAGIC;
    layout.version = AVIF_SEQUENCE_METADATA_VERSION;
    layout.hash = 0U;
    avifdec_memory_copy(workspace, &layout, sizeof(layout));
    stored = layout;
    stored.hash = avif_sequence_metadata_hash(workspace, required);
    avifdec_memory_copy(workspace, &stored, sizeof(stored));
    return AVIFDEC_OK;
}

static AvifdecStatus avif_sequence_metadata_open(
    const AvifdecSequenceIndex *index,
    const unsigned char **workspace,
    AvifSequenceMetadataHeader *header,
    AvifdecLimits *limits,
    AvifdecError *error) {
    const unsigned char *data;
    const unsigned char *base_workspace;
    size_t size;
    size_t base_size;
    size_t extension_size;
    const unsigned char *extension;
    AvifdecStatus status;

    status = avif_sequence_index_source(
        index, &data, &size, limits, error);
    (void)data;
    (void)size;
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_index_extension(
        index, &base_workspace, &base_size, &extension_size, error);
    if (status != AVIFDEC_OK) return status;
    extension = base_workspace + base_size;
    avifdec_memory_copy(header, extension, sizeof(*header));
    if (header->magic != AVIF_SEQUENCE_METADATA_MAGIC ||
        header->version != AVIF_SEQUENCE_METADATA_VERSION ||
        header->total_size != extension_size ||
        extension_size < sizeof(*header) ||
        header->metadata_offset > header->total_size ||
        header->thumbnail_offset > header->total_size ||
        header->span_offset > header->total_size ||
        avif_sequence_metadata_hash(
            extension, header->total_size) != header->hash) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    *workspace = extension;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_index_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifdecSequenceIndexInfo *info,
    AvifdecError *error) {
    AvifSequenceApiValidationContext validation_context;
    AvifSequenceValidationCallbacks validation;
    AvifdecSequenceIndexInfo base_info;
    AvifdecMetadataResult metadata;
    AvifSequenceMetadataHeader layout;
    size_t metadata_required;
    AvifdecStatus status;

    avif_sequence_api_error_clear(error);
    if (info != 0) {
        avifdec_memory_fill(info, 0U, sizeof(*info));
    }
    if (info == 0) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    validation_context.data = (const unsigned char *)data;
    validation_context.size = size;
    validation =
        avif_sequence_api_validation_callbacks(&validation_context);
    status = avif_sequence_index_query(
        data, size, limits, &validation, &base_info, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_metadata_count(
        data, size, limits, &metadata, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_metadata_layout(
        &metadata, &metadata_required, &layout);
    if (status != AVIFDEC_OK ||
        !avifdec_size_add(
            base_info.workspace_required,
            metadata_required, &base_info.workspace_required)) {
        return avif_sequence_api_fail(
            error, status != AVIFDEC_OK ? status : AVIFDEC_OVERFLOW,
            0U, 0U);
    }
    *info = base_info;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_index_init(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    void *workspace,
    size_t workspace_size,
    AvifdecSequenceIndex *index,
    AvifdecSequenceIndexInfo *info,
    AvifdecError *error) {
    AvifSequenceApiValidationContext validation_context;
    AvifSequenceValidationCallbacks validation;
    AvifdecSequenceIndexInfo queried;
    AvifdecSequenceIndexInfo base_info;
    AvifdecMetadataResult metadata;
    AvifSequenceMetadataHeader layout;
    size_t metadata_required;
    size_t base_required;
    AvifdecStatus status;

    avif_sequence_api_error_clear(error);
    if (index == 0 || info == 0 ||
        (workspace == 0 && workspace_size != 0U)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    avifdec_memory_fill(index, 0U, sizeof(*index));
    avifdec_memory_fill(info, 0U, sizeof(*info));
    validation_context.data = (const unsigned char *)data;
    validation_context.size = size;
    validation =
        avif_sequence_api_validation_callbacks(&validation_context);
    status = avifdec_sequence_index_query(
        data, size, limits, &queried, error);
    *info = queried;
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_metadata_count(
        data, size, limits, &metadata, error);
    if (status != AVIFDEC_OK) return status;
    status = avif_sequence_metadata_layout(
        &metadata, &metadata_required, &layout);
    if (status != AVIFDEC_OK ||
        metadata_required > queried.workspace_required) {
        return avif_sequence_api_fail(
            error, status != AVIFDEC_OK ? status : AVIFDEC_OVERFLOW,
            0U, 0U);
    }
    base_required = queried.workspace_required - metadata_required;
    if (workspace == 0 || workspace_size < queried.workspace_required) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    status = avif_sequence_index_init(
        data, size, limits, &validation, workspace,
        base_required, index, &base_info, error);
    if (status != AVIFDEC_OK ||
        base_info.workspace_required != base_required) {
        avifdec_memory_fill(index, 0U, sizeof(*index));
        avifdec_memory_fill(
            workspace, 0U, queried.workspace_required);
        return status != AVIFDEC_OK
            ? status
            : avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    status = avif_sequence_metadata_fill(
        index, data, size, limits,
        (unsigned char *)workspace + base_required,
        metadata_required, &metadata, error);
    if (status != AVIFDEC_OK) {
        avifdec_memory_fill(index, 0U, sizeof(*index));
        avifdec_memory_fill(
            workspace, 0U, queried.workspace_required);
        return status;
    }
    status = avif_sequence_index_attach_extension(
        index, metadata_required, error);
    if (status != AVIFDEC_OK) {
        avifdec_memory_fill(index, 0U, sizeof(*index));
        avifdec_memory_fill(
            workspace, 0U, queried.workspace_required);
        return status;
    }
    *info = queried;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_track_query(
    const AvifdecSequenceIndex *index,
    size_t track_index,
    AvifdecSequenceTrackInfo *track,
    AvifdecError *error) {
    avif_sequence_api_error_clear(error);
    if (track != 0) {
        avifdec_memory_fill(track, 0U, sizeof(*track));
    }
    return avif_sequence_track_query(
        index, track_index, track, error);
}

AvifdecStatus avifdec_sequence_track_reference_query(
    const AvifdecSequenceIndex *index,
    size_t reference_index,
    AvifdecSequenceTrackReferenceInfo *reference,
    AvifdecError *error) {
    avif_sequence_api_error_clear(error);
    if (reference != 0) {
        avifdec_memory_fill(reference, 0U, sizeof(*reference));
    }
    return avif_sequence_track_reference_query(
        index, reference_index, reference, error);
}

AvifdecStatus avifdec_sequence_select(
    const AvifdecSequenceIndex *index,
    const AvifdecSequenceSelectOptions *options,
    AvifdecSequenceSelection *selection,
    AvifdecError *error) {
    avif_sequence_api_error_clear(error);
    if (selection != 0) {
        avifdec_memory_fill(selection, 0U, sizeof(*selection));
    }
    return avif_sequence_select(index, options, selection, error);
}

AvifdecStatus avifdec_sequence_presentation_query_ex(
    const AvifdecSequenceIndex *index,
    const AvifdecSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    AvifdecSequencePresentationInfo *presentation,
    AvifdecError *error) {
    AvifSequenceDecodeCallbacks callbacks =
        avif_sequence_api_decode_callbacks();
    AvifSequenceDecodePlan plan;
    AvifdecStatus status;

    avif_sequence_api_error_clear(error);
    if (presentation != 0) {
        avifdec_memory_fill(
            presentation, 0U, sizeof(*presentation));
    }
    if (presentation == 0 ||
        !avif_sequence_api_executor_valid(executor)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_sequence_decode_plan_query(
        index, selection, executor, presentation_index,
        &callbacks, &plan, error);
    if (status != AVIFDEC_OK) return status;
    *presentation = plan.presentation;
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_presentation_query(
    const AvifdecSequenceIndex *index,
    const AvifdecSequenceSelection *selection,
    size_t presentation_index,
    AvifdecSequencePresentationInfo *presentation,
    AvifdecError *error) {
    return avifdec_sequence_presentation_query_ex(
        index, selection, 0, presentation_index,
        presentation, error);
}

AvifdecStatus avifdec_sequence_decode_presentation_ex(
    const AvifdecSequenceIndex *index,
    const AvifdecSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecSequencePresentationInfo *presentation,
    AvifdecError *error) {
    AvifSequenceDecodeCallbacks callbacks =
        avif_sequence_api_decode_callbacks();

    avif_sequence_api_error_clear(error);
    if (presentation != 0) {
        avifdec_memory_fill(
            presentation, 0U, sizeof(*presentation));
    }
    if (trace != 0) {
        avifdec_memory_fill(trace, 0U, sizeof(*trace));
    }
    if (!avif_sequence_api_executor_valid(executor)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    return avif_sequence_decode_presentation(
        index, selection, executor, presentation_index,
        &callbacks, workspace, workspace_size, image,
        trace, presentation, error);
}

AvifdecStatus avifdec_sequence_decode_presentation(
    const AvifdecSequenceIndex *index,
    const AvifdecSequenceSelection *selection,
    size_t presentation_index,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecSequencePresentationInfo *presentation,
    AvifdecError *error) {
    return avifdec_sequence_decode_presentation_ex(
        index, selection, 0, presentation_index,
        workspace, workspace_size, image, trace,
        presentation, error);
}

static int avif_sequence_metadata_row_selected(
    uint32_t track_id,
    AvifdecMetadataScope scope,
    uint32_t target_track_id) {
    return track_id == 0U ||
           (scope == AVIFDEC_METADATA_SCOPE_TRACK &&
            target_track_id == track_id);
}

static AvifdecStatus avif_sequence_metadata_validate_row(
    size_t span_index,
    size_t span_count,
    size_t payload_size,
    const AvifdecSpan *spans,
    size_t total_span_count,
    AvifdecError *error) {
    size_t span_end;
    size_t computed_size = 0U;
    size_t index;

    if (!avifdec_size_add(span_index, span_count, &span_end)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OVERFLOW, 0U, 0U);
    }
    if (span_end > total_span_count) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    for (index = span_index; index < span_end; ++index) {
        if ((spans[index].data == 0 && spans[index].size != 0U) ||
            !avifdec_size_add(
                computed_size, spans[index].size, &computed_size)) {
            return avif_sequence_api_fail(
                error,
                spans[index].data == 0
                    ? AVIFDEC_INVALID_DATA : AVIFDEC_OVERFLOW,
                spans[index].file_offset, 0U);
        }
    }
    if (computed_size != payload_size) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_DATA, 0U, 0U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_metadata_query(
    const AvifdecSequenceIndex *index,
    uint32_t track_id,
    AvifdecMetadataInfo *metadata,
    size_t metadata_capacity,
    AvifdecThumbnailInfo *thumbnails,
    size_t thumbnail_capacity,
    AvifdecSpan *spans,
    size_t span_capacity,
    AvifdecMetadataResult *result,
    AvifdecError *error) {
    const unsigned char *workspace;
    const AvifdecMetadataInfo *cached_metadata;
    const AvifdecThumbnailInfo *cached_thumbnails;
    const AvifdecSpan *cached_spans;
    AvifSequenceMetadataHeader header;
    AvifdecLimits limits;
    size_t metadata_bytes;
    size_t thumbnail_bytes;
    size_t span_bytes;
    size_t index_value;
    size_t output_metadata_count = 0U;
    size_t output_thumbnail_count = 0U;
    size_t output_span_count = 0U;
    AvifdecStatus status;

    avif_sequence_api_error_clear(error);
    if (result != 0) {
        avifdec_memory_fill(result, 0U, sizeof(*result));
    }
    if (result == 0 ||
        (metadata == 0 && metadata_capacity != 0U) ||
        (thumbnails == 0 && thumbnail_capacity != 0U) ||
        (spans == 0 && span_capacity != 0U)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    status = avif_sequence_metadata_open(
        index, &workspace, &header, &limits, error);
    if (status != AVIFDEC_OK) return status;
    if (!avifdec_size_multiply(
            header.metadata_count, sizeof(*cached_metadata),
            &metadata_bytes) ||
        !avifdec_size_multiply(
            header.thumbnail_count, sizeof(*cached_thumbnails),
            &thumbnail_bytes) ||
        !avifdec_size_multiply(
            header.span_count, sizeof(*cached_spans), &span_bytes) ||
        header.metadata_offset > header.total_size ||
        metadata_bytes >
            header.total_size - header.metadata_offset ||
        header.thumbnail_offset > header.total_size ||
        thumbnail_bytes >
            header.total_size - header.thumbnail_offset ||
        header.span_offset > header.total_size ||
        span_bytes > header.total_size - header.span_offset) {
        return avif_sequence_api_fail(
            error, AVIFDEC_INVALID_ARGUMENT, 0U, 0U);
    }
    if (track_id != 0U) {
        int found = 0;

        for (index_value = 0U;
             index_value < limits.max_tracks;
             ++index_value) {
            AvifdecSequenceTrackInfo track;
            AvifdecError local_error;

            status = avif_sequence_track_query(
                index, index_value, &track, &local_error);
            if (status == AVIFDEC_INVALID_ARGUMENT) break;
            if (status != AVIFDEC_OK) return status;
            if (track.track_id == track_id) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_ARGUMENT, track_id,
                AVIFDEC_FOURCC('t', 'r', 'a', 'k'));
        }
    }
    cached_metadata = (const AvifdecMetadataInfo *)(
        workspace + header.metadata_offset);
    cached_thumbnails = (const AvifdecThumbnailInfo *)(
        workspace + header.thumbnail_offset);
    cached_spans = (const AvifdecSpan *)(
        workspace + header.span_offset);
    for (index_value = 0U;
         index_value < header.metadata_count;
         ++index_value) {
        const AvifdecMetadataInfo *row =
            &cached_metadata[index_value];

        if ((row->flags & AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE) == 0U ||
            (row->scope != AVIFDEC_METADATA_SCOPE_UNSCOPED &&
             row->scope != AVIFDEC_METADATA_SCOPE_TRACK)) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        status = avif_sequence_metadata_validate_row(
            row->span_index, row->span_count, row->payload_size,
            cached_spans, header.span_count, error);
        if (status != AVIFDEC_OK) return status;
        if (!avif_sequence_metadata_row_selected(
                track_id, row->scope, row->target_track_id)) {
            continue;
        }
        if (output_metadata_count == SIZE_MAX ||
            !avifdec_size_add(
                output_span_count, row->span_count,
                &output_span_count)) {
            return avif_sequence_api_fail(
                error, AVIFDEC_OVERFLOW, 0U, 0U);
        }
        ++output_metadata_count;
    }
    for (index_value = 0U;
         index_value < header.thumbnail_count;
         ++index_value) {
        const AvifdecThumbnailInfo *row =
            &cached_thumbnails[index_value];

        if ((row->flags & AVIFDEC_METADATA_FLAG_SEQUENCE_WIDE) == 0U) {
            return avif_sequence_api_fail(
                error, AVIFDEC_INVALID_DATA, 0U, 0U);
        }
        status = avif_sequence_metadata_validate_row(
            row->span_index, row->span_count, row->payload_size,
            cached_spans, header.span_count, error);
        if (status != AVIFDEC_OK) return status;
        if (track_id != 0U && row->target_track_id != track_id) {
            continue;
        }
        if (output_thumbnail_count == SIZE_MAX ||
            !avifdec_size_add(
                output_span_count, row->span_count,
                &output_span_count)) {
            return avif_sequence_api_fail(
                error, AVIFDEC_OVERFLOW, 0U, 0U);
        }
        ++output_thumbnail_count;
    }
    result->primary_item_id = header.primary_item_id;
    result->metadata_count = output_metadata_count;
    result->thumbnail_count = output_thumbnail_count;
    result->span_count = output_span_count;
    if (metadata == 0 && metadata_capacity == 0U &&
        thumbnails == 0 && thumbnail_capacity == 0U &&
        spans == 0 && span_capacity == 0U) {
        return AVIFDEC_OK;
    }
    if (metadata_capacity < output_metadata_count ||
        thumbnail_capacity < output_thumbnail_count ||
        span_capacity < output_span_count ||
        (output_metadata_count != 0U && metadata == 0) ||
        (output_thumbnail_count != 0U && thumbnails == 0) ||
        (output_span_count != 0U && spans == 0)) {
        return avif_sequence_api_fail(
            error, AVIFDEC_OUT_OF_MEMORY, 0U, 0U);
    }
    output_metadata_count = 0U;
    output_thumbnail_count = 0U;
    output_span_count = 0U;
    for (index_value = 0U;
         index_value < header.metadata_count;
         ++index_value) {
        const AvifdecMetadataInfo *row =
            &cached_metadata[index_value];
        size_t span_index;

        if (!avif_sequence_metadata_row_selected(
                track_id, row->scope, row->target_track_id)) {
            continue;
        }
        metadata[output_metadata_count] = *row;
        metadata[output_metadata_count].span_index =
            output_span_count;
        for (span_index = 0U;
             span_index < row->span_count;
             ++span_index) {
            spans[output_span_count++] =
                cached_spans[row->span_index + span_index];
        }
        ++output_metadata_count;
    }
    for (index_value = 0U;
         index_value < header.thumbnail_count;
         ++index_value) {
        const AvifdecThumbnailInfo *row =
            &cached_thumbnails[index_value];
        size_t span_index;

        if (track_id != 0U &&
            row->target_track_id != track_id) {
            continue;
        }
        thumbnails[output_thumbnail_count] = *row;
        thumbnails[output_thumbnail_count].span_index =
            output_span_count;
        for (span_index = 0U;
             span_index < row->span_count;
             ++span_index) {
            spans[output_span_count++] =
                cached_spans[row->span_index + span_index];
        }
        ++output_thumbnail_count;
    }
    return AVIFDEC_OK;
}
