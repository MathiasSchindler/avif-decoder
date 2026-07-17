#include "avifdec.h"
#include "av1.h"
#include "av1_bitstream.h"
#include "base.h"
#include "bmff.h"

#define AVIF_SEQ_MAX_TRACKS 8U
#define AVIF_SEQ_MAX_FRAMES AVIFDEC_DEFAULT_MAX_FRAMES
#define AVIF_SEQ_MAX_DATA_BOXES 16U
#define AVIF_SEQ_MAX_EDITS 16U
#define AVIF_SEQ_MAX_STSD_ENTRIES 4U
#define AVIF_SEQ_MAX_STSC_ENTRIES AVIF_SEQ_MAX_FRAMES
#define AVIF_SEQ_MAX_STTS_ENTRIES AVIF_SEQ_MAX_FRAMES
#define AVIF_SEQ_MAX_CTTS_ENTRIES AVIF_SEQ_MAX_FRAMES
#define AVIF_SEQ_MAX_STSS_ENTRIES AVIF_SEQ_MAX_FRAMES
#define AVIF_SEQ_MAX_CHUNKS AVIF_SEQ_MAX_FRAMES
#define AVIF_SEQ_WORKSPACE_BASE_ALIGNMENT 16U
#define AVIF_SEQ_MAX_DECODE_SPANS (AVIF_SEQ_MAX_FRAMES + 1U)

typedef struct {
    AvifdecBmffBox trak;
    AvifdecBmffBox tkhd;
    AvifdecBmffBox tref;
    AvifdecBmffBox edts;
    AvifdecBmffBox elst;
    AvifdecBmffBox mdia;
    AvifdecBmffBox mdhd;
    AvifdecBmffBox hdlr;
    AvifdecBmffBox minf;
    AvifdecBmffBox stbl;
    AvifdecBmffBox stsd;
    AvifdecBmffBox stts;
    AvifdecBmffBox ctts;
    AvifdecBmffBox stsc;
    AvifdecBmffBox stsz;
    AvifdecBmffBox stz2;
    AvifdecBmffBox stco;
    AvifdecBmffBox co64;
    AvifdecBmffBox stss;

    uint32_t track_id;
    uint32_t handler_type;
    uint8_t matrix_identity;

    uint32_t media_timescale;
    uint64_t media_duration;

    uint8_t has_edit;
    uint64_t edit_segment_duration;
    int64_t edit_media_time;
    uint32_t edit_rate;

    /* stsd/av01 VisualSampleEntry state. */
    uint8_t seen_entry;
    uint32_t entry_width;
    uint32_t entry_height;
    uint8_t seen_av1c;
    uint8_t profile;
    uint8_t level;
    uint8_t tier;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
    const unsigned char *config_obus;
    size_t config_obus_size;
    uint8_t has_nclx;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
    uint8_t seen_icc;
    const unsigned char *icc_data;
    size_t icc_size;
    uint8_t transform_flags;
    AvifdecCleanAperture clean_aperture;
    uint32_t pixel_aspect_h_spacing;
    uint32_t pixel_aspect_v_spacing;
    uint8_t is_alpha_entry;

    /* tref state. */
    uint8_t has_auxl;
    uint32_t auxl_target;
    uint8_t has_prem;
    uint32_t prem_target;

    /* Sample table, bounded by AVIF_SEQ_MAX_FRAMES. */
    size_t sample_count;
    uint64_t dts[AVIF_SEQ_MAX_FRAMES];
    uint32_t sample_duration[AVIF_SEQ_MAX_FRAMES];
    int64_t cts_offset[AVIF_SEQ_MAX_FRAMES];
    uint64_t sample_offset[AVIF_SEQ_MAX_FRAMES];
    uint32_t sample_size[AVIF_SEQ_MAX_FRAMES];
    uint8_t sample_sync[AVIF_SEQ_MAX_FRAMES];
} AvifSeqTrack;

typedef struct {
    const unsigned char *data;
    size_t size;
    AvifdecLimits limits;
    AvifdecError *error;
    AvifdecBmffBox moov;
    AvifdecBmffBox mvhd;
    AvifdecBmffBox data_boxes[AVIF_SEQ_MAX_DATA_BOXES];
    size_t data_box_count;
    uint32_t movie_timescale;
    uint64_t movie_duration;
    uint8_t mvhd_version1;
    AvifSeqTrack tracks[AVIF_SEQ_MAX_TRACKS];
    size_t track_count;
    int failed;
} AvifSeqContext;

typedef struct {
    const unsigned char *data;
    size_t size;
    size_t position;
    size_t base_offset;
} AvifSeqBoxIter;

/*
 * Mirrors avif_executor_valid()/avif_executor_width() in avif.c: a null
 * executor means "run serially", and any non-null executor must present a
 * usable parallel_for and an in-range worker_count. Each translation unit
 * that accepts an AvifdecExecutor directly performs this same validation
 * locally rather than sharing a helper across files.
 */
static int avifseq_executor_valid(const AvifdecExecutor *executor) {
    return executor == 0 ||
           (executor->parallel_for != 0 &&
            executor->worker_count != 0U &&
            executor->worker_count <= AVIFDEC_EXECUTOR_MAX_WORKERS);
}

static size_t avifseq_executor_width(const AvifdecExecutor *executor) {
    return executor == 0 ? 1U : executor->worker_count;
}

static AvifdecStatus avifseq_fail(AvifSeqContext *context,
                                  AvifdecStatus status,
                                  size_t offset,
                                  uint32_t box_type) {
    if (context->error != 0 && context->error->status == AVIFDEC_OK) {
        context->error->status = status;
        context->error->offset = offset;
        context->error->context = box_type;
    }
    context->failed = 1;
    return status;
}

static int avifseq_box_is_set(const AvifdecBmffBox *box) {
    return box->size != 0U;
}

static int avifseq_range_contains(const AvifdecBmffBox *box,
                                  const AvifdecBmffBox *ancestor) {
    size_t ancestor_end;
    size_t box_end;

    if (!avifseq_box_is_set(ancestor)) return 0;
    if (!avifdec_size_add(ancestor->offset, ancestor->size, &ancestor_end) ||
        !avifdec_size_add(box->offset, box->size, &box_end)) {
        return 0;
    }
    return box->offset >= ancestor->payload_offset && box_end <= ancestor_end;
}

static int avifseq_within(const AvifdecBmffBox *box,
                          const AvifdecBmffBox *parent,
                          size_t relative_depth) {
    if (!avifseq_box_is_set(parent) ||
        box->depth != parent->depth + relative_depth) {
        return 0;
    }
    return avifseq_range_contains(box, parent);
}

static int avifseq_text_equal(const unsigned char *bytes,
                              size_t length,
                              const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        if (index >= length || bytes[index] != (unsigned char)text[index]) {
            return 0;
        }
        ++index;
    }
    return index == length;
}

static void avifseq_box_iter_init(AvifSeqBoxIter *iter,
                                  const unsigned char *data,
                                  size_t size,
                                  size_t base_offset) {
    iter->data = data;
    iter->size = size;
    iter->position = 0U;
    iter->base_offset = base_offset;
}

/*
 * Manually walks a leaf-level box sequence (used for 'stsd' sample entry
 * children and 'tref' reference-type children), which the generic BMFF
 * walker does not descend into.
 */
static AvifdecStatus avifseq_box_iter_next(AvifSeqContext *context,
                                           AvifSeqBoxIter *iter,
                                           uint32_t *type,
                                           size_t *box_offset,
                                           size_t *payload_offset,
                                           size_t *payload_size,
                                           int *done) {
    uint32_t size32;
    size_t box_end;

    *done = 0;
    if (iter->position >= iter->size) {
        *done = 1;
        return AVIFDEC_OK;
    }
    if (iter->size - iter->position < 8U) {
        return avifseq_fail(context, AVIFDEC_TRUNCATED,
                            iter->base_offset + iter->position, 0U);
    }
    size32 = avifdec_load_u32be(iter->data + iter->position);
    *type = avifdec_load_u32be(iter->data + iter->position + 4U);
    *box_offset = iter->base_offset + iter->position;
    if (size32 < 8U) {
        return avifseq_fail(context, AVIFDEC_UNSUPPORTED, *box_offset, *type);
    }
    if (!avifdec_size_add(iter->position, size32, &box_end) ||
        box_end > iter->size) {
        return avifseq_fail(context, AVIFDEC_TRUNCATED, *box_offset, *type);
    }
    *payload_offset = iter->base_offset + iter->position + 8U;
    *payload_size = size32 - 8U;
    iter->position = box_end;
    return AVIFDEC_OK;
}

static void avifseq_collect_box(const AvifdecBmffBox *box, void *user_data) {
    AvifSeqContext *context = (AvifSeqContext *)user_data;
    uint32_t type = box->type;
    AvifSeqTrack *track;

    if (context->failed) return;
    if (box->depth == 0U && type == AVIFDEC_FOURCC('m', 'o', 'o', 'v')) {
        if (avifseq_box_is_set(&context->moov)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->moov = *box;
        return;
    }
    if (box->depth == 0U && type == AVIFDEC_FOURCC('m', 'd', 'a', 't')) {
        if (context->data_box_count >= AVIF_SEQ_MAX_DATA_BOXES) {
            (void)avifseq_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        context->data_boxes[context->data_box_count++] = *box;
        return;
    }
    if (type == AVIFDEC_FOURCC('m', 'v', 'h', 'd') &&
        avifseq_within(box, &context->moov, 1U)) {
        if (avifseq_box_is_set(&context->mvhd)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        context->mvhd = *box;
        return;
    }
    if (type == AVIFDEC_FOURCC('t', 'r', 'a', 'k') &&
        avifseq_within(box, &context->moov, 1U)) {
        if (context->track_count >= AVIF_SEQ_MAX_TRACKS) {
            (void)avifseq_fail(context, AVIFDEC_LIMIT_EXCEEDED, box->offset, type);
            return;
        }
        avifdec_memory_fill(&context->tracks[context->track_count], 0U,
                            sizeof(context->tracks[context->track_count]));
        context->tracks[context->track_count].trak = *box;
        ++context->track_count;
        return;
    }
    if (context->track_count == 0U) return;
    track = &context->tracks[context->track_count - 1U];
    if (!avifseq_range_contains(box, &track->trak)) return;

    if (type == AVIFDEC_FOURCC('t', 'k', 'h', 'd') &&
        avifseq_within(box, &track->trak, 1U)) {
        if (avifseq_box_is_set(&track->tkhd)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->tkhd = *box;
    } else if (type == AVIFDEC_FOURCC('t', 'r', 'e', 'f') &&
               avifseq_within(box, &track->trak, 1U)) {
        if (avifseq_box_is_set(&track->tref)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->tref = *box;
    } else if (type == AVIFDEC_FOURCC('e', 'd', 't', 's') &&
               avifseq_within(box, &track->trak, 1U)) {
        if (avifseq_box_is_set(&track->edts)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->edts = *box;
    } else if (type == AVIFDEC_FOURCC('m', 'd', 'i', 'a') &&
               avifseq_within(box, &track->trak, 1U)) {
        if (avifseq_box_is_set(&track->mdia)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->mdia = *box;
    } else if (type == AVIFDEC_FOURCC('e', 'l', 's', 't') &&
               avifseq_within(box, &track->edts, 1U)) {
        if (avifseq_box_is_set(&track->elst)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->elst = *box;
    } else if (type == AVIFDEC_FOURCC('m', 'd', 'h', 'd') &&
               avifseq_within(box, &track->mdia, 1U)) {
        if (avifseq_box_is_set(&track->mdhd)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->mdhd = *box;
    } else if (type == AVIFDEC_FOURCC('h', 'd', 'l', 'r') &&
               avifseq_within(box, &track->mdia, 1U)) {
        if (avifseq_box_is_set(&track->hdlr)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->hdlr = *box;
    } else if (type == AVIFDEC_FOURCC('m', 'i', 'n', 'f') &&
               avifseq_within(box, &track->mdia, 1U)) {
        if (avifseq_box_is_set(&track->minf)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->minf = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 'b', 'l') &&
               avifseq_within(box, &track->minf, 1U)) {
        if (avifseq_box_is_set(&track->stbl)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stbl = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 's', 'd') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stsd)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stsd = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 't', 's') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stts)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stts = *box;
    } else if (type == AVIFDEC_FOURCC('c', 't', 't', 's') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->ctts)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->ctts = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 's', 'c') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stsc)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stsc = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 's', 'z') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stsz)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stsz = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 'z', '2') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stz2)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stz2 = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 'c', 'o') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stco)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stco = *box;
    } else if (type == AVIFDEC_FOURCC('c', 'o', '6', '4') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->co64)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->co64 = *box;
    } else if (type == AVIFDEC_FOURCC('s', 't', 's', 's') &&
               avifseq_within(box, &track->stbl, 1U)) {
        if (avifseq_box_is_set(&track->stss)) {
            (void)avifseq_fail(context, AVIFDEC_INVALID_DATA, box->offset, type);
            return;
        }
        track->stss = *box;
    }
}

static AvifdecStatus avifseq_parse_mvhd(AvifSeqContext *context) {
    AvifdecByteReader reader;
    uint8_t version;

    if (!avifseq_box_is_set(&context->mvhd)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, context->moov.offset,
                            AVIFDEC_FOURCC('m', 'v', 'h', 'd'));
    }
    avifdec_byte_reader_init(&reader, context->data + context->mvhd.payload_offset,
                             context->mvhd.payload_size, context->mvhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    (void)avifdec_byte_reader_skip(&reader, 3U);
    if (version != 0U && version != 1U) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, context->mvhd.offset, context->mvhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        context->movie_timescale = avifdec_byte_reader_u32be(&reader);
        context->movie_duration = avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        context->movie_timescale = avifdec_byte_reader_u32be(&reader);
        context->movie_duration = (uint64_t)avifdec_byte_reader_u32be(&reader);
    }
    context->mvhd_version1 = version;
    if (reader.status != AVIFDEC_OK || context->movie_timescale == 0U) {
        return avifseq_fail(context, reader.status != AVIFDEC_OK ? reader.status : AVIFDEC_INVALID_DATA,
                            context->mvhd.offset, context->mvhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_tkhd(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint8_t version;
    size_t matrix_index;
    static const uint32_t identity_matrix[9] = {
        0x00010000U, 0U, 0U,
        0U, 0x00010000U, 0U,
        0U, 0U, 0x40000000U
    };

    if (!avifseq_box_is_set(&track->tkhd)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->trak.offset,
                            AVIFDEC_FOURCC('t', 'k', 'h', 'd'));
    }
    avifdec_byte_reader_init(&reader, context->data + track->tkhd.payload_offset,
                             track->tkhd.payload_size, track->tkhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    (void)avifdec_byte_reader_skip(&reader, 3U);
    if (version != 0U && version != 1U) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->tkhd.offset, track->tkhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        track->track_id = avifdec_byte_reader_u32be(&reader);
        (void)avifdec_byte_reader_skip(&reader, 4U);
        (void)avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        track->track_id = avifdec_byte_reader_u32be(&reader);
        (void)avifdec_byte_reader_skip(&reader, 4U);
        (void)avifdec_byte_reader_u32be(&reader);
    }
    (void)avifdec_byte_reader_skip(&reader, 8U);
    (void)avifdec_byte_reader_u16be(&reader);
    (void)avifdec_byte_reader_u16be(&reader);
    (void)avifdec_byte_reader_u16be(&reader);
    (void)avifdec_byte_reader_skip(&reader, 2U);
    track->matrix_identity = 1U;
    for (matrix_index = 0U; matrix_index < 9U; ++matrix_index) {
        uint32_t value = avifdec_byte_reader_u32be(&reader);
        if (value != identity_matrix[matrix_index]) track->matrix_identity = 0U;
    }
    (void)avifdec_byte_reader_u32be(&reader);
    (void)avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK || track->track_id == 0U) {
        return avifseq_fail(context, reader.status != AVIFDEC_OK ? reader.status : AVIFDEC_INVALID_DATA,
                            track->tkhd.offset, track->tkhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_mdhd(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint8_t version;

    if (!avifseq_box_is_set(&track->mdhd)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->mdia.offset,
                            AVIFDEC_FOURCC('m', 'd', 'h', 'd'));
    }
    avifdec_byte_reader_init(&reader, context->data + track->mdhd.payload_offset,
                             track->mdhd.payload_size, track->mdhd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    (void)avifdec_byte_reader_skip(&reader, 3U);
    if (version != 0U && version != 1U) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->mdhd.offset, track->mdhd.type);
    }
    if (version == 1U) {
        (void)avifdec_byte_reader_skip(&reader, 16U);
        track->media_timescale = avifdec_byte_reader_u32be(&reader);
        track->media_duration = avifdec_byte_reader_u64be(&reader);
    } else {
        (void)avifdec_byte_reader_skip(&reader, 8U);
        track->media_timescale = avifdec_byte_reader_u32be(&reader);
        track->media_duration = (uint64_t)avifdec_byte_reader_u32be(&reader);
    }
    if (reader.status != AVIFDEC_OK || track->media_timescale == 0U) {
        return avifseq_fail(context, reader.status != AVIFDEC_OK ? reader.status : AVIFDEC_INVALID_DATA,
                            track->mdhd.offset, track->mdhd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_hdlr(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;

    if (!avifseq_box_is_set(&track->hdlr)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->mdia.offset,
                            AVIFDEC_FOURCC('h', 'd', 'l', 'r'));
    }
    avifdec_byte_reader_init(&reader, context->data + track->hdlr.payload_offset,
                             track->hdlr.payload_size, track->hdlr.payload_offset);
    (void)avifdec_byte_reader_u32be(&reader);
    (void)avifdec_byte_reader_u32be(&reader);
    track->handler_type = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avifseq_fail(context, reader.status, track->hdlr.offset, track->hdlr.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_elst(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t entry_count;

    if (!avifseq_box_is_set(&track->edts)) {
        track->has_edit = 0U;
        return AVIFDEC_OK;
    }
    if (!avifseq_box_is_set(&track->elst)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->edts.offset,
                            AVIFDEC_FOURCC('e', 'l', 's', 't'));
    }
    avifdec_byte_reader_init(&reader, context->data + track->elst.payload_offset,
                             track->elst.payload_size, track->elst.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    (void)avifdec_byte_reader_skip(&reader, 3U);
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK || (version != 0U && version != 1U)) {
        return avifseq_fail(context, reader.status != AVIFDEC_OK ? reader.status : AVIFDEC_INVALID_DATA,
                            track->elst.offset, track->elst.type);
    }
    if (entry_count > AVIF_SEQ_MAX_EDITS) {
        return avifseq_fail(context, AVIFDEC_LIMIT_EXCEEDED, track->elst.offset, track->elst.type);
    }
    if (entry_count != 1U) {
        return avifseq_fail(context, AVIFDEC_UNSUPPORTED, track->elst.offset, track->elst.type);
    }
    if (version == 1U) {
        track->edit_segment_duration = avifdec_byte_reader_u64be(&reader);
        track->edit_media_time = (int64_t)avifdec_byte_reader_u64be(&reader);
    } else {
        track->edit_segment_duration = (uint64_t)avifdec_byte_reader_u32be(&reader);
        track->edit_media_time = (int64_t)(int32_t)avifdec_byte_reader_u32be(&reader);
    }
    track->edit_rate = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK) {
        return avifseq_fail(context, reader.status, track->elst.offset, track->elst.type);
    }
    if (track->edit_media_time != 0 || track->edit_rate != 0x00010000U) {
        return avifseq_fail(context, AVIFDEC_UNSUPPORTED, track->elst.offset, track->elst.type);
    }
    track->has_edit = 1U;
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_tref(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifSeqBoxIter iter;

    track->has_auxl = 0U;
    track->has_prem = 0U;
    if (!avifseq_box_is_set(&track->tref)) return AVIFDEC_OK;
    avifseq_box_iter_init(&iter, context->data + track->tref.payload_offset,
                          track->tref.payload_size, track->tref.payload_offset);
    for (;;) {
        uint32_t type;
        size_t box_offset;
        size_t payload_offset;
        size_t payload_size;
        int done;
        AvifdecStatus status = avifseq_box_iter_next(context, &iter, &type, &box_offset,
                                                     &payload_offset, &payload_size, &done);

        if (status != AVIFDEC_OK) return status;
        if (done) break;
        if (type == AVIFDEC_FOURCC('a', 'u', 'x', 'l') || type == AVIFDEC_FOURCC('p', 'r', 'e', 'm')) {
            uint32_t track_id;

            if (payload_size != 4U) {
                return avifseq_fail(context, AVIFDEC_UNSUPPORTED, box_offset, type);
            }
            track_id = avifdec_load_u32be(context->data + payload_offset);
            if (type == AVIFDEC_FOURCC('a', 'u', 'x', 'l')) {
                if (track->has_auxl) {
                    return avifseq_fail(context, AVIFDEC_INVALID_DATA, box_offset, type);
                }
                track->has_auxl = 1U;
                track->auxl_target = track_id;
            } else {
                if (track->has_prem) {
                    return avifseq_fail(context, AVIFDEC_INVALID_DATA, box_offset, type);
                }
                track->has_prem = 1U;
                track->prem_target = track_id;
            }
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_stsd(AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint8_t version;
    uint32_t entry_count;
    AvifSeqBoxIter entry_iter;
    uint32_t entry_type;
    size_t entry_box_offset;
    size_t entry_payload_offset;
    size_t entry_payload_size;
    int done;
    AvifdecStatus status;
    AvifdecByteReader entry_reader;
    AvifSeqBoxIter child_iter;
    int seen_av1c = 0;
    int seen_nclx = 0;
    int seen_pasp = 0;
    int seen_clap = 0;
    int seen_auxi = 0;

    if (!avifseq_box_is_set(&track->stsd)) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->stbl.offset,
                            AVIFDEC_FOURCC('s', 't', 's', 'd'));
    }
    avifdec_byte_reader_init(&reader, context->data + track->stsd.payload_offset,
                             track->stsd.payload_size, track->stsd.payload_offset);
    version = avifdec_byte_reader_u8(&reader);
    (void)avifdec_byte_reader_skip(&reader, 3U);
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (reader.status != AVIFDEC_OK || version != 0U) {
        return avifseq_fail(context, reader.status != AVIFDEC_OK ? reader.status : AVIFDEC_INVALID_DATA,
                            track->stsd.offset, track->stsd.type);
    }
    if (entry_count > AVIF_SEQ_MAX_STSD_ENTRIES) {
        return avifseq_fail(context, AVIFDEC_LIMIT_EXCEEDED, track->stsd.offset, track->stsd.type);
    }
    if (entry_count != 1U) {
        return avifseq_fail(context, AVIFDEC_UNSUPPORTED, track->stsd.offset, track->stsd.type);
    }
    avifseq_box_iter_init(&entry_iter, context->data + avifdec_byte_reader_offset(&reader),
                          avifdec_byte_reader_remaining(&reader), avifdec_byte_reader_offset(&reader));
    status = avifseq_box_iter_next(context, &entry_iter, &entry_type, &entry_box_offset,
                                   &entry_payload_offset, &entry_payload_size, &done);
    if (status != AVIFDEC_OK) return status;
    if (done || entry_type != AVIFDEC_FOURCC('a', 'v', '0', '1') || entry_payload_size < 78U) {
        return avifseq_fail(context, AVIFDEC_UNSUPPORTED, entry_box_offset, entry_type);
    }
    avifdec_byte_reader_init(&entry_reader, context->data + entry_payload_offset,
                             entry_payload_size, entry_payload_offset);
    (void)avifdec_byte_reader_skip(&entry_reader, 6U);
    (void)avifdec_byte_reader_u16be(&entry_reader);
    (void)avifdec_byte_reader_skip(&entry_reader, 16U);
    track->entry_width = avifdec_byte_reader_u16be(&entry_reader);
    track->entry_height = avifdec_byte_reader_u16be(&entry_reader);
    (void)avifdec_byte_reader_skip(&entry_reader, 8U);
    (void)avifdec_byte_reader_u32be(&entry_reader);
    (void)avifdec_byte_reader_u16be(&entry_reader);
    (void)avifdec_byte_reader_skip(&entry_reader, 32U);
    (void)avifdec_byte_reader_u16be(&entry_reader);
    (void)avifdec_byte_reader_u16be(&entry_reader);
    if (entry_reader.status != AVIFDEC_OK || track->entry_width == 0U || track->entry_height == 0U) {
        return avifseq_fail(context,
                            entry_reader.status != AVIFDEC_OK ? entry_reader.status : AVIFDEC_INVALID_DATA,
                            entry_box_offset, entry_type);
    }
    track->seen_entry = 1U;
    avifseq_box_iter_init(&child_iter, context->data + entry_payload_offset + 78U,
                          entry_payload_size - 78U, entry_payload_offset + 78U);
    for (;;) {
        uint32_t child_type;
        size_t child_box_offset;
        size_t child_payload_offset;
        size_t child_payload_size;
        const unsigned char *payload;

        status = avifseq_box_iter_next(context, &child_iter, &child_type, &child_box_offset,
                                       &child_payload_offset, &child_payload_size, &done);
        if (status != AVIFDEC_OK) return status;
        if (done) break;
        payload = context->data + child_payload_offset;
        if (child_type == AVIFDEC_FOURCC('a', 'v', '1', 'C')) {
            if (seen_av1c || child_payload_size < 4U || payload[0] != 0x81U ||
                (payload[3] & 0xe0U) != 0U) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            track->profile = payload[1] >> 5;
            track->level = payload[1] & 31U;
            track->tier = payload[2] >> 7;
            track->bit_depth = (payload[2] & 0x40U) == 0U ? 8U
                              : (payload[2] & 0x20U) == 0U ? 10U : 12U;
            track->monochrome = (uint8_t)((payload[2] >> 4) & 1U);
            track->subsampling_x = (uint8_t)((payload[2] >> 3) & 1U);
            track->subsampling_y = (uint8_t)((payload[2] >> 2) & 1U);
            track->chroma_sample_position = (uint8_t)(payload[2] & 3U);
            if (child_payload_size > 4U) {
                track->config_obus = payload + 4U;
                track->config_obus_size = child_payload_size - 4U;
            }
            seen_av1c = 1;
            track->seen_av1c = 1U;
        } else if (child_type == AVIFDEC_FOURCC('c', 'o', 'l', 'r')) {
            uint32_t color_type;

            if (child_payload_size < 4U) {
                return avifseq_fail(context, AVIFDEC_TRUNCATED, child_box_offset, child_type);
            }
            color_type = avifdec_load_u32be(payload);
            if (color_type == AVIFDEC_FOURCC('n', 'c', 'l', 'x')) {
                if (seen_nclx || child_payload_size != 11U || (payload[10] & 0x7fU) != 0U) {
                    return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
                }
                track->color_primaries = avifdec_load_u16be(payload + 4U);
                track->transfer_characteristics = avifdec_load_u16be(payload + 6U);
                track->matrix_coefficients = avifdec_load_u16be(payload + 8U);
                track->color_range = payload[10] >> 7;
                track->has_nclx = 1U;
                seen_nclx = 1;
            } else if (color_type == AVIFDEC_FOURCC('r', 'I', 'C', 'C') ||
                       color_type == AVIFDEC_FOURCC('p', 'r', 'o', 'f')) {
                if (track->seen_icc) {
                    return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
                }
                track->icc_data = payload + 4U;
                track->icc_size = child_payload_size - 4U;
                track->seen_icc = 1U;
            }
        } else if (child_type == AVIFDEC_FOURCC('p', 'a', 's', 'p')) {
            if (seen_pasp || child_payload_size != 8U) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            track->pixel_aspect_h_spacing = avifdec_load_u32be(payload);
            track->pixel_aspect_v_spacing = avifdec_load_u32be(payload + 4U);
            if (track->pixel_aspect_h_spacing == 0U || track->pixel_aspect_v_spacing == 0U) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            track->transform_flags |= AVIFDEC_TRANSFORM_PASP;
            seen_pasp = 1;
        } else if (child_type == AVIFDEC_FOURCC('c', 'l', 'a', 'p')) {
            if (seen_clap || child_payload_size != 32U) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            track->clean_aperture.width_n = avifdec_load_u32be(payload);
            track->clean_aperture.width_d = avifdec_load_u32be(payload + 4U);
            track->clean_aperture.height_n = avifdec_load_u32be(payload + 8U);
            track->clean_aperture.height_d = avifdec_load_u32be(payload + 12U);
            track->clean_aperture.horizontal_offset_n = (int32_t)avifdec_load_u32be(payload + 16U);
            track->clean_aperture.horizontal_offset_d = avifdec_load_u32be(payload + 20U);
            track->clean_aperture.vertical_offset_n = (int32_t)avifdec_load_u32be(payload + 24U);
            track->clean_aperture.vertical_offset_d = avifdec_load_u32be(payload + 28U);
            track->transform_flags |= AVIFDEC_TRANSFORM_CLAP;
            seen_clap = 1;
        } else if (child_type == AVIFDEC_FOURCC('a', 'u', 'x', 'i')) {
            size_t string_size;

            if (seen_auxi || child_payload_size < 5U) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            seen_auxi = 1;
            for (string_size = 0U;
                 4U + string_size < child_payload_size && payload[4U + string_size] != 0U;
                 ++string_size) {
            }
            if (4U + string_size >= child_payload_size) {
                return avifseq_fail(context, AVIFDEC_INVALID_DATA, child_box_offset, child_type);
            }
            if (avifseq_text_equal(payload + 4U, string_size,
                                   "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha")) {
                track->is_alpha_entry = 1U;
            }
        }
    }
    if (!track->seen_av1c) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA, track->stsd.offset, track->stsd.type);
    }
    if (entry_iter.position != entry_iter.size) {
        return avifseq_fail(context, AVIFDEC_INVALID_DATA,
                            entry_box_offset, track->stsd.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_sample_sizes(
    AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint32_t sample_count;
    size_t index;

    if (avifseq_box_is_set(&track->stsz) ==
        avifseq_box_is_set(&track->stz2)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stbl.offset,
            AVIFDEC_FOURCC('s', 't', 's', 'z'));
    }
    if (avifseq_box_is_set(&track->stsz)) {
        uint32_t fixed_size;

        avifdec_byte_reader_init(
            &reader, context->data + track->stsz.payload_offset,
            track->stsz.payload_size, track->stsz.payload_offset);
        if (avifdec_byte_reader_u32be(&reader) != 0U) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA, track->stsz.offset,
                track->stsz.type);
        }
        fixed_size = avifdec_byte_reader_u32be(&reader);
        sample_count = avifdec_byte_reader_u32be(&reader);
        if (reader.status != AVIFDEC_OK ||
            sample_count == 0U ||
            sample_count > context->limits.max_frames ||
            sample_count > AVIF_SEQ_MAX_FRAMES) {
            return avifseq_fail(
                context,
                reader.status != AVIFDEC_OK ? reader.status :
                    AVIFDEC_LIMIT_EXCEEDED,
                track->stsz.offset, track->stsz.type);
        }
        for (index = 0U; index < sample_count; ++index) {
            uint32_t sample_size = fixed_size != 0U
                ? fixed_size : avifdec_byte_reader_u32be(&reader);

            if (sample_size == 0U) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    avifdec_byte_reader_offset(&reader),
                    track->stsz.type);
            }
            track->sample_size[index] = sample_size;
        }
    } else {
        uint8_t field_size;

        avifdec_byte_reader_init(
            &reader, context->data + track->stz2.payload_offset,
            track->stz2.payload_size, track->stz2.payload_offset);
        if (avifdec_byte_reader_u32be(&reader) != 0U) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA, track->stz2.offset,
                track->stz2.type);
        }
        (void)avifdec_byte_reader_skip(&reader, 3U);
        field_size = avifdec_byte_reader_u8(&reader);
        sample_count = avifdec_byte_reader_u32be(&reader);
        if (reader.status != AVIFDEC_OK ||
            (field_size != 4U && field_size != 8U && field_size != 16U) ||
            sample_count == 0U ||
            sample_count > context->limits.max_frames ||
            sample_count > AVIF_SEQ_MAX_FRAMES) {
            return avifseq_fail(
                context,
                reader.status != AVIFDEC_OK ? reader.status :
                sample_count > context->limits.max_frames ||
                sample_count > AVIF_SEQ_MAX_FRAMES
                    ? AVIFDEC_LIMIT_EXCEEDED : AVIFDEC_INVALID_DATA,
                track->stz2.offset, track->stz2.type);
        }
        for (index = 0U; index < sample_count; ++index) {
            uint32_t sample_size;

            if (field_size == 4U) {
                uint8_t packed;

                if ((index & 1U) == 0U) {
                    packed = avifdec_byte_reader_u8(&reader);
                    sample_size = packed >> 4U;
                    if (index + 1U < sample_count) {
                        track->sample_size[index + 1U] =
                            packed & 15U;
                    }
                } else {
                    sample_size = track->sample_size[index];
                }
            } else if (field_size == 8U) {
                sample_size = avifdec_byte_reader_u8(&reader);
            } else {
                sample_size = avifdec_byte_reader_u16be(&reader);
            }
            if (sample_size == 0U) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    avifdec_byte_reader_offset(&reader),
                    track->stz2.type);
            }
            track->sample_size[index] = sample_size;
        }
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U) {
        return avifseq_fail(
            context,
            reader.status != AVIFDEC_OK ? reader.status :
                AVIFDEC_INVALID_DATA,
            avifdec_byte_reader_offset(&reader),
            avifseq_box_is_set(&track->stsz)
                ? track->stsz.type : track->stz2.type);
    }
    track->sample_count = sample_count;
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_timing(
    AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint32_t entry_count;
    uint64_t dts = 0U;
    size_t sample_index = 0U;
    size_t entry;

    if (!avifseq_box_is_set(&track->stts)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stbl.offset,
            AVIFDEC_FOURCC('s', 't', 't', 's'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->stts.payload_offset,
        track->stts.payload_size, track->stts.payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stts.offset,
            track->stts.type);
    }
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (entry_count == 0U || entry_count > AVIF_SEQ_MAX_STTS_ENTRIES) {
        return avifseq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, track->stts.offset,
            track->stts.type);
    }
    for (entry = 0U; entry < entry_count; ++entry) {
        uint32_t count = avifdec_byte_reader_u32be(&reader);
        uint32_t duration = avifdec_byte_reader_u32be(&reader);
        size_t count_index;

        if (count == 0U || duration == 0U ||
            count > track->sample_count - sample_index) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                avifdec_byte_reader_offset(&reader),
                track->stts.type);
        }
        for (count_index = 0U; count_index < count; ++count_index) {
            track->dts[sample_index] = dts;
            track->sample_duration[sample_index] = duration;
            if ((uint64_t)duration > UINT64_MAX - dts) {
                return avifseq_fail(
                    context, AVIFDEC_OVERFLOW, track->stts.offset,
                    track->stts.type);
            }
            dts += duration;
            ++sample_index;
        }
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        sample_index != track->sample_count ||
        dts != track->media_duration) {
        return avifseq_fail(
            context,
            reader.status != AVIFDEC_OK ? reader.status :
                AVIFDEC_INVALID_DATA,
            track->stts.offset, track->stts.type);
    }

    if (!avifseq_box_is_set(&track->ctts)) return AVIFDEC_OK;
    avifdec_byte_reader_init(
        &reader, context->data + track->ctts.payload_offset,
        track->ctts.payload_size, track->ctts.payload_offset);
    {
        uint8_t version = avifdec_byte_reader_u8(&reader);

        (void)avifdec_byte_reader_skip(&reader, 3U);
        entry_count = avifdec_byte_reader_u32be(&reader);
        if ((version != 0U && version != 1U) ||
            entry_count == 0U ||
            entry_count > AVIF_SEQ_MAX_CTTS_ENTRIES) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA, track->ctts.offset,
                track->ctts.type);
        }
        sample_index = 0U;
        for (entry = 0U; entry < entry_count; ++entry) {
            uint32_t count = avifdec_byte_reader_u32be(&reader);
            uint32_t raw_offset = avifdec_byte_reader_u32be(&reader);
            int64_t offset = version == 0U
                ? (int64_t)raw_offset : (int64_t)(int32_t)raw_offset;
            size_t count_index;

            if (count == 0U ||
                count > track->sample_count - sample_index) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    avifdec_byte_reader_offset(&reader),
                    track->ctts.type);
            }
            for (count_index = 0U; count_index < count; ++count_index) {
                track->cts_offset[sample_index++] = offset;
            }
        }
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        sample_index != track->sample_count) {
        return avifseq_fail(
            context,
            reader.status != AVIFDEC_OK ? reader.status :
                AVIFDEC_INVALID_DATA,
            track->ctts.offset, track->ctts.type);
    }
    return AVIFDEC_OK;
}

static int avifseq_sample_in_mdat(
    const AvifSeqContext *context, uint64_t offset, uint32_t size) {
    size_t index;

    for (index = 0U; index < context->data_box_count; ++index) {
        const AvifdecBmffBox *box = &context->data_boxes[index];
        uint64_t start = box->payload_offset;
        uint64_t end = start + box->payload_size;

        if (offset >= start && offset <= end &&
            (uint64_t)size <= end - offset) {
            return 1;
        }
    }
    return 0;
}

static AvifdecStatus avifseq_parse_chunks(
    AvifSeqContext *context, AvifSeqTrack *track) {
    uint32_t first_chunk[AVIF_SEQ_MAX_STSC_ENTRIES];
    uint32_t samples_per_chunk[AVIF_SEQ_MAX_STSC_ENTRIES];
    uint64_t chunk_offset[AVIF_SEQ_MAX_CHUNKS];
    AvifdecByteReader reader;
    uint32_t entry_count;
    uint32_t chunk_count;
    size_t entry;
    size_t chunk;
    size_t sample_index = 0U;
    size_t current_entry = 0U;

    if (!avifseq_box_is_set(&track->stsc) ||
        avifseq_box_is_set(&track->stco) ==
            avifseq_box_is_set(&track->co64)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stbl.offset,
            AVIFDEC_FOURCC('s', 't', 's', 'c'));
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->stsc.payload_offset,
        track->stsc.payload_size, track->stsc.payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stsc.offset,
            track->stsc.type);
    }
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (entry_count == 0U ||
        entry_count > AVIF_SEQ_MAX_STSC_ENTRIES) {
        return avifseq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED, track->stsc.offset,
            track->stsc.type);
    }
    for (entry = 0U; entry < entry_count; ++entry) {
        uint32_t description_index;

        first_chunk[entry] = avifdec_byte_reader_u32be(&reader);
        samples_per_chunk[entry] =
            avifdec_byte_reader_u32be(&reader);
        description_index = avifdec_byte_reader_u32be(&reader);
        if (first_chunk[entry] == 0U ||
            samples_per_chunk[entry] == 0U ||
            description_index != 1U ||
            (entry == 0U && first_chunk[entry] != 1U) ||
            (entry != 0U &&
             first_chunk[entry] <= first_chunk[entry - 1U])) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                avifdec_byte_reader_offset(&reader),
                track->stsc.type);
        }
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U) {
        return avifseq_fail(
            context,
            reader.status != AVIFDEC_OK ? reader.status :
                AVIFDEC_INVALID_DATA,
            track->stsc.offset, track->stsc.type);
    }

    {
        const AvifdecBmffBox *offset_box =
            avifseq_box_is_set(&track->stco)
                ? &track->stco : &track->co64;
        int is_64_bit = offset_box->type ==
            AVIFDEC_FOURCC('c', 'o', '6', '4');

        avifdec_byte_reader_init(
            &reader, context->data + offset_box->payload_offset,
            offset_box->payload_size, offset_box->payload_offset);
        if (avifdec_byte_reader_u32be(&reader) != 0U) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA, offset_box->offset,
                offset_box->type);
        }
        chunk_count = avifdec_byte_reader_u32be(&reader);
        if (chunk_count == 0U ||
            chunk_count > AVIF_SEQ_MAX_CHUNKS) {
            return avifseq_fail(
                context, AVIFDEC_LIMIT_EXCEEDED, offset_box->offset,
                offset_box->type);
        }
        for (chunk = 0U; chunk < chunk_count; ++chunk) {
            chunk_offset[chunk] = is_64_bit
                ? avifdec_byte_reader_u64be(&reader)
                : avifdec_byte_reader_u32be(&reader);
        }
        if (reader.status != AVIFDEC_OK ||
            avifdec_byte_reader_remaining(&reader) != 0U) {
            return avifseq_fail(
                context,
                reader.status != AVIFDEC_OK ? reader.status :
                    AVIFDEC_INVALID_DATA,
                offset_box->offset, offset_box->type);
        }
    }

    for (chunk = 0U; chunk < chunk_count; ++chunk) {
        uint64_t offset = chunk_offset[chunk];
        size_t count_index;

        while (current_entry + 1U < entry_count &&
               first_chunk[current_entry + 1U] <= chunk + 1U) {
            ++current_entry;
        }
        for (count_index = 0U;
             count_index < samples_per_chunk[current_entry];
             ++count_index) {
            uint32_t sample_size;

            if (sample_index >= track->sample_count) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->stsc.offset, track->stsc.type);
            }
            sample_size = track->sample_size[sample_index];
            if (!avifseq_sample_in_mdat(context, offset, sample_size)) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    offset > SIZE_MAX ? context->size : (size_t)offset,
                    avifseq_box_is_set(&track->stco)
                        ? track->stco.type : track->co64.type);
            }
            track->sample_offset[sample_index] = offset;
            if ((uint64_t)sample_size > UINT64_MAX - offset) {
                return avifseq_fail(
                    context, AVIFDEC_OVERFLOW,
                    track->stsc.offset, track->stsc.type);
            }
            offset += sample_size;
            ++sample_index;
        }
    }
    if (sample_index != track->sample_count) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stsc.offset,
            track->stsc.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_sync_samples(
    AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecByteReader reader;
    uint32_t entry_count;
    uint32_t previous = 0U;
    size_t entry;

    if (!avifseq_box_is_set(&track->stss)) {
        avifdec_memory_fill(
            track->sample_sync, 1U, track->sample_count);
        return AVIFDEC_OK;
    }
    avifdec_byte_reader_init(
        &reader, context->data + track->stss.payload_offset,
        track->stss.payload_size, track->stss.payload_offset);
    if (avifdec_byte_reader_u32be(&reader) != 0U) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stss.offset,
            track->stss.type);
    }
    entry_count = avifdec_byte_reader_u32be(&reader);
    if (entry_count == 0U ||
        entry_count > AVIF_SEQ_MAX_STSS_ENTRIES) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->stss.offset,
            track->stss.type);
    }
    for (entry = 0U; entry < entry_count; ++entry) {
        uint32_t sample_number =
            avifdec_byte_reader_u32be(&reader);

        if (sample_number == 0U ||
            sample_number > track->sample_count ||
            sample_number <= previous) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                avifdec_byte_reader_offset(&reader),
                track->stss.type);
        }
        track->sample_sync[sample_number - 1U] = 1U;
        previous = sample_number;
    }
    if (reader.status != AVIFDEC_OK ||
        avifdec_byte_reader_remaining(&reader) != 0U ||
        track->sample_sync[0] == 0U) {
        return avifseq_fail(
            context,
            reader.status != AVIFDEC_OK ? reader.status :
                AVIFDEC_INVALID_DATA,
            track->stss.offset, track->stss.type);
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_parse_track(
    AvifSeqContext *context, AvifSeqTrack *track) {
    AvifdecStatus status;

    status = avifseq_parse_tkhd(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_mdhd(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_hdlr(context, track);
    if (status != AVIFDEC_OK) return status;
    if (track->handler_type != AVIFDEC_FOURCC('p', 'i', 'c', 't') &&
        track->handler_type != AVIFDEC_FOURCC('a', 'u', 'x', 'v')) {
        return AVIFDEC_OK;
    }
    if (!track->matrix_identity) {
        return avifseq_fail(
            context, AVIFDEC_UNSUPPORTED, track->tkhd.offset,
            track->tkhd.type);
    }
    if (!avifseq_box_is_set(&track->mdia) ||
        !avifseq_box_is_set(&track->minf) ||
        !avifseq_box_is_set(&track->stbl)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, track->trak.offset,
            track->trak.type);
    }
    status = avifseq_parse_elst(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_tref(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_stsd(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_sample_sizes(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_timing(context, track);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_parse_chunks(context, track);
    if (status != AVIFDEC_OK) return status;
    return avifseq_parse_sync_samples(context, track);
}

static AvifdecLimits avifseq_effective_limits(
    const AvifdecLimits *limits) {
    AvifdecLimits result;

    avifdec_memory_fill(&result, 0U, sizeof(result));
    result.max_width =
        limits == 0 || limits->max_width == 0U
            ? 32768U : limits->max_width;
    result.max_height =
        limits == 0 || limits->max_height == 0U
            ? 32768U : limits->max_height;
    result.max_pixels =
        limits == 0 || limits->max_pixels == 0U
            ? 268435456U : limits->max_pixels;
    result.max_items =
        limits == 0 || limits->max_items == 0U
            ? AVIFDEC_DEFAULT_MAX_ITEMS : limits->max_items;
    result.max_extents =
        limits == 0 || limits->max_extents == 0U
            ? AVIFDEC_DEFAULT_MAX_EXTENTS : limits->max_extents;
    result.max_properties =
        limits == 0 || limits->max_properties == 0U
            ? AVIFDEC_DEFAULT_MAX_PROPERTIES : limits->max_properties;
    result.max_obus =
        limits == 0 || limits->max_obus == 0U
            ? AVIFDEC_DEFAULT_MAX_OBUS : limits->max_obus;
    result.max_frames =
        limits == 0 || limits->max_frames == 0U
            ? AVIFDEC_DEFAULT_MAX_FRAMES : limits->max_frames;
    result.operating_point =
        limits == 0 ? 0U : limits->operating_point;
    result.av1_framing =
        limits == 0 ? AVIFDEC_AV1_LOW_OVERHEAD :
        limits->av1_framing;
    result.spatial_layer =
        limits == 0 ? 0U : limits->spatial_layer;
    result.spatial_layer_set =
        limits == 0 ? 0U : limits->spatial_layer_set;
    return result;
}

static int avifseq_has_avis_brand(const AvifdecBmffInfo *info) {
    size_t index;

    if (info->major_brand ==
        AVIFDEC_FOURCC('a', 'v', 'i', 's')) return 1;
    for (index = 0U;
         index < info->compatible_brand_count; ++index) {
        if (info->compatible_brands[index] ==
            AVIFDEC_FOURCC('a', 'v', 'i', 's')) return 1;
    }
    return 0;
}

static AvifdecStatus avifseq_open(
    AvifSeqContext *context,
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifSeqTrack **main_track,
    AvifSeqTrack **alpha_track,
    AvifdecError *error) {
    AvifdecBmffInfo bmff_info;
    AvifdecBmffLimits bmff_limits = { 32U, 100000U };
    AvifdecStatus status;
    size_t index;

    avifdec_memory_fill(context, 0U, sizeof(*context));
    if (error != 0) {
        error->status = AVIFDEC_OK;
        error->offset = 0U;
        error->context = 0U;
    }
    context->data = (const unsigned char *)data;
    context->size = size;
    context->limits = avifseq_effective_limits(limits);
    context->error = error;
    status = avifdec_bmff_inspect(
        data, size, &bmff_limits, avifseq_collect_box, context,
        &bmff_info, error);
    if (status != AVIFDEC_OK) return status;
    if (context->failed) {
        return error == 0 ? AVIFDEC_INVALID_DATA : error->status;
    }
    if (!avifseq_has_avis_brand(&bmff_info) ||
        !avifseq_box_is_set(&context->moov) ||
        context->track_count == 0U) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, 0U,
            AVIFDEC_FOURCC('f', 't', 'y', 'p'));
    }
    status = avifseq_parse_mvhd(context);
    if (status != AVIFDEC_OK) return status;
    *main_track = 0;
    *alpha_track = 0;
    for (index = 0U; index < context->track_count; ++index) {
        AvifSeqTrack *track = &context->tracks[index];

        status = avifseq_parse_track(context, track);
        if (status != AVIFDEC_OK) return status;
        if (track->handler_type !=
                AVIFDEC_FOURCC('p', 'i', 'c', 't') &&
            track->handler_type !=
                AVIFDEC_FOURCC('a', 'u', 'x', 'v')) {
            continue;
        }
        if (track->handler_type ==
            AVIFDEC_FOURCC('a', 'u', 'x', 'v')) {
            if (!track->is_alpha_entry) {
                return avifseq_fail(
                    context, AVIFDEC_UNSUPPORTED,
                    track->stsd.offset, track->stsd.type);
            }
            if (*alpha_track != 0) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->trak.offset, track->trak.type);
            }
            *alpha_track = track;
        } else {
            if (track->is_alpha_entry) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->stsd.offset, track->stsd.type);
            }
            if (*main_track != 0) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    track->trak.offset, track->trak.type);
            }
            *main_track = track;
        }
    }
    if (*main_track == 0) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA, context->moov.offset,
            AVIFDEC_FOURCC('t', 'r', 'a', 'k'));
    }
    if ((*main_track)->sample_count == 0U ||
        (*main_track)->sample_count > context->limits.max_frames) {
        return avifseq_fail(
            context, AVIFDEC_LIMIT_EXCEEDED,
            (*main_track)->stsz.offset,
            AVIFDEC_FOURCC('s', 't', 's', 'z'));
    }
    if (*alpha_track != 0) {
        AvifSeqTrack *main = *main_track;
        AvifSeqTrack *alpha = *alpha_track;

        if (!alpha->has_auxl ||
            alpha->auxl_target != main->track_id ||
            alpha->entry_width != main->entry_width ||
            alpha->entry_height != main->entry_height ||
            alpha->media_timescale != main->media_timescale ||
            alpha->media_duration != main->media_duration ||
            alpha->sample_count != main->sample_count ||
            !alpha->monochrome) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                alpha->trak.offset, alpha->trak.type);
        }
        for (index = 0U; index < main->sample_count; ++index) {
            if (alpha->dts[index] != main->dts[index] ||
                alpha->sample_duration[index] !=
                    main->sample_duration[index] ||
                alpha->sample_sync[index] !=
                    main->sample_sync[index]) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    alpha->stts.offset, alpha->stts.type);
            }
        }
        if (main->has_prem) {
            if (main->prem_target != alpha->track_id) {
                return avifseq_fail(
                    context, AVIFDEC_INVALID_DATA,
                    main->tref.offset, main->tref.type);
            }
        }
    } else if ((*main_track)->has_prem) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA,
            (*main_track)->tref.offset,
            (*main_track)->tref.type);
    }
    return AVIFDEC_OK;
}

static void avifseq_info_from_track(
    const AvifSeqTrack *track, AvifdecImageInfo *info) {
    avifdec_memory_fill(info, 0U, sizeof(*info));
    info->primary_item_id = track->track_id;
    info->primary_item_type =
        AVIFDEC_FOURCC('a', 'v', '0', '1');
    info->width = track->entry_width;
    info->height = track->entry_height;
    info->profile = track->profile;
    info->level = track->level;
    info->tier = track->tier;
    info->bit_depth = track->bit_depth;
    info->monochrome = track->monochrome;
    info->subsampling_x = track->subsampling_x;
    info->subsampling_y = track->subsampling_y;
    info->chroma_sample_position =
        track->chroma_sample_position;
    info->channel_count = track->monochrome ? 1U : 3U;
    info->has_nclx = track->has_nclx;
    info->color_primaries = track->color_primaries;
    info->transfer_characteristics =
        track->transfer_characteristics;
    info->matrix_coefficients = track->matrix_coefficients;
    info->color_range = track->color_range;
    info->icc_data = track->icc_data;
    info->icc_size = track->icc_size;
    info->transform_flags = track->transform_flags;
    info->clean_aperture = track->clean_aperture;
    info->pixel_aspect_h_spacing =
        track->pixel_aspect_h_spacing;
    info->pixel_aspect_v_spacing =
        track->pixel_aspect_v_spacing;
    info->crop.x = 0U;
    info->crop.y = 0U;
    info->crop.width = info->width;
    info->crop.height = info->height;
    if ((track->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U) {
        (void)avifdec_clap_to_crop_rect(
            &track->clean_aperture, info->width, info->height,
            &info->crop);
    }
    info->presentation_width = info->crop.width;
    info->presentation_height = info->crop.height;
}

static size_t avifseq_sync_index(
    const AvifSeqTrack *track, size_t frame_index) {
    while (frame_index != 0U &&
           track->sample_sync[frame_index] == 0U) {
        --frame_index;
    }
    return frame_index;
}

static int avifseq_sample_has_sequence_header(
    const unsigned char *data, size_t size) {
    size_t position = 0U;
    size_t obu_count = 0U;

    while (position < size && obu_count++ < 64U) {
        uint8_t header = data[position++];
        uint8_t obu_type = (header >> 3U) & 15U;
        uint8_t extension_flag = (header >> 2U) & 1U;
        uint8_t has_size_field = (header >> 1U) & 1U;
        size_t payload_size = 0U;
        unsigned int shift = 0U;

        if ((header & 0x81U) != 0U || !has_size_field) return 0;
        if (extension_flag) {
            if (position >= size) return 0;
            ++position;
        }
        for (;;) {
            uint8_t byte;

            if (position >= size || shift >= 56U) return 0;
            byte = data[position++];
            payload_size |= (size_t)(byte & 0x7fU) << shift;
            if ((byte & 0x80U) == 0U) break;
            shift += 7U;
        }
        if (payload_size > size - position) return 0;
        if (obu_type == 1U) return 1;
        if (obu_type == 3U || obu_type == 6U) return 0;
        position += payload_size;
    }
    return 0;
}

static AvifdecStatus avifseq_build_spans(
    AvifSeqContext *context,
    const AvifSeqTrack *track,
    size_t frame_index,
    AvifdecSpan spans[AVIF_SEQ_MAX_DECODE_SPANS],
    size_t *span_count,
    size_t *sync_index) {
    size_t index;

    *sync_index = avifseq_sync_index(track, frame_index);
    *span_count = 0U;
    if (track->config_obus_size != 0U &&
        !avifseq_sample_has_sequence_header(
            context->data +
                (size_t)track->sample_offset[*sync_index],
            track->sample_size[*sync_index])) {
        spans[*span_count].data = track->config_obus;
        spans[*span_count].size = track->config_obus_size;
        spans[*span_count].file_offset =
            (size_t)(track->config_obus - context->data);
        ++*span_count;
    }
    for (index = *sync_index; index <= frame_index; ++index) {
        uint64_t offset = track->sample_offset[index];

        if (offset > SIZE_MAX ||
            track->sample_size[index] >
                context->size - (size_t)offset) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                offset > SIZE_MAX ? context->size : (size_t)offset,
                AVIFDEC_FOURCC('m', 'd', 'a', 't'));
        }
        spans[*span_count].data =
            context->data + (size_t)offset;
        spans[*span_count].size = track->sample_size[index];
        spans[*span_count].file_offset = (size_t)offset;
        ++*span_count;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_track_workspace_requirement(
    AvifSeqContext *context,
    const AvifSeqTrack *track,
    size_t worker_count,
    size_t *workspace_required) {
    size_t first = 0U;

    *workspace_required = 0U;
    while (first < track->sample_count) {
        AvifdecSpan spans[AVIF_SEQ_MAX_DECODE_SPANS];
        AvifdecImageInfo group_info;
        size_t last = first;
        size_t span_count;
        size_t sync_index;
        AvifdecStatus status;

        while (last + 1U < track->sample_count &&
               !track->sample_sync[last + 1U]) {
            ++last;
        }
        status = avifseq_build_spans(
            context, track, last, spans, &span_count, &sync_index);
        if (status != AVIFDEC_OK) return status;
        if (sync_index != first) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                track->stss.offset, track->stss.type);
        }
        avifdec_memory_fill(&group_info, 0U, sizeof(group_info));
        status = avifdec_av1_query_ex(
            spans, span_count, &context->limits, worker_count,
            &group_info, context->error);
        if (status != AVIFDEC_OK) return status;
        if (group_info.workspace_required > *workspace_required) {
            *workspace_required = group_info.workspace_required;
        }
        first = last + 1U;
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_query_frame_context(
    AvifSeqContext *context,
    AvifSeqTrack *main_track,
    AvifSeqTrack *alpha_track,
    size_t frame_index,
    size_t worker_count,
    AvifdecFrameInfo *frame) {
    AvifdecSpan spans[AVIF_SEQ_MAX_DECODE_SPANS];
    size_t span_count;
    size_t sync_index;
    AvifdecStatus status;

    if (frame_index >= main_track->sample_count) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_ARGUMENT,
            main_track->stsz.offset, main_track->stsz.type);
    }
    avifdec_memory_fill(frame, 0U, sizeof(*frame));
    avifseq_info_from_track(main_track, &frame->image);
    status = avifseq_build_spans(
        context, main_track, frame_index, spans,
        &span_count, &sync_index);
    if (status != AVIFDEC_OK) return status;
    /*
     * The main track's workspace sizing tracks the executor width the
     * caller intends to decode with, so its tile-parallel buffers (if
     * any) match what avifdec_av1_decode_ex() will actually allocate.
     */
    status = avifdec_av1_query_ex(
        spans, span_count, &context->limits, worker_count, &frame->image,
        context->error);
    if (status != AVIFDEC_OK) return status;
    frame->image.extent_count = span_count;
    frame->image.payload_size = 0U;
    {
        size_t index;
        for (index = 0U; index < span_count; ++index) {
            if (!avifdec_size_add(
                    frame->image.payload_size, spans[index].size,
                    &frame->image.payload_size)) {
                return avifseq_fail(
                    context, AVIFDEC_OVERFLOW,
                    main_track->stsz.offset, main_track->stsz.type);
            }
        }
    }
    frame->image.transform_flags =
        main_track->transform_flags;
    frame->image.clean_aperture =
        main_track->clean_aperture;
    frame->image.pixel_aspect_h_spacing =
        main_track->pixel_aspect_h_spacing;
    frame->image.pixel_aspect_v_spacing =
        main_track->pixel_aspect_v_spacing;
    frame->image.crop.x = 0U;
    frame->image.crop.y = 0U;
    frame->image.crop.width = frame->image.width;
    frame->image.crop.height = frame->image.height;
    if ((main_track->transform_flags &
         AVIFDEC_TRANSFORM_CLAP) != 0U &&
        !avifdec_clap_to_crop_rect(
            &main_track->clean_aperture,
            frame->image.width, frame->image.height,
            &frame->image.crop)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA,
            main_track->stsd.offset,
            AVIFDEC_FOURCC('c', 'l', 'a', 'p'));
    }
    frame->image.presentation_width =
        frame->image.crop.width;
    frame->image.presentation_height =
        frame->image.crop.height;
    frame->frame_index = frame_index;
    frame->sync_frame_index = sync_index;
    frame->dts = main_track->dts[frame_index];
    if (frame->dts > (uint64_t)INT64_MAX ||
        (main_track->cts_offset[frame_index] < 0 &&
        (uint64_t)(-main_track->cts_offset[frame_index]) >
            frame->dts)) {
        return avifseq_fail(
            context, AVIFDEC_INVALID_DATA,
            main_track->ctts.offset, main_track->ctts.type);
    }
    if (main_track->cts_offset[frame_index] > 0 &&
        (uint64_t)main_track->cts_offset[frame_index] >
            (uint64_t)INT64_MAX - frame->dts) {
        return avifseq_fail(
            context, AVIFDEC_OVERFLOW,
            main_track->ctts.offset, main_track->ctts.type);
    }
    frame->pts =
        (int64_t)frame->dts +
        main_track->cts_offset[frame_index];
    frame->duration =
        main_track->sample_duration[frame_index];
    frame->is_sync =
        main_track->sample_sync[frame_index];
    frame->sample_size =
        main_track->sample_size[frame_index];
    if (alpha_track != 0) {
        AvifdecImageInfo alpha_info;
        size_t alpha_span_count;
        size_t alpha_sync_index;

        avifseq_info_from_track(alpha_track, &alpha_info);
        alpha_info.has_nclx = 0U;
        alpha_info.icc_data = 0;
        alpha_info.icc_size = 0U;
        status = avifseq_build_spans(
            context, alpha_track, frame_index, spans,
            &alpha_span_count, &alpha_sync_index);
        if (status != AVIFDEC_OK) return status;
        /*
         * The alpha track is always sized (and later decoded, see
         * avifdec_sequence_decode_frame_ex()) for a single worker,
         * independent of worker_count above. It reuses the main track's
         * workspace buffer sequentially rather than concurrently, and
         * alpha planes are typically single-tile, so sizing its
         * tile-parallel buffers for the full executor width would only
         * add unused per-worker scratch to workspace_required.
         */
        status = avifdec_av1_query(
            spans, alpha_span_count, &context->limits,
            &alpha_info, context->error);
        if (status != AVIFDEC_OK) return status;
        if (!alpha_info.monochrome ||
            alpha_info.width != frame->image.width ||
            alpha_info.height != frame->image.height ||
            alpha_sync_index != sync_index) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                alpha_track->stsd.offset, alpha_track->stsd.type);
        }
        frame->image.has_alpha = 1U;
        frame->image.alpha_premultiplied =
            main_track->has_prem;
        frame->image.alpha_bit_depth = alpha_info.bit_depth;
        frame->image.alpha_color_range =
            alpha_info.color_range;
        frame->image.alpha_item_id = alpha_track->track_id;
        if (alpha_info.workspace_required >
            frame->image.workspace_required) {
            frame->image.workspace_required =
                alpha_info.workspace_required;
        }
    }
    return AVIFDEC_OK;
}

static AvifdecStatus avifseq_fill_sequence_info(
    AvifSeqContext *context,
    AvifSeqTrack *main_track,
    AvifSeqTrack *alpha_track,
    size_t worker_count,
    AvifdecSequenceInfo *info) {
    AvifdecFrameInfo first_frame;
    size_t workspace_required;
    AvifdecStatus status = avifseq_query_frame_context(
        context, main_track, alpha_track, 0U, worker_count, &first_frame);

    if (status != AVIFDEC_OK) return status;
    status = avifseq_track_workspace_requirement(
        context, main_track, worker_count, &workspace_required);
    if (status != AVIFDEC_OK) return status;
    if (alpha_track != 0) {
        size_t alpha_workspace_required;

        status = avifseq_track_workspace_requirement(
            context, alpha_track, 1U, &alpha_workspace_required);
        if (status != AVIFDEC_OK) return status;
        if (alpha_workspace_required > workspace_required) {
            workspace_required = alpha_workspace_required;
        }
    }
    avifdec_memory_fill(info, 0U, sizeof(*info));
    info->main_track_id = main_track->track_id;
    info->alpha_track_id =
        alpha_track == 0 ? 0U : alpha_track->track_id;
    info->width = first_frame.image.width;
    info->height = first_frame.image.height;
    info->presentation_width =
        first_frame.image.presentation_width;
    info->presentation_height =
        first_frame.image.presentation_height;
    info->crop = first_frame.image.crop;
    info->transform_flags =
        first_frame.image.transform_flags;
    info->clean_aperture =
        first_frame.image.clean_aperture;
    info->pixel_aspect_h_spacing =
        first_frame.image.pixel_aspect_h_spacing;
    info->pixel_aspect_v_spacing =
        first_frame.image.pixel_aspect_v_spacing;
    info->color_primaries =
        first_frame.image.color_primaries;
    info->transfer_characteristics =
        first_frame.image.transfer_characteristics;
    info->matrix_coefficients =
        first_frame.image.matrix_coefficients;
    info->color_range = first_frame.image.color_range;
    info->has_nclx = first_frame.image.has_nclx;
    info->icc_data = first_frame.image.icc_data;
    info->icc_size = first_frame.image.icc_size;
    info->profile = first_frame.image.profile;
    info->level = first_frame.image.level;
    info->tier = first_frame.image.tier;
    info->bit_depth = first_frame.image.bit_depth;
    info->monochrome = first_frame.image.monochrome;
    info->subsampling_x =
        first_frame.image.subsampling_x;
    info->subsampling_y =
        first_frame.image.subsampling_y;
    info->chroma_sample_position =
        first_frame.image.chroma_sample_position;
    info->timescale = main_track->media_timescale;
    info->duration = main_track->media_duration;
    info->frame_count = main_track->sample_count;
    info->workspace_required = workspace_required;
    info->has_alpha = alpha_track != 0;
    info->alpha_premultiplied =
        alpha_track != 0 && main_track->has_prem;
    if ((context->mvhd_version1 &&
         context->movie_duration == UINT64_MAX) ||
        (!context->mvhd_version1 &&
         context->movie_duration == UINT32_MAX)) {
        info->repeat_forever = 1U;
    } else {
        uint64_t base_movie_duration;

        if (main_track->media_duration >
            UINT64_MAX / context->movie_timescale) {
            return avifseq_fail(
                context, AVIFDEC_OVERFLOW,
                context->mvhd.offset, context->mvhd.type);
        }
        base_movie_duration =
            main_track->media_duration *
            context->movie_timescale;
        if (base_movie_duration %
                main_track->media_timescale != 0U) {
            return avifseq_fail(
                context, AVIFDEC_UNSUPPORTED,
                context->mvhd.offset, context->mvhd.type);
        }
        base_movie_duration /= main_track->media_timescale;
        if (base_movie_duration == 0U ||
            context->movie_duration < base_movie_duration ||
            context->movie_duration %
                base_movie_duration != 0U ||
            context->movie_duration / base_movie_duration - 1U >
                (uint64_t)INT32_MAX) {
            return avifseq_fail(
                context, AVIFDEC_INVALID_DATA,
                context->mvhd.offset, context->mvhd.type);
        }
        info->repeat_count_present = 1U;
        info->repeat_count = (int32_t)(
            context->movie_duration / base_movie_duration - 1U);
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_query_ex(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecSequenceInfo *info,
    AvifdecError *error) {
    AvifSeqContext context;
    AvifSeqTrack *main_track;
    AvifSeqTrack *alpha_track;
    AvifdecStatus status;

    if (info == 0 || !avifseq_executor_valid(executor) ||
        (data == 0 && size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifseq_open(
        &context, data, size, limits, &main_track, &alpha_track,
        error);
    if (status != AVIFDEC_OK) return status;
    return avifseq_fill_sequence_info(
        &context, main_track, alpha_track,
        avifseq_executor_width(executor), info);
}

AvifdecStatus avifdec_sequence_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    AvifdecSequenceInfo *info,
    AvifdecError *error) {
    return avifdec_sequence_query_ex(
        data, size, limits, 0, info, error);
}

AvifdecStatus avifdec_sequence_frame_query_ex(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    size_t frame_index,
    AvifdecFrameInfo *frame,
    AvifdecError *error) {
    AvifSeqContext context;
    AvifSeqTrack *main_track;
    AvifSeqTrack *alpha_track;
    AvifdecStatus status;

    if (frame == 0 || !avifseq_executor_valid(executor) ||
        (data == 0 && size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifseq_open(
        &context, data, size, limits, &main_track, &alpha_track,
        error);
    if (status != AVIFDEC_OK) return status;
    return avifseq_query_frame_context(
        &context, main_track, alpha_track, frame_index,
        avifseq_executor_width(executor), frame);
}

AvifdecStatus avifdec_sequence_frame_query(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    size_t frame_index,
    AvifdecFrameInfo *frame,
    AvifdecError *error) {
    return avifdec_sequence_frame_query_ex(
        data, size, limits, 0, frame_index, frame, error);
}

AvifdecStatus avifdec_sequence_decode_frame_ex(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    size_t frame_index,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecFrameInfo *frame,
    AvifdecError *error) {
    AvifSeqContext context;
    AvifSeqTrack *main_track;
    AvifSeqTrack *alpha_track;
    AvifdecEntropyTrace local_trace;
    AvifdecSpan spans[AVIF_SEQ_MAX_DECODE_SPANS];
    size_t span_count;
    size_t sync_index;
    AvifdecStatus status;

    if (image == 0 || frame == 0 || !avifseq_executor_valid(executor) ||
        (data == 0 && size != 0U) ||
        (workspace == 0 && workspace_size != 0U)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    status = avifseq_open(
        &context, data, size, limits, &main_track, &alpha_track,
        error);
    if (status != AVIFDEC_OK) return status;
    status = avifseq_query_frame_context(
        &context, main_track, alpha_track, frame_index,
        avifseq_executor_width(executor), frame);
    if (status != AVIFDEC_OK) return status;
    if (workspace_size < frame->image.workspace_required) {
        return AVIFDEC_OUT_OF_MEMORY;
    }
    status = avifseq_build_spans(
        &context, main_track, frame_index, spans,
        &span_count, &sync_index);
    if (status != AVIFDEC_OK) return status;
    /*
     * This single call replays every frame from sync_index through
     * frame_index in strict, serial dependency order internally (later
     * frames may reference earlier ones as AV1 references), but lets
     * each individual frame's independent AV1 tiles and post-filter row
     * units run across the executor's workers, exactly as
     * avifdec_decode_ex() does for a single still image.
     */
    status = avifdec_av1_decode_ex(
        spans, span_count, &context.limits, executor, &frame->image,
        workspace, workspace_size, image,
        trace == 0 ? &local_trace : trace, error);
    if (status != AVIFDEC_OK || alpha_track == 0) return status;
    {
        AvifdecImageInfo alpha_info;
        AvifdecImage alpha_image;
        AvifdecEntropyTrace alpha_trace;

        if (image->alpha_plane == 0 ||
            image->alpha_stride < frame->image.width) {
            return AVIFDEC_INVALID_ARGUMENT;
        }
        avifseq_info_from_track(alpha_track, &alpha_info);
        alpha_info.has_nclx = 0U;
        alpha_info.icc_data = 0;
        alpha_info.icc_size = 0U;
        status = avifseq_build_spans(
            &context, alpha_track, frame_index, spans,
            &span_count, &sync_index);
        if (status != AVIFDEC_OK) return status;
        avifdec_memory_fill(&alpha_image, 0U, sizeof(alpha_image));
        alpha_image.planes[0] = image->alpha_plane;
        alpha_image.strides[0] = image->alpha_stride;
        /*
         * Always serial (see avifseq_query_frame_context()): alpha's
         * workspace contribution is only ever sized for one worker, and
         * it reuses the main track's workspace buffer after that decode
         * has returned rather than concurrently with it, so running it
         * through the executor here would either read/write tile-worker
         * buffers that were never allocated, or - if sized for it - race
         * the main decode's use of the same arena.
         */
        status = avifdec_av1_decode(
            spans, span_count, &context.limits, &alpha_info,
            workspace, workspace_size, &alpha_image, &alpha_trace,
            error);
        if (status != AVIFDEC_OK) return status;
        image->alpha_width = alpha_image.widths[0];
        image->alpha_height = alpha_image.heights[0];
        image->alpha_bit_depth = alpha_image.bit_depth;
        image->alpha_color_range = alpha_info.color_range;
        image->alpha_premultiplied = main_track->has_prem;
    }
    return AVIFDEC_OK;
}

AvifdecStatus avifdec_sequence_decode_frame(
    const void *data,
    size_t size,
    const AvifdecLimits *limits,
    size_t frame_index,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecFrameInfo *frame,
    AvifdecError *error) {
    return avifdec_sequence_decode_frame_ex(
        data, size, limits, 0, frame_index, workspace, workspace_size,
        image, trace, frame, error);
}
