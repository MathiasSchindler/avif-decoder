#include "avif_sequence_decode.h"
#include "bmff_child.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FOURCC(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | \
     ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | \
     (uint32_t)(unsigned char)(d))

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                    __FILE__, __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

#define MAX_FILE_SIZE 131072U
#define MAX_TRACK_SPECS 4U
#define MAX_TRACK_SAMPLES 8U
#define MAX_REFS 4U
#define MAX_EDITS 4U

typedef struct {
    unsigned char data[MAX_FILE_SIZE];
    size_t size;
} TestBuffer;

typedef struct {
    uint32_t type;
    uint32_t target;
} TestReference;

typedef struct {
    uint64_t segment_duration;
    int64_t media_time;
    uint16_t rate_integer;
    uint16_t rate_fraction;
} TestEdit;

typedef struct {
    uint32_t id;
    uint32_t handler;
    uint32_t media_timescale;
    uint64_t media_duration;
    uint64_t movie_duration_override;
    uint32_t sample_duration;
    uint32_t width;
    uint32_t height;
    uint32_t clap_width;
    uint32_t clap_height;
    uint16_t alternate_group;
    size_t sample_count;
    size_t sync_count;
    size_t refs_count;
    size_t edit_count;
    TestReference refs[MAX_REFS];
    TestEdit edits[MAX_EDITS];
    int32_t matrix[9];
    uint8_t enabled;
    uint8_t alpha;
    uint8_t stz2;
    uint8_t co64;
    uint8_t ctts;
    uint8_t config_header;
    uint8_t config_prefix_metadata;
    uint8_t config_duplicate_header;
    uint8_t sample_header_mode;
    uint8_t edit_version;
    uint8_t edit_flags;
    uint8_t empty_elst;
    uint8_t tkhd_version;
    uint8_t unknown_tkhd_duration;
    uint8_t pasp;
    uint8_t clap;
    uint8_t icc;
    uint8_t omit_nclx;
    uint8_t two_stsd_entries;
} TestTrack;

typedef struct {
    size_t position;
    uint8_t co64;
    size_t track_index;
} ChunkPatch;

typedef struct {
    ChunkPatch patches[MAX_TRACK_SPECS];
    size_t patch_count;
} BuildPatches;

static void buffer_put_u8(TestBuffer *buffer, uint8_t value) {
    if (buffer->size >= sizeof(buffer->data)) abort();
    buffer->data[buffer->size++] = value;
}

static void buffer_put_u16(TestBuffer *buffer, uint16_t value) {
    buffer_put_u8(buffer, (uint8_t)(value >> 8U));
    buffer_put_u8(buffer, (uint8_t)value);
}

static void buffer_put_u32(TestBuffer *buffer, uint32_t value) {
    buffer_put_u8(buffer, (uint8_t)(value >> 24U));
    buffer_put_u8(buffer, (uint8_t)(value >> 16U));
    buffer_put_u8(buffer, (uint8_t)(value >> 8U));
    buffer_put_u8(buffer, (uint8_t)value);
}

static void buffer_put_u64(TestBuffer *buffer, uint64_t value) {
    buffer_put_u32(buffer, (uint32_t)(value >> 32U));
    buffer_put_u32(buffer, (uint32_t)value);
}

static void buffer_put_bytes(
    TestBuffer *buffer,
    const void *bytes,
    size_t size) {
    if (size > sizeof(buffer->data) - buffer->size) abort();
    memcpy(buffer->data + buffer->size, bytes, size);
    buffer->size += size;
}

static void buffer_patch_u32(
    TestBuffer *buffer,
    size_t position,
    uint32_t value) {
    buffer->data[position] = (unsigned char)(value >> 24U);
    buffer->data[position + 1U] = (unsigned char)(value >> 16U);
    buffer->data[position + 2U] = (unsigned char)(value >> 8U);
    buffer->data[position + 3U] = (unsigned char)value;
}

static void buffer_patch_u16(
    TestBuffer *buffer,
    size_t position,
    uint16_t value) {
    buffer->data[position] = (unsigned char)(value >> 8U);
    buffer->data[position + 1U] = (unsigned char)value;
}

static void buffer_patch_u64(
    TestBuffer *buffer,
    size_t position,
    uint64_t value) {
    buffer_patch_u32(buffer, position, (uint32_t)(value >> 32U));
    buffer_patch_u32(buffer, position + 4U, (uint32_t)value);
}

static size_t buffer_find_type(
    const TestBuffer *buffer,
    uint32_t type) {
    size_t position;

    for (position = 4U; position + 4U <= buffer->size; ++position) {
        if (buffer->data[position] ==
                (unsigned char)(type >> 24U) &&
            buffer->data[position + 1U] ==
                (unsigned char)(type >> 16U) &&
            buffer->data[position + 2U] ==
                (unsigned char)(type >> 8U) &&
            buffer->data[position + 3U] ==
                (unsigned char)type) {
            return position;
        }
    }
    return SIZE_MAX;
}

static size_t box_begin(TestBuffer *buffer, uint32_t type) {
    size_t start = buffer->size;

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, type);
    return start;
}

static size_t box_begin_extended(TestBuffer *buffer, uint32_t type) {
    size_t start = buffer->size;

    buffer_put_u32(buffer, 1U);
    buffer_put_u32(buffer, type);
    buffer_put_u64(buffer, 0U);
    return start;
}

static size_t box_begin_to_end(TestBuffer *buffer, uint32_t type) {
    size_t start = buffer->size;

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, type);
    return start;
}

static void box_end(TestBuffer *buffer, size_t start) {
    size_t size = buffer->size - start;

    if (buffer->data[start] == 0U &&
        buffer->data[start + 1U] == 0U &&
        buffer->data[start + 2U] == 0U &&
        buffer->data[start + 3U] == 1U) {
        buffer_patch_u64(buffer, start + 8U, size);
    } else {
        if (size > UINT32_MAX) abort();
        buffer_patch_u32(buffer, start, (uint32_t)size);
    }
}

static void track_defaults(TestTrack *track, uint32_t id) {
    static const int32_t identity[9] = {
        65536, 0, 0,
        0, 65536, 0,
        0, 0, 0x40000000
    };

    memset(track, 0, sizeof(*track));
    track->id = id;
    track->handler = TEST_FOURCC('p', 'i', 'c', 't');
    track->media_timescale = 1000U;
    track->sample_duration = 10U;
    track->sample_count = 2U;
    track->media_duration = 20U;
    track->sync_count = 1U;
    track->width = 2U;
    track->height = 2U;
    track->enabled = 1U;
    track->config_header = 1U;
    memcpy(track->matrix, identity, sizeof(identity));
}

static size_t sample_size_for_track(const TestTrack *track) {
    if (track->sample_header_mode == 4U) return 2U;
    if (track->sample_header_mode == 3U) return 9U;
    if (track->sample_header_mode != 0U) return 6U;
    return 3U;
}

static void put_sequence_header(TestBuffer *buffer, uint8_t marker) {
    buffer_put_u8(buffer, 0x0aU);
    buffer_put_u8(buffer, 1U);
    buffer_put_u8(buffer, marker);
}

static void put_sample(
    TestBuffer *buffer,
    const TestTrack *track) {
    uint8_t marker = (uint8_t)(0xa0U + track->id);

    if (track->sample_header_mode == 4U) {
        buffer_put_u8(buffer, 0x30U);
        buffer_put_u8(buffer, 0U);
        return;
    }
    if (track->sample_header_mode != 0U) {
        put_sequence_header(
            buffer,
            track->sample_header_mode == 2U
                ? (uint8_t)(marker + 1U) : marker);
    }
    if (track->sample_header_mode == 3U) {
        put_sequence_header(buffer, marker);
    }
    buffer_put_u8(buffer, 0x32U);
    buffer_put_u8(buffer, 1U);
    buffer_put_u8(buffer, 0U);
}

static void put_ftyp(TestBuffer *buffer) {
    size_t box = box_begin(buffer, TEST_FOURCC('f', 't', 'y', 'p'));

    buffer_put_u32(buffer, TEST_FOURCC('a', 'v', 'i', 's'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, TEST_FOURCC('a', 'v', 'i', 's'));
    box_end(buffer, box);
}

static void put_mvhd(
    TestBuffer *buffer,
    uint32_t timescale,
    uint64_t duration,
    int version1) {
    size_t box = box_begin(buffer, TEST_FOURCC('m', 'v', 'h', 'd'));

    buffer_put_u32(buffer, version1 ? 0x01000000U : 0U);
    if (version1) {
        buffer_put_u64(buffer, 0U);
        buffer_put_u64(buffer, 0U);
        buffer_put_u32(buffer, timescale);
        buffer_put_u64(buffer, duration);
    } else {
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, timescale);
        buffer_put_u32(buffer, (uint32_t)duration);
    }
    buffer_put_u32(buffer, 0x00010000U);
    buffer_put_u16(buffer, 0x0100U);
    buffer_put_u16(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0x00010000U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0x00010000U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0x40000000U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 10U);
    box_end(buffer, box);
}

static void put_tkhd(TestBuffer *buffer, const TestTrack *track) {
    size_t box = box_begin(buffer, TEST_FOURCC('t', 'k', 'h', 'd'));
    size_t index;
    uint64_t duration;
    uint32_t display_width =
        track->clap && track->clap_width != 0U
            ? track->clap_width : track->width;
    uint32_t display_height =
        track->clap && track->clap_height != 0U
            ? track->clap_height : track->height;

    if (track->unknown_tkhd_duration != 0U ||
        (track->sample_count == 0U &&
         track->media_duration == 0U)) {
        duration = track->tkhd_version
            ? UINT64_MAX : UINT32_MAX;
    } else if (track->edit_count != 0U) {
        duration = 0U;
        for (index = 0U; index < track->edit_count; ++index) {
            duration += track->edits[index].segment_duration;
        }
    } else {
        uint64_t scaled =
            track->media_duration * 1000U;

        duration = scaled / track->media_timescale;
        if (scaled % track->media_timescale != 0U) ++duration;
    }
    buffer_put_u32(
        buffer,
        (track->tkhd_version ? 0x01000000U : 0U) |
        (track->enabled ? 7U : 6U));
    if (track->tkhd_version) {
        buffer_put_u64(buffer, 0U);
        buffer_put_u64(buffer, 0U);
    } else {
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, 0U);
    }
    buffer_put_u32(buffer, track->id);
    buffer_put_u32(buffer, 0U);
    if (track->tkhd_version) {
        buffer_put_u64(buffer, duration);
    } else {
        buffer_put_u32(buffer, (uint32_t)duration);
    }
    buffer_put_u64(buffer, 0U);
    buffer_put_u16(buffer, 0U);
    buffer_put_u16(buffer, track->alternate_group);
    buffer_put_u16(buffer, 0U);
    buffer_put_u16(buffer, 0U);
    for (index = 0U; index < 9U; ++index) {
        buffer_put_u32(buffer, (uint32_t)track->matrix[index]);
    }
    if (track->matrix[0] == 0 &&
        track->matrix[4] == 0) {
        buffer_put_u32(buffer, display_height << 16U);
        buffer_put_u32(buffer, display_width << 16U);
    } else {
        buffer_put_u32(buffer, display_width << 16U);
        buffer_put_u32(buffer, display_height << 16U);
    }
    box_end(buffer, box);
}

static void put_edits(TestBuffer *buffer, const TestTrack *track) {
    size_t edts;
    size_t elst;
    size_t index;

    if (track->edit_count == 0U && !track->empty_elst) return;
    edts = box_begin(buffer, TEST_FOURCC('e', 'd', 't', 's'));
    elst = box_begin(buffer, TEST_FOURCC('e', 'l', 's', 't'));
    buffer_put_u32(
        buffer,
        (track->edit_version ? 0x01000000U : 0U) |
            track->edit_flags);
    buffer_put_u32(buffer, (uint32_t)track->edit_count);
    for (index = 0U; index < track->edit_count; ++index) {
        const TestEdit *edit = &track->edits[index];

        if (track->edit_version) {
            buffer_put_u64(buffer, edit->segment_duration);
            buffer_put_u64(buffer, (uint64_t)edit->media_time);
        } else {
            buffer_put_u32(buffer, (uint32_t)edit->segment_duration);
            buffer_put_u32(buffer, (uint32_t)edit->media_time);
        }
        buffer_put_u16(buffer, edit->rate_integer);
        buffer_put_u16(buffer, edit->rate_fraction);
    }
    box_end(buffer, elst);
    box_end(buffer, edts);
}

static void put_tref(TestBuffer *buffer, const TestTrack *track) {
    size_t tref;
    size_t index;

    if (track->refs_count == 0U) return;
    tref = box_begin(buffer, TEST_FOURCC('t', 'r', 'e', 'f'));
    for (index = 0U; index < track->refs_count; ++index) {
        size_t reference =
            box_begin(buffer, track->refs[index].type);
        buffer_put_u32(buffer, track->refs[index].target);
        box_end(buffer, reference);
    }
    box_end(buffer, tref);
}

static void put_mdhd(TestBuffer *buffer, const TestTrack *track) {
    size_t box = box_begin(buffer, TEST_FOURCC('m', 'd', 'h', 'd'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->media_timescale);
    buffer_put_u32(buffer, (uint32_t)track->media_duration);
    buffer_put_u16(buffer, 0x55c4U);
    buffer_put_u16(buffer, 0U);
    box_end(buffer, box);
}

static void put_hdlr(TestBuffer *buffer, const TestTrack *track) {
    size_t box = box_begin(buffer, TEST_FOURCC('h', 'd', 'l', 'r'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->handler);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u8(buffer, 0U);
    box_end(buffer, box);
}

static void put_stsd(TestBuffer *buffer, const TestTrack *track) {
    size_t stsd = box_begin(buffer, TEST_FOURCC('s', 't', 's', 'd'));
    size_t entry;
    size_t entry_size;
    size_t child;
    unsigned char zeros[32];
    uint8_t av1c_depth = track->alpha ? 0x10U : 0U;
    uint8_t marker = (uint8_t)(0xa0U + track->id);

    memset(zeros, 0, sizeof(zeros));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->two_stsd_entries ? 2U : 1U);
    entry = box_begin(buffer, TEST_FOURCC('a', 'v', '0', '1'));
    buffer_put_bytes(buffer, zeros, 6U);
    buffer_put_u16(buffer, 1U);
    buffer_put_bytes(buffer, zeros, 16U);
    buffer_put_u16(buffer, (uint16_t)track->width);
    buffer_put_u16(buffer, (uint16_t)track->height);
    buffer_put_u32(buffer, 0x00480000U);
    buffer_put_u32(buffer, 0x00480000U);
    buffer_put_u32(buffer, 0U);
    buffer_put_u16(buffer, 1U);
    buffer_put_bytes(buffer, zeros, 32U);
    buffer_put_u16(buffer, 0x0018U);
    buffer_put_u16(buffer, 0xffffU);

    child = box_begin(buffer, TEST_FOURCC('a', 'v', '1', 'C'));
    buffer_put_u8(buffer, 0x81U);
    buffer_put_u8(buffer, 0U);
    buffer_put_u8(buffer, av1c_depth);
    buffer_put_u8(buffer, 0U);
    if (track->config_prefix_metadata) {
        buffer_put_u8(buffer, 0x2aU);
        buffer_put_u8(buffer, 1U);
        buffer_put_u8(buffer, 0U);
    }
    if (track->config_header) {
        put_sequence_header(buffer, marker);
        if (track->config_duplicate_header) {
            put_sequence_header(buffer, marker);
        }
    }
    box_end(buffer, child);

    if (!track->omit_nclx) {
        child = box_begin(buffer, TEST_FOURCC('c', 'o', 'l', 'r'));
        buffer_put_u32(buffer, TEST_FOURCC('n', 'c', 'l', 'x'));
        buffer_put_u16(buffer, 1U);
        buffer_put_u16(buffer, 13U);
        buffer_put_u16(buffer, 6U);
        buffer_put_u8(buffer, 0x80U);
        box_end(buffer, child);
    }

    if (track->icc) {
        static const unsigned char profile[] = { 1U, 2U, 3U, 4U };

        child = box_begin(buffer, TEST_FOURCC('c', 'o', 'l', 'r'));
        buffer_put_u32(buffer, TEST_FOURCC('p', 'r', 'o', 'f'));
        buffer_put_bytes(buffer, profile, sizeof(profile));
        box_end(buffer, child);
    }
    if (track->pasp) {
        child = box_begin(buffer, TEST_FOURCC('p', 'a', 's', 'p'));
        buffer_put_u32(buffer, 4U);
        buffer_put_u32(buffer, 3U);
        box_end(buffer, child);
    }
    if (track->clap) {
        uint32_t clap_width =
            track->clap_width != 0U
                ? track->clap_width : track->width;
        uint32_t clap_height =
            track->clap_height != 0U
                ? track->clap_height : track->height;

        child = box_begin(buffer, TEST_FOURCC('c', 'l', 'a', 'p'));
        buffer_put_u32(buffer, clap_width);
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, clap_height);
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, 1U);
        box_end(buffer, child);
    }

    if (track->alpha) {
        static const char alpha_urn[] =
            "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";

        child = box_begin(buffer, TEST_FOURCC('a', 'u', 'x', 'i'));
        buffer_put_u32(buffer, 0U);
        buffer_put_bytes(buffer, alpha_urn, sizeof(alpha_urn));
        box_end(buffer, child);
    }
    box_end(buffer, entry);
    entry_size = buffer->size - entry;
    if (track->two_stsd_entries) {
        buffer_put_bytes(buffer, buffer->data + entry, entry_size);
    }
    box_end(buffer, stsd);
}

static void put_stts(TestBuffer *buffer, const TestTrack *track) {
    size_t box = box_begin(buffer, TEST_FOURCC('s', 't', 't', 's'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->sample_count == 0U ? 0U : 1U);
    if (track->sample_count != 0U) {
        buffer_put_u32(buffer, (uint32_t)track->sample_count);
        buffer_put_u32(buffer, track->sample_duration);
    }
    box_end(buffer, box);
}

static void put_ctts(TestBuffer *buffer, const TestTrack *track) {
    size_t box;

    if (!track->ctts) return;
    box = box_begin(buffer, TEST_FOURCC('c', 't', 't', 's'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 1U);
    buffer_put_u32(buffer, (uint32_t)track->sample_count);
    buffer_put_u32(buffer, 0U);
    box_end(buffer, box);
}

static void put_stsc(TestBuffer *buffer, const TestTrack *track) {
    size_t box = box_begin(buffer, TEST_FOURCC('s', 't', 's', 'c'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->sample_count == 0U ? 0U : 1U);
    if (track->sample_count != 0U) {
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, (uint32_t)track->sample_count);
        buffer_put_u32(buffer, 1U);
    }
    box_end(buffer, box);
}

static void put_sample_sizes(
    TestBuffer *buffer,
    const TestTrack *track) {
    size_t sample_size = sample_size_for_track(track);
    size_t index;

    if (track->stz2) {
        size_t box = box_begin(buffer, TEST_FOURCC('s', 't', 'z', '2'));

        buffer_put_u32(buffer, 0U);
        buffer_put_u8(buffer, 0U);
        buffer_put_u8(buffer, 0U);
        buffer_put_u8(buffer, 0U);
        buffer_put_u8(buffer, 4U);
        buffer_put_u32(buffer, (uint32_t)track->sample_count);
        for (index = 0U; index < track->sample_count; index += 2U) {
            uint8_t packed = (uint8_t)(sample_size << 4U);

            if (index + 1U < track->sample_count) {
                packed |= (uint8_t)sample_size;
            }
            buffer_put_u8(buffer, packed);
        }
        box_end(buffer, box);
    } else {
        size_t box = box_begin(buffer, TEST_FOURCC('s', 't', 's', 'z'));

        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, (uint32_t)track->sample_count);
        for (index = 0U; index < track->sample_count; ++index) {
            buffer_put_u32(buffer, (uint32_t)sample_size);
        }
        box_end(buffer, box);
    }
}

static void put_chunk_offsets(
    TestBuffer *buffer,
    const TestTrack *track,
    size_t track_index,
    BuildPatches *patches) {
    size_t box = box_begin(
        buffer, track->co64
            ? TEST_FOURCC('c', 'o', '6', '4')
            : TEST_FOURCC('s', 't', 'c', 'o'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->sample_count == 0U ? 0U : 1U);
    if (track->sample_count != 0U) {
        ChunkPatch *patch = &patches->patches[patches->patch_count++];

        patch->position = buffer->size;
        patch->co64 = track->co64;
        patch->track_index = track_index;
        if (track->co64) {
            buffer_put_u64(buffer, 0U);
        } else {
            buffer_put_u32(buffer, 0U);
        }
    }
    box_end(buffer, box);
}

static void put_stss(TestBuffer *buffer, const TestTrack *track) {
    size_t box;
    size_t index;

    if (track->sample_count == 0U ||
        track->sync_count == track->sample_count) {
        return;
    }
    box = box_begin(buffer, TEST_FOURCC('s', 't', 's', 's'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, (uint32_t)track->sync_count);
    for (index = 0U; index < track->sync_count; ++index) {
        buffer_put_u32(buffer, (uint32_t)index + 1U);
    }
    box_end(buffer, box);
}

static void put_track(
    TestBuffer *buffer,
    const TestTrack *track,
    size_t track_index,
    BuildPatches *patches) {
    size_t trak = box_begin(buffer, TEST_FOURCC('t', 'r', 'a', 'k'));
    size_t mdia;
    size_t minf;
    size_t stbl;

    put_tkhd(buffer, track);
    put_tref(buffer, track);
    put_edits(buffer, track);
    mdia = box_begin(buffer, TEST_FOURCC('m', 'd', 'i', 'a'));
    put_mdhd(buffer, track);
    put_hdlr(buffer, track);
    minf = box_begin(buffer, TEST_FOURCC('m', 'i', 'n', 'f'));
    stbl = box_begin(buffer, TEST_FOURCC('s', 't', 'b', 'l'));
    put_stsd(buffer, track);
    put_stts(buffer, track);
    put_ctts(buffer, track);
    put_stsc(buffer, track);
    put_sample_sizes(buffer, track);
    put_chunk_offsets(buffer, track, track_index, patches);
    put_stss(buffer, track);
    box_end(buffer, stbl);
    box_end(buffer, minf);
    box_end(buffer, mdia);
    box_end(buffer, trak);
}

static void build_classic_file(
    TestBuffer *buffer,
    const TestTrack *tracks,
    size_t track_count,
    int extended_moov,
    int zero_mdat) {
    BuildPatches patches;
    size_t starts[MAX_TRACK_SPECS];
    size_t moov;
    size_t mdat;
    size_t track_index;
    size_t patch_index;
    uint64_t movie_duration = 0U;

    memset(buffer, 0, sizeof(*buffer));
    memset(&patches, 0, sizeof(patches));
    put_ftyp(buffer);
    moov = extended_moov
        ? box_begin_extended(buffer, TEST_FOURCC('m', 'o', 'o', 'v'))
        : box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'v'));
    for (track_index = 0U; track_index < track_count; ++track_index) {
        uint64_t duration =
            tracks[track_index].movie_duration_override != 0U
            ? tracks[track_index].movie_duration_override
            : tracks[track_index].edit_count != 0U
            ? tracks[track_index].edits[0].segment_duration
            : tracks[track_index].media_duration;
        size_t edit;

        for (edit =
                tracks[track_index].movie_duration_override != 0U
                    ? tracks[track_index].edit_count : 1U;
             tracks[track_index].movie_duration_override == 0U &&
             edit < tracks[track_index].edit_count; ++edit) {
            duration += tracks[track_index].edits[edit].segment_duration;
        }
        if (duration > movie_duration) movie_duration = duration;
    }
    put_mvhd(buffer, 1000U, movie_duration, 0);
    for (track_index = 0U; track_index < track_count; ++track_index) {
        put_track(buffer, &tracks[track_index], track_index, &patches);
    }
    box_end(buffer, moov);
    mdat = zero_mdat
        ? box_begin_to_end(buffer, TEST_FOURCC('m', 'd', 'a', 't'))
        : box_begin(buffer, TEST_FOURCC('m', 'd', 'a', 't'));
    for (track_index = 0U; track_index < track_count; ++track_index) {
        size_t sample;

        starts[track_index] = buffer->size;
        for (sample = 0U; sample < tracks[track_index].sample_count;
             ++sample) {
            put_sample(buffer, &tracks[track_index]);
        }
    }
    if (!zero_mdat) box_end(buffer, mdat);
    for (patch_index = 0U; patch_index < patches.patch_count;
         ++patch_index) {
        const ChunkPatch *patch = &patches.patches[patch_index];

        if (patch->co64) {
            buffer_patch_u64(
                buffer, patch->position, starts[patch->track_index]);
        } else {
            buffer_patch_u32(
                buffer, patch->position,
                (uint32_t)starts[patch->track_index]);
        }
    }
}

typedef struct {
    const TestTrack *tracks;
    size_t track_count;
    size_t calls;
    uint64_t semantic_bias;
} ValidationState;

static AvifdecStatus validate_header(
    void *user_data,
    uint32_t track_id,
    const unsigned char *sequence_header_obu,
    size_t sequence_header_obu_size,
    const AvifdecLimits *limits,
    AvifSequenceHeaderValidation *validation,
    AvifdecError *error) {
    ValidationState *state = (ValidationState *)user_data;
    size_t index;

    (void)limits;
    (void)error;
    if (sequence_header_obu_size != 3U ||
        sequence_header_obu[0] != 0x0aU ||
        sequence_header_obu[1] != 1U) {
        return AVIFDEC_INVALID_DATA;
    }
    for (index = 0U; index < state->track_count; ++index) {
        if (state->tracks[index].id == track_id) {
            validation->coded_width = state->tracks[index].width;
            validation->coded_height = state->tracks[index].height;
            validation->semantic_id =
                sequence_header_obu[2] +
                (state->semantic_bias == UINT64_MAX
                    ? 0U : state->semantic_bias);
            validation->av1c_marker = 1U;
            validation->av1c_version = 1U;
            validation->profile =
                state->semantic_bias == UINT64_MAX - 1U
                    ? 1U : 0U;
            validation->level = 0U;
            validation->tier = 0U;
            validation->high_bitdepth = 0U;
            validation->twelve_bit = 0U;
            validation->bit_depth = 8U;
            validation->monochrome = state->tracks[index].alpha;
            validation->subsampling_x = 0U;
            validation->subsampling_y = 0U;
            validation->chroma_sample_position = 0U;
            validation->color_description_present =
                state->semantic_bias == UINT64_MAX - 3U
                    ? 0U : 1U;
            validation->color_primaries =
                validation->color_description_present == 0U
                    ? 0U
                    : state->semantic_bias == UINT64_MAX - 2U
                        ? 9U
                        : state->semantic_bias == UINT64_MAX - 4U
                            ? 2U : 1U;
            validation->transfer_characteristics =
                validation->color_description_present == 0U
                    ? 0U
                    : state->semantic_bias == UINT64_MAX - 5U
                        ? 2U
                        : state->semantic_bias == UINT64_MAX - 7U
                            ? 9U : 13U;
            validation->matrix_coefficients =
                validation->color_description_present == 0U
                    ? 0U
                    : state->semantic_bias == UINT64_MAX - 6U
                        ? 2U
                        : state->semantic_bias == UINT64_MAX - 8U
                            ? 9U : 6U;
            validation->color_range =
                (uint8_t)(
                    (state->tracks[index].alpha &&
                     state->semantic_bias == UINT64_MAX) ||
                    state->semantic_bias == UINT64_MAX - 9U
                        ? 0U : 1U);
            ++state->calls;
            return AVIFDEC_OK;
        }
    }
    return AVIFDEC_INVALID_DATA;
}

static AvifSequenceValidationCallbacks validation_callbacks(
    ValidationState *state) {
    AvifSequenceValidationCallbacks callbacks;

    callbacks.user_data = state;
    callbacks.validate_header = validate_header;
    return callbacks;
}

typedef struct {
    unsigned char *allocation;
    unsigned char *workspace;
    AvifSequenceIndex index;
    AvifSequenceIndexInfo info;
} InitializedIndex;

static int init_index(
    const TestBuffer *file,
    ValidationState *validation_state,
    InitializedIndex *initialized) {
    AvifSequenceValidationCallbacks callbacks =
        validation_callbacks(validation_state);
    AvifdecError error;
    AvifdecStatus status;
    AvifSequenceIndex short_index;
    AvifSequenceIndexInfo short_info;

    memset(initialized, 0, sizeof(*initialized));
    status = avif_sequence_index_query(
        file->data, file->size, NULL, &callbacks,
        &initialized->info, &error);
    if (status != AVIFDEC_OK) {
        fprintf(
            stderr, "index query failed: status=%d offset=%zu context=%08x\n",
            (int)status, error.offset, error.context);
    }
    CHECK(status == AVIFDEC_OK);
    CHECK(initialized->info.workspace_required != 0U);
    initialized->allocation = (unsigned char *)malloc(
        initialized->info.workspace_required + 2U);
    CHECK(initialized->allocation != NULL);
    memset(
        initialized->allocation, 0xa5,
        initialized->info.workspace_required + 2U);
    initialized->workspace = initialized->allocation + 1U;
    memset(&short_index, 0xff, sizeof(short_index));
    status = avif_sequence_index_init(
        file->data, file->size, NULL, &callbacks,
        initialized->workspace,
        initialized->info.workspace_required - 1U,
        &short_index, &short_info, &error);
    CHECK(status == AVIFDEC_OUT_OF_MEMORY);
    CHECK(short_info.workspace_required ==
          initialized->info.workspace_required);
    CHECK(short_index.opaque[0] == 0U);
    status = avif_sequence_index_init(
        file->data, file->size, NULL, &callbacks,
        initialized->workspace,
        initialized->info.workspace_required,
        &initialized->index, &initialized->info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(initialized->allocation[0] == 0xa5U);
    CHECK(initialized->allocation[
              initialized->info.workspace_required + 1U] == 0xa5U);
    return 1;
}

static void destroy_index(InitializedIndex *initialized) {
    free(initialized->allocation);
    memset(initialized, 0, sizeof(*initialized));
}

static int test_child_iterator(void) {
    TestBuffer buffer;
    AvifBmffChildIterator iterator;
    AvifBmffChild child;
    AvifdecError error;
    AvifdecStatus status;
    size_t first;
    size_t second;
    size_t third;
    int has_child;

    memset(&buffer, 0, sizeof(buffer));
    buffer_put_u8(&buffer, 0xccU);
    buffer_put_u8(&buffer, 0xddU);
    first = box_begin(&buffer, TEST_FOURCC('a', 'a', 'a', 'a'));
    buffer_put_u8(&buffer, 1U);
    box_end(&buffer, first);
    second = box_begin_extended(
        &buffer, TEST_FOURCC('b', 'b', 'b', 'b'));
    buffer_put_u8(&buffer, 2U);
    box_end(&buffer, second);
    third = box_begin_to_end(
        &buffer, TEST_FOURCC('c', 'c', 'c', 'c'));
    buffer_put_u8(&buffer, 3U);
    status = avif_bmff_child_iterator_init(
        &iterator, buffer.data, buffer.size, 2U,
        buffer.size - 2U, TEST_FOURCC('p', 'a', 'r', 't'), &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_OK && has_child);
    CHECK(child.offset == first && child.header_size == 8U);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_OK && has_child);
    CHECK(child.offset == second && child.header_size == 16U);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_OK && has_child);
    CHECK(child.offset == third && child.extends_to_parent_end);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_OK && !has_child);

    memset(&buffer, 0, sizeof(buffer));
    buffer_put_u32(&buffer, 1U);
    buffer_put_u32(&buffer, TEST_FOURCC('o', 'v', 'f', 'l'));
    buffer_put_u64(&buffer, UINT64_MAX);
    status = avif_bmff_child_iterator_init(
        &iterator, buffer.data, buffer.size, 0U,
        buffer.size, 0U, &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    if (SIZE_MAX < UINT64_MAX) {
        CHECK(status == AVIFDEC_OVERFLOW);
    } else {
        CHECK(status == AVIFDEC_TRUNCATED);
    }

    memset(&buffer, 0, sizeof(buffer));
    buffer_put_u32(&buffer, 7U);
    buffer_put_u32(&buffer, TEST_FOURCC('s', 'm', 'a', 'l'));
    status = avif_bmff_child_iterator_init(
        &iterator, buffer.data, buffer.size, 0U,
        buffer.size, 0U, &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    status = avif_bmff_child_iterator_init(
        &iterator, NULL, 0U, 0U, 0U, 0U, &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_bmff_child_next(
        &iterator, &child, &has_child, &error);
    CHECK(status == AVIFDEC_OK && !has_child);
    return 1;
}

static int test_classic_tables_workspace_and_stale(void) {
    TestBuffer file;
    TestTrack track;
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceTrackInfo track_info;
    AvifSequenceSampleInfo sample;
    AvifSequenceSelection selection;
    AvifSequencePresentationInfo presentation;
    AvifSequenceIndex stale;
    AvifSequenceValidationCallbacks callbacks;
    unsigned char *snapshot;
    AvifdecError error;
    AvifdecStatus status;

    track_defaults(&track, 1U);
    track.stz2 = 1U;
    track.co64 = 1U;
    track.pasp = 1U;
    track.clap = 1U;
    track.icc = 1U;
    build_classic_file(&file, &track, 1U, 1, 1);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    CHECK(initialized.info.track_count == 1U);
    CHECK(initialized.info.sample_count == 2U);
    CHECK(initialized.info.presentation_count == 2U);
    CHECK(validation.calls >= 2U);
    snapshot = (unsigned char *)malloc(
        initialized.info.workspace_required);
    CHECK(snapshot != NULL);
    memcpy(
        snapshot, initialized.workspace,
        initialized.info.workspace_required);
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(track_info.track_id == 1U);
    CHECK(track_info.sample_count == 2U);
    CHECK(track_info.coded_width == 2U);
    CHECK(track_info.color.has_nclx);
    CHECK(track_info.color.icc.size == 4U);
    CHECK((track_info.transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U);
    CHECK((track_info.transform_flags & AVIFDEC_TRANSFORM_PASP) != 0U);
    status = avif_sequence_sample_query(
        &initialized.index, 1U, 1U, &sample, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(sample.size == 3U);
    CHECK(sample.sync_sample_index == 0U);
    CHECK(sample.prepend_config == 1U);
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.main_track_id == 1U);
    CHECK(selection.presentation_count == 2U);
    status = avif_sequence_presentation_query(
        &initialized.index, &selection, 1U,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(presentation.main_sample_index == 1U);
    CHECK(presentation.main_sync_sample_index == 0U);
    CHECK(presentation.start_time == 10U);
    CHECK(presentation.duration == 10U);
    CHECK(presentation.alpha_sample_index == SIZE_MAX);
    CHECK(memcmp(
              snapshot, initialized.workspace,
              initialized.info.workspace_required) == 0);
    free(snapshot);

    stale = initialized.index;
    callbacks = validation_callbacks(&validation);
    status = avif_sequence_index_init(
        file.data, file.size, NULL, &callbacks,
        initialized.workspace, initialized.info.workspace_required,
        &initialized.index, &initialized.info, &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_sequence_track_query(
        &stale, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_INVALID_ARGUMENT);
    initialized.workspace[
        initialized.info.workspace_required - 1U] ^= 1U;
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_INVALID_ARGUMENT);
    destroy_index(&initialized);
    return 1;
}

static AvifdecStatus query_one_track(
    TestTrack *track,
    TestBuffer *file,
    AvifSequenceIndexInfo *info,
    AvifdecError *error) {
    ValidationState validation;
    AvifSequenceValidationCallbacks callbacks;

    build_classic_file(file, track, 1U, 0, 0);
    validation.tracks = track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    callbacks = validation_callbacks(&validation);
    return avif_sequence_index_query(
        file->data, file->size, NULL, &callbacks, info, error);
}

static int test_config_obu_and_rejections(void) {
    TestBuffer file;
    TestTrack track;
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSampleInfo sample;
    AvifSequenceTrackInfo track_info;
    AvifSequenceIndexInfo info;
    AvifdecError error;
    AvifdecStatus status;

    track_defaults(&track, 1U);
    track.sample_header_mode = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_sample_query(
        &initialized.index, 1U, 0U, &sample, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(sample.prepend_config == 0U);
    destroy_index(&initialized);

    track_defaults(&track, 1U);
    track.config_header = 0U;
    track.sample_header_mode = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);

    track_defaults(&track, 1U);
    track.omit_nclx = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(track_info.color.has_nclx);
    CHECK(track_info.color.color_primaries == 1U);
    CHECK(track_info.color.transfer_characteristics == 13U);
    CHECK(track_info.color.matrix_coefficients == 6U);
    destroy_index(&initialized);

    track_defaults(&track, 1U);
    track.config_header = 0U;
    track.sample_header_mode = 1U;
    track.omit_nclx = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);

    track_defaults(&track, 1U);
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = UINT64_MAX - 2U;
    {
        AvifSequenceValidationCallbacks callbacks =
            validation_callbacks(&validation);

        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
    }
    CHECK(status == AVIFDEC_INVALID_DATA);

    {
        static const uint64_t unspecified_biases[3] = {
            UINT64_MAX - 4U,
            UINT64_MAX - 5U,
            UINT64_MAX - 6U
        };
        static const uint64_t mismatch_biases[3] = {
            UINT64_MAX - 7U,
            UINT64_MAX - 8U,
            UINT64_MAX - 9U
        };
        size_t bias_index;

        for (bias_index = 0U; bias_index < 3U; ++bias_index) {
            AvifSequenceValidationCallbacks callbacks;

            validation.semantic_bias =
                unspecified_biases[bias_index];
            callbacks = validation_callbacks(&validation);
            status = avif_sequence_index_query(
                file.data, file.size, NULL, &callbacks, &info, &error);
            CHECK(status == AVIFDEC_OK);
        }
        for (bias_index = 0U; bias_index < 3U; ++bias_index) {
            AvifSequenceValidationCallbacks callbacks;

            validation.semantic_bias =
                mismatch_biases[bias_index];
            callbacks = validation_callbacks(&validation);
            status = avif_sequence_index_query(
                file.data, file.size, NULL, &callbacks, &info, &error);
            CHECK(status == AVIFDEC_INVALID_DATA);
        }
    }

    track.config_header = 0U;
    track.sample_header_mode = 1U;
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.semantic_bias = UINT64_MAX - 3U;
    {
        AvifSequenceValidationCallbacks callbacks =
            validation_callbacks(&validation);

        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
    }
    CHECK(status == AVIFDEC_OK);

    track_defaults(&track, 1U);
    track.sample_header_mode = 2U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.sample_header_mode = 3U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.config_prefix_metadata = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.config_duplicate_header = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.sample_header_mode = 4U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = UINT64_MAX - 1U;
    {
        AvifSequenceValidationCallbacks callbacks =
            validation_callbacks(&validation);

        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
    }
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.config_header = 0U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.ctts = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('c', 't', 't', 's'));

    track_defaults(&track, 1U);
    track.two_stsd_entries = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    return 1;
}

static int test_edits_and_timestamps(void) {
    TestBuffer file;
    TestTrack track;
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSelection selection;
    AvifSequencePresentationInfo presentation;
    AvifSequenceIndexInfo info;
    AvifdecError error;
    AvifdecStatus status;

    track_defaults(&track, 1U);
    track.empty_elst = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(info.edit_count == 0U);
    CHECK(info.presentation_count == 2U);

    track_defaults(&track, 1U);
    track.sample_count = 3U;
    track.media_duration = 30U;
    track.edit_count = 2U;
    track.edit_version = 1U;
    track.edits[0].segment_duration = 5U;
    track.edits[0].media_time = -1;
    track.edits[0].rate_integer = 1U;
    track.edits[1].segment_duration = 20U;
    track.edits[1].media_time = 10;
    track.edits[1].rate_integer = 1U;
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    CHECK(initialized.info.edit_count == 2U);
    CHECK(initialized.info.presentation_count == 2U);
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.duration == 25U);
    status = avif_sequence_presentation_query(
        &initialized.index, &selection, 0U,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(presentation.main_sample_index == 1U);
    CHECK(presentation.start_time == 5U);
    CHECK(presentation.duration == 10U);
    status = avif_sequence_presentation_query(
        &initialized.index, &selection, 1U,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(presentation.main_sample_index == 2U);
    CHECK(presentation.start_time == 15U);
    destroy_index(&initialized);

    track.edits[1].rate_integer = 0U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);

    track_defaults(&track, 1U);
    track.edit_count = 1U;
    track.edits[0].segment_duration = 20U;
    track.edits[0].media_time = 0;
    track.edits[0].rate_integer = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(info.edit_count == 1U);
    track.edits[0].rate_integer = UINT16_MAX;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);
    track.edits[0].rate_integer = 1U;
    track.edits[0].rate_fraction = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);

    track_defaults(&track, 1U);
    track.sample_count = 3U;
    track.media_duration = 30U;
    track.edit_count = 1U;
    track.edits[0].segment_duration = 10U;
    track.edits[0].media_time = 25;
    track.edits[0].rate_integer = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    track.edit_count = 2U;
    track.edit_version = 1U;
    track.edits[0].segment_duration = UINT64_MAX;
    track.edits[0].media_time = -1;
    track.edits[0].rate_integer = 1U;
    track.edits[1].segment_duration = 1U;
    track.edits[1].media_time = -1;
    track.edits[1].rate_integer = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OVERFLOW);

    track_defaults(&track, 1U);
    track.media_timescale = 3U;
    track.sample_duration = 1U;
    track.media_duration = 2U;
    track.movie_duration_override = 667U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.timescale == 3000U);
    CHECK(selection.duration == 2000U);
    destroy_index(&initialized);

    track_defaults(&track, 10U);
    track.tkhd_version = 1U;
    track.unknown_tkhd_duration = 1U;
    track.edit_version = 1U;
    track.edit_flags = 1U;
    track.edit_count = 1U;
    track.edits[0].segment_duration = 20U;
    track.edits[0].media_time = 0;
    track.edits[0].rate_integer = 1U;
    status = query_one_track(&track, &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    return 1;
}

static int test_matrices_multiple_tracks_and_alternates(void) {
    static const int32_t orthogonal[8][4] = {
        { 65536, 0, 0, 65536 },
        { 0, -65536, 65536, 0 },
        { -65536, 0, 0, -65536 },
        { 0, 65536, -65536, 0 },
        { -65536, 0, 0, 65536 },
        { 0, -65536, -65536, 0 },
        { 65536, 0, 0, -65536 },
        { 0, 65536, 65536, 0 }
    };
    static const uint8_t angles[8] = {
        0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U
    };
    TestBuffer file;
    TestTrack tracks[2];
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSelectOptions options;
    AvifSequenceSelection selection;
    AvifSequenceTrackInfo track_info;
    AvifSequenceTrackReferenceInfo reference;
    AvifSequenceIndexInfo info;
    AvifdecError error;
    AvifdecStatus status;
    size_t matrix_case;

    track_defaults(&tracks[0], 1U);
    tracks[0].width = 4U;
    tracks[0].height = 4U;
    tracks[0].clap = 1U;
    tracks[0].clap_width = 2U;
    tracks[0].clap_height = 2U;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = tracks;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(track_info.crop.x == 1U && track_info.crop.y == 1U);
    CHECK(track_info.crop.width == 2U &&
          track_info.crop.height == 2U);
    destroy_index(&initialized);

    for (matrix_case = 0U; matrix_case < 8U; ++matrix_case) {
        track_defaults(&tracks[0], 1U);
        tracks[0].matrix[0] = orthogonal[matrix_case][0];
        tracks[0].matrix[1] = orthogonal[matrix_case][1];
        tracks[0].matrix[3] = orthogonal[matrix_case][2];
        tracks[0].matrix[4] = orthogonal[matrix_case][3];
        status = query_one_track(
            &tracks[0], &file, &info, &error);
        CHECK(status == AVIFDEC_OK);
        validation.tracks = tracks;
        validation.track_count = 1U;
        validation.calls = 0U;
        validation.semantic_bias = 0U;
        CHECK(init_index(&file, &validation, &initialized));
        status = avif_sequence_track_query(
            &initialized.index, 0U, &track_info, &error);
        CHECK(status == AVIFDEC_OK);
        CHECK(track_info.irot_angle == angles[matrix_case]);
        CHECK(((track_info.transform_flags &
                AVIFDEC_TRANSFORM_IMIR) != 0U) ==
              (matrix_case >= 4U));
        destroy_index(&initialized);
    }

    track_defaults(&tracks[0], 1U);
    tracks[0].width = 2U;
    tracks[0].height = 3U;
    tracks[0].matrix[0] = 0;
    tracks[0].matrix[1] = -65536;
    tracks[0].matrix[3] = 65536;
    tracks[0].matrix[4] = 0;
    tracks[0].matrix[6] = 7 << 16;
    tracks[0].matrix[7] = -2 * 65536;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = tracks;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(track_info.presentation_width == 3U);
    CHECK(track_info.presentation_height == 2U);
    CHECK(track_info.irot_angle == 1U);
    destroy_index(&initialized);

    track_defaults(&tracks[0], 1U);
    tracks[0].matrix[0] = 2 * 65536;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);

    track_defaults(&tracks[0], 1U);
    tracks[0].matrix[0] = -65536;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_OK);
    validation.tracks = tracks;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_track_query(
        &initialized.index, 0U, &track_info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK((track_info.transform_flags &
           AVIFDEC_TRANSFORM_IMIR) != 0U);
    destroy_index(&initialized);

    track_defaults(&tracks[0], 1U);
    tracks[0].matrix[3] = 65536;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);
    track_defaults(&tracks[0], 1U);
    tracks[0].matrix[2] = 1;
    status = query_one_track(&tracks[0], &file, &info, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);

    track_defaults(&tracks[0], 1U);
    track_defaults(&tracks[1], 3U);
    tracks[0].alternate_group = 1U;
    tracks[1].alternate_group = 1U;
    tracks[0].refs[0].type = TEST_FOURCC('a', 'l', 't', 'r');
    tracks[0].refs[0].target = 3U;
    tracks[0].refs_count = 1U;
    build_classic_file(&file, tracks, 2U, 0, 0);
    validation.tracks = tracks;
    validation.track_count = 2U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    CHECK(initialized.info.track_count == 2U);
    CHECK(initialized.info.track_reference_count == 1U);
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_UNSUPPORTED);
    memset(&options, 0, sizeof(options));
    options.main_track_id = 1U;
    options.flags = AVIF_SEQUENCE_SELECT_DISABLE_ALPHA;
    status = avif_sequence_select(
        &initialized.index, &options, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.main_track_id == 1U);
    status = avif_sequence_track_reference_query(
        &initialized.index, 0U, &reference, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(reference.relationship_type ==
          TEST_FOURCC('a', 'l', 't', 'r'));
    destroy_index(&initialized);

    tracks[0].enabled = 0U;
    build_classic_file(&file, tracks, 2U, 0, 0);
    validation.calls = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.main_track_id == 3U);
    destroy_index(&initialized);
    return 1;
}

static int test_independent_alpha_mapping(void) {
    TestBuffer file;
    TestTrack tracks[2];
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSelection selection;
    AvifSequencePresentationInfo presentation;
    AvifdecError error;
    AvifdecStatus status;

    track_defaults(&tracks[0], 1U);
    track_defaults(&tracks[1], 2U);
    tracks[0].refs[0].type = TEST_FOURCC('p', 'r', 'e', 'm');
    tracks[0].refs[0].target = 2U;
    tracks[0].refs_count = 1U;
    tracks[1].handler = TEST_FOURCC('a', 'u', 'x', 'v');
    tracks[1].alpha = 1U;
    tracks[1].sample_count = 1U;
    tracks[1].sync_count = 1U;
    tracks[1].media_timescale = 500U;
    tracks[1].sample_duration = 10U;
    tracks[1].media_duration = 10U;
    tracks[1].refs[0].type = TEST_FOURCC('a', 'u', 'x', 'l');
    tracks[1].refs[0].target = 1U;
    tracks[1].refs_count = 1U;
    build_classic_file(&file, tracks, 2U, 0, 0);
    validation.tracks = tracks;
    validation.track_count = 2U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(selection.has_alpha);
    CHECK(selection.alpha_track_id == 2U);
    CHECK(selection.alpha_premultiplied);
    status = avif_sequence_presentation_query(
        &initialized.index, &selection, 1U,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(presentation.main_sample_index == 1U);
    CHECK(presentation.alpha_sample_index == 0U);
    CHECK(presentation.alpha_sync_sample_index == 0U);
    CHECK(presentation.image.alpha_bit_depth == 8U);
    destroy_index(&initialized);

    validation.semantic_bias = UINT64_MAX;
    {
        AvifSequenceValidationCallbacks callbacks =
            validation_callbacks(&validation);
        AvifSequenceIndexInfo info;

        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_INVALID_DATA);
    }
    tracks[1].width = 4U;
    tracks[1].height = 4U;
    tracks[1].clap = 1U;
    tracks[1].clap_width = 2U;
    tracks[1].clap_height = 2U;
    build_classic_file(&file, tracks, 2U, 0, 0);
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    destroy_index(&initialized);

    {
        TestTrack ambiguous[3];
        AvifSequenceSelectOptions options;

        track_defaults(&ambiguous[0], 1U);
        track_defaults(&ambiguous[1], 2U);
        track_defaults(&ambiguous[2], 4U);
        ambiguous[1].handler = TEST_FOURCC('a', 'u', 'x', 'v');
        ambiguous[2].handler = TEST_FOURCC('a', 'u', 'x', 'v');
        ambiguous[1].alpha = 1U;
        ambiguous[2].alpha = 1U;
        ambiguous[1].sample_count = 1U;
        ambiguous[2].sample_count = 1U;
        ambiguous[1].sample_duration = 20U;
        ambiguous[2].sample_duration = 20U;
        ambiguous[1].media_duration = 20U;
        ambiguous[2].media_duration = 20U;
        ambiguous[1].sync_count = 1U;
        ambiguous[2].sync_count = 1U;
        ambiguous[1].refs[0].type =
            TEST_FOURCC('a', 'u', 'x', 'l');
        ambiguous[2].refs[0].type =
            TEST_FOURCC('a', 'u', 'x', 'l');
        ambiguous[1].refs[0].target = 1U;
        ambiguous[2].refs[0].target = 1U;
        ambiguous[1].refs_count = 1U;
        ambiguous[2].refs_count = 1U;
        build_classic_file(&file, ambiguous, 3U, 0, 0);
        validation.tracks = ambiguous;
        validation.track_count = 3U;
        validation.calls = 0U;
        validation.semantic_bias = 0U;
        CHECK(init_index(&file, &validation, &initialized));
        status = avif_sequence_select(
            &initialized.index, NULL, &selection, &error);
        CHECK(status == AVIFDEC_UNSUPPORTED);
        memset(&options, 0, sizeof(options));
        options.main_track_id = 1U;
        options.alpha_track_id = 2U;
        status = avif_sequence_select(
            &initialized.index, &options, &selection, &error);
        CHECK(status == AVIFDEC_OK);
        CHECK(selection.alpha_track_id == 2U);
        options.alpha_track_id = 0U;
        options.flags = AVIF_SEQUENCE_SELECT_DISABLE_ALPHA;
        status = avif_sequence_select(
            &initialized.index, &options, &selection, &error);
        CHECK(status == AVIFDEC_OK);
        CHECK(!selection.has_alpha);
        destroy_index(&initialized);
    }
    return 1;
}

typedef struct {
    uint8_t second_fragment;
    uint8_t discontinuity;
    uint8_t sequence_gap;
    uint8_t sequence_regress;
    uint8_t composition_offset;
    uint8_t malformed_range;
    uint8_t first_non_sync;
    uint8_t tfhd_defaults;
    uint8_t explicit_base;
    uint8_t trun_fields;
    uint8_t zero_movie_duration;
    uint8_t invalid_trex_flags;
    uint8_t invalid_tfhd_flags;
    uint8_t invalid_trun_flags;
    uint8_t reserved_trun_flags;
    uint8_t nonexistent_trex_id;
    uint8_t nonexistent_tfhd_id;
} FragmentOptions;

static void put_mvex(
    TestBuffer *buffer,
    const TestTrack *track,
    int tfhd_defaults,
    int invalid_flags,
    int nonexistent_track) {
    size_t mvex = box_begin(buffer, TEST_FOURCC('m', 'v', 'e', 'x'));
    size_t trex = box_begin(buffer, TEST_FOURCC('t', 'r', 'e', 'x'));

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(
        buffer, nonexistent_track ? 99U : track->id);
    buffer_put_u32(buffer, 1U);
    buffer_put_u32(
        buffer, tfhd_defaults ? 0U : track->sample_duration);
    buffer_put_u32(buffer, tfhd_defaults ? 0U : 3U);
    buffer_put_u32(buffer, invalid_flags ? 0x04000000U : 0U);
    box_end(buffer, trex);
    box_end(buffer, mvex);
}

static size_t append_fragment(
    TestBuffer *buffer,
    const TestTrack *track,
    uint32_t sequence_number,
    uint64_t decode_time,
    uint32_t sample_count,
    const FragmentOptions *options) {
    size_t moof = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'f'));
    size_t mfhd = box_begin(buffer, TEST_FOURCC('m', 'f', 'h', 'd'));
    size_t traf;
    size_t tfhd;
    size_t tfdt;
    size_t trun;
    size_t data_offset_position = SIZE_MAX;
    size_t base_offset_position = SIZE_MAX;
    size_t mdat;
    size_t data_start;
    uint32_t tfhd_flags =
        options->explicit_base ? 1U : 0x020000U;
    uint32_t trun_flags = options->explicit_base ? 0U : 1U;
    uint32_t sample;

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, sequence_number);
    box_end(buffer, mfhd);
    traf = box_begin(buffer, TEST_FOURCC('t', 'r', 'a', 'f'));
    if (options->tfhd_defaults) tfhd_flags |= 0x38U;
    if (options->invalid_tfhd_flags) tfhd_flags |= 0x20U;
    tfhd = box_begin(buffer, TEST_FOURCC('t', 'f', 'h', 'd'));
    buffer_put_u32(buffer, tfhd_flags);
    buffer_put_u32(
        buffer, options->nonexistent_tfhd_id ? 99U : track->id);
    if (options->explicit_base) {
        base_offset_position = buffer->size;
        buffer_put_u64(buffer, 0U);
    }
    if ((tfhd_flags & 8U) != 0U) {
        buffer_put_u32(buffer, track->sample_duration);
    }
    if ((tfhd_flags & 0x10U) != 0U) {
        buffer_put_u32(buffer, 3U);
    }
    if ((tfhd_flags & 0x20U) != 0U) {
        buffer_put_u32(
            buffer,
            options->invalid_tfhd_flags ? 0x04000000U : 0U);
    }
    box_end(buffer, tfhd);
    tfdt = box_begin(buffer, TEST_FOURCC('t', 'f', 'd', 't'));
    buffer_put_u32(buffer, 0x01000000U);
    buffer_put_u64(buffer, decode_time);
    box_end(buffer, tfdt);
    if (options->composition_offset) trun_flags |= 0x800U;
    if (options->first_non_sync) trun_flags |= 4U;
    if (options->trun_fields) trun_flags |= 0x300U;
    if (options->invalid_trun_flags ||
        options->reserved_trun_flags) {
        trun_flags |= 0x400U;
    }
    trun = box_begin(buffer, TEST_FOURCC('t', 'r', 'u', 'n'));
    buffer_put_u32(buffer, trun_flags);
    buffer_put_u32(buffer, sample_count);
    if (!options->explicit_base) {
        data_offset_position = buffer->size;
        buffer_put_u32(buffer, 0U);
    }
    if (options->first_non_sync) {
        buffer_put_u32(buffer, 0x00010000U);
    }
    for (sample = 0U; sample < sample_count; ++sample) {
        if (options->trun_fields) {
            buffer_put_u32(buffer, track->sample_duration);
            buffer_put_u32(buffer, 3U);
        }
        if (options->invalid_trun_flags ||
            options->reserved_trun_flags) {
            buffer_put_u32(
                buffer,
                options->invalid_trun_flags
                    ? options->invalid_trun_flags == 1U
                        ? 0x04000000U : 0x0c000000U
                    : 0x03000000U);
        }
        if (options->composition_offset) {
            buffer_put_u32(buffer, 0U);
        }
    }
    box_end(buffer, trun);
    box_end(buffer, traf);
    box_end(buffer, moof);
    mdat = box_begin(buffer, TEST_FOURCC('m', 'd', 'a', 't'));
    data_start = buffer->size;
    for (sample = 0U; sample < sample_count; ++sample) {
        put_sample(buffer, track);
    }
    box_end(buffer, mdat);
    if (options->explicit_base) {
        buffer_patch_u64(
            buffer, base_offset_position,
            options->malformed_range
                ? buffer->size + 100U : data_start);
    } else if (options->malformed_range) {
        buffer_patch_u32(
            buffer, data_offset_position,
            (uint32_t)(buffer->size + 100U - moof));
    } else {
        buffer_patch_u32(
            buffer, data_offset_position,
            (uint32_t)(data_start - moof));
    }
    return data_start;
}

static void build_fragment_file(
    TestBuffer *buffer,
    TestTrack *track,
    const FragmentOptions *options) {
    BuildPatches patches;
    size_t moov;

    memset(buffer, 0, sizeof(*buffer));
    memset(&patches, 0, sizeof(patches));
    track_defaults(track, 1U);
    track->sample_count = 0U;
    track->sync_count = 0U;
    track->media_duration = 0U;
    put_ftyp(buffer);
    moov = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'v'));
    put_mvhd(
        buffer, 1000U,
        options->zero_movie_duration ? 0U : 20U, 1);
    put_track(buffer, track, 0U, &patches);
    put_mvex(
        buffer, track, options->tfhd_defaults,
        options->invalid_trex_flags,
        options->nonexistent_trex_id);
    box_end(buffer, moov);
    if (options->second_fragment) {
        (void)append_fragment(
            buffer, track, 1U, 0U, 1U, options);
        (void)append_fragment(
            buffer, track,
            options->sequence_regress
                ? 1U : options->sequence_gap ? 3U : 2U,
            options->discontinuity ? 11U : 10U,
            1U, options);
    } else {
        (void)append_fragment(
            buffer, track, 1U, 0U, 2U, options);
    }
}

static AvifdecStatus query_fragment(
    TestBuffer *file,
    TestTrack *track,
    const FragmentOptions *options,
    AvifSequenceIndexInfo *info,
    AvifdecError *error) {
    ValidationState validation;
    AvifSequenceValidationCallbacks callbacks;

    build_fragment_file(file, track, options);
    validation.tracks = track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    callbacks = validation_callbacks(&validation);
    return avif_sequence_index_query(
        file->data, file->size, NULL, &callbacks, info, error);
}

static size_t put_interleaved_traf(
    TestBuffer *buffer,
    const TestTrack *track) {
    size_t traf = box_begin(buffer, TEST_FOURCC('t', 'r', 'a', 'f'));
    size_t box = box_begin(buffer, TEST_FOURCC('t', 'f', 'h', 'd'));
    size_t patch;

    buffer_put_u32(buffer, 0x020000U);
    buffer_put_u32(buffer, track->id);
    box_end(buffer, box);
    box = box_begin(buffer, TEST_FOURCC('t', 'f', 'd', 't'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    box_end(buffer, box);
    box = box_begin(buffer, TEST_FOURCC('t', 'r', 'u', 'n'));
    buffer_put_u32(buffer, 1U);
    buffer_put_u32(buffer, 1U);
    patch = buffer->size;
    buffer_put_u32(buffer, 0U);
    box_end(buffer, box);
    box_end(buffer, traf);
    return patch;
}

static void build_interleaved_fragment_file(
    TestBuffer *buffer,
    TestTrack tracks[2]) {
    BuildPatches patches;
    size_t moov;
    size_t mvex;
    size_t moof;
    size_t box;
    size_t patch0;
    size_t patch1;
    size_t mdat;
    size_t start0;
    size_t start1;
    size_t index;

    memset(buffer, 0, sizeof(*buffer));
    memset(&patches, 0, sizeof(patches));
    for (index = 0U; index < 2U; ++index) {
        track_defaults(&tracks[index], (uint32_t)(index + 1U));
        tracks[index].sample_count = 0U;
        tracks[index].sync_count = 0U;
        tracks[index].media_duration = 0U;
    }
    put_ftyp(buffer);
    moov = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'v'));
    put_mvhd(buffer, 1000U, 10U, 0);
    put_track(buffer, &tracks[0], 0U, &patches);
    put_track(buffer, &tracks[1], 1U, &patches);
    mvex = box_begin(buffer, TEST_FOURCC('m', 'v', 'e', 'x'));
    for (index = 0U; index < 2U; ++index) {
        box = box_begin(buffer, TEST_FOURCC('t', 'r', 'e', 'x'));
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, tracks[index].id);
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, 10U);
        buffer_put_u32(buffer, 3U);
        buffer_put_u32(buffer, 0U);
        box_end(buffer, box);
    }
    box_end(buffer, mvex);
    box_end(buffer, moov);
    moof = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'f'));
    box = box_begin(buffer, TEST_FOURCC('m', 'f', 'h', 'd'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 1U);
    box_end(buffer, box);
    patch0 = put_interleaved_traf(buffer, &tracks[0]);
    patch1 = put_interleaved_traf(buffer, &tracks[1]);
    box_end(buffer, moof);
    mdat = box_begin(buffer, TEST_FOURCC('m', 'd', 'a', 't'));
    start1 = buffer->size;
    put_sample(buffer, &tracks[1]);
    start0 = buffer->size;
    put_sample(buffer, &tracks[0]);
    box_end(buffer, mdat);
    buffer_patch_u32(buffer, patch0, (uint32_t)(start0 - moof));
    buffer_patch_u32(buffer, patch1, (uint32_t)(start1 - moof));
}

static size_t put_implicit_traf(
    TestBuffer *buffer,
    const TestTrack *track,
    int data_offset_present) {
    size_t traf = box_begin(buffer, TEST_FOURCC('t', 'r', 'a', 'f'));
    size_t box = box_begin(buffer, TEST_FOURCC('t', 'f', 'h', 'd'));
    size_t patch = SIZE_MAX;

    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, track->id);
    box_end(buffer, box);
    box = box_begin(buffer, TEST_FOURCC('t', 'f', 'd', 't'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 0U);
    box_end(buffer, box);
    box = box_begin(buffer, TEST_FOURCC('t', 'r', 'u', 'n'));
    buffer_put_u32(buffer, data_offset_present ? 1U : 0U);
    buffer_put_u32(buffer, 1U);
    if (data_offset_present) {
        patch = buffer->size;
        buffer_put_u32(buffer, 0U);
    }
    box_end(buffer, box);
    box_end(buffer, traf);
    return patch;
}

static void build_implicit_base_fragment_file(
    TestBuffer *buffer,
    TestTrack tracks[2]) {
    BuildPatches patches;
    size_t moov;
    size_t mvex;
    size_t moof;
    size_t box;
    size_t unknown_patch;
    size_t mdat;
    size_t unknown_start;
    size_t index;

    memset(buffer, 0, sizeof(*buffer));
    memset(&patches, 0, sizeof(patches));
    track_defaults(&tracks[0], 9U);
    tracks[0].handler = TEST_FOURCC('s', 'o', 'u', 'n');
    track_defaults(&tracks[1], 1U);
    for (index = 0U; index < 2U; ++index) {
        tracks[index].sample_count = 0U;
        tracks[index].sync_count = 0U;
        tracks[index].media_duration = 0U;
    }
    put_ftyp(buffer);
    moov = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'v'));
    put_mvhd(buffer, 1000U, 10U, 0);
    put_track(buffer, &tracks[0], 0U, &patches);
    put_track(buffer, &tracks[1], 1U, &patches);
    mvex = box_begin(buffer, TEST_FOURCC('m', 'v', 'e', 'x'));
    for (index = 0U; index < 2U; ++index) {
        box = box_begin(buffer, TEST_FOURCC('t', 'r', 'e', 'x'));
        buffer_put_u32(buffer, 0U);
        buffer_put_u32(buffer, tracks[index].id);
        buffer_put_u32(buffer, 1U);
        buffer_put_u32(buffer, 10U);
        buffer_put_u32(buffer, 3U);
        buffer_put_u32(buffer, 0U);
        box_end(buffer, box);
    }
    box_end(buffer, mvex);
    box_end(buffer, moov);
    moof = box_begin(buffer, TEST_FOURCC('m', 'o', 'o', 'f'));
    box = box_begin(buffer, TEST_FOURCC('m', 'f', 'h', 'd'));
    buffer_put_u32(buffer, 0U);
    buffer_put_u32(buffer, 1U);
    box_end(buffer, box);
    unknown_patch = put_implicit_traf(buffer, &tracks[0], 1);
    (void)put_implicit_traf(buffer, &tracks[1], 0);
    box_end(buffer, moof);
    mdat = box_begin(buffer, TEST_FOURCC('m', 'd', 'a', 't'));
    unknown_start = buffer->size;
    buffer_put_u8(buffer, 0U);
    buffer_put_u8(buffer, 0U);
    buffer_put_u8(buffer, 0U);
    put_sample(buffer, &tracks[1]);
    box_end(buffer, mdat);
    buffer_patch_u32(
        buffer, unknown_patch,
        (uint32_t)(unknown_start - moof));
}

static int test_fragments_defaults_offsets_and_malformed(void) {
    TestBuffer file;
    TestTrack track;
    FragmentOptions options;
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSelection selection;
    AvifSequencePresentationInfo presentation;
    AvifSequenceSampleInfo sample;
    AvifSequenceIndexInfo info;
    AvifdecError error;
    AvifdecStatus status;

    memset(&options, 0, sizeof(options));
    options.tfhd_defaults = 1U;
    build_fragment_file(&file, &track, &options);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    CHECK(initialized.info.fragment_count == 1U);
    CHECK(initialized.info.sample_count == 2U);
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    status = avif_sequence_presentation_query(
        &initialized.index, &selection, 1U,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK((presentation.flags &
           AVIF_SEQUENCE_PRESENTATION_FRAGMENTED) != 0U);
    status = avif_sequence_sample_query(
        &initialized.index, 1U, 1U, &sample, &error);
    CHECK(status == AVIFDEC_OK && sample.fragmented);
    destroy_index(&initialized);

    {
        AvifSequenceValidationCallbacks callbacks =
            validation_callbacks(&validation);
        size_t tkhd = buffer_find_type(
            &file, TEST_FOURCC('t', 'k', 'h', 'd'));

        CHECK(tkhd != SIZE_MAX);
        buffer_patch_u32(&file, tkhd + 24U, 0U);
        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_INVALID_DATA);
    }

    memset(&options, 0, sizeof(options));
    options.explicit_base = 1U;
    options.trun_fields = 1U;
    options.zero_movie_duration = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(info.fragment_count == 1U);

    memset(&options, 0, sizeof(options));
    options.second_fragment = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(info.fragment_count == 2U);

    options.discontinuity = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    options.discontinuity = 0U;
    options.sequence_gap = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_OK);
    options.sequence_gap = 0U;
    options.sequence_regress = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    memset(&options, 0, sizeof(options));
    options.composition_offset = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'r', 'u', 'n'));

    memset(&options, 0, sizeof(options));
    options.malformed_range = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    memset(&options, 0, sizeof(options));
    options.first_non_sync = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    memset(&options, 0, sizeof(options));
    options.invalid_trex_flags = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'r', 'e', 'x'));

    memset(&options, 0, sizeof(options));
    options.invalid_tfhd_flags = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'f', 'h', 'd'));

    memset(&options, 0, sizeof(options));
    options.invalid_trun_flags = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'r', 'u', 'n'));
    options.invalid_trun_flags = 2U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    memset(&options, 0, sizeof(options));
    options.reserved_trun_flags = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    memset(&options, 0, sizeof(options));
    options.nonexistent_trex_id = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'r', 'e', 'x'));

    memset(&options, 0, sizeof(options));
    options.nonexistent_tfhd_id = 1U;
    status = query_fragment(
        &file, &track, &options, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(error.context == TEST_FOURCC('t', 'f', 'h', 'd'));

    {
        TestTrack interleaved[2];
        AvifSequenceValidationCallbacks callbacks;

        build_interleaved_fragment_file(&file, interleaved);
        validation.tracks = interleaved;
        validation.track_count = 2U;
        validation.calls = 0U;
        validation.semantic_bias = 0U;
        callbacks = validation_callbacks(&validation);
        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_OK);
        CHECK(info.fragment_count == 2U);
        CHECK(info.sample_count == 2U);
    }
    {
        TestTrack implicit[2];
        AvifSequenceValidationCallbacks callbacks;

        build_implicit_base_fragment_file(&file, implicit);
        validation.tracks = implicit;
        validation.track_count = 2U;
        validation.calls = 0U;
        validation.semantic_bias = 0U;
        callbacks = validation_callbacks(&validation);
        status = avif_sequence_index_query(
            file.data, file.size, NULL, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_OK);
        CHECK(info.track_count == 1U);
        CHECK(info.fragment_count == 1U);
        CHECK(info.sample_count == 1U);
    }
    return 1;
}

typedef struct {
    size_t query_calls;
    size_t decode_calls;
    size_t observed_spans;
    uint32_t alpha_width;
    uint32_t alpha_height;
    uint8_t fail_alpha;
} DecodeState;

static AvifdecStatus replay_query_callback(
    void *user_data,
    const AvifSequenceReplay *replay,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    AvifdecError *error) {
    DecodeState *state = (DecodeState *)user_data;
    size_t index;

    (void)limits;
    (void)executor;
    for (index = 0U; index < replay->span_count; ++index) {
        AvifdecSpan span;
        size_t sample_index;
        AvifdecStatus status = avif_sequence_replay_span_query(
            replay, index, &span, &sample_index, error);

        if (status != AVIFDEC_OK || span.size == 0U) return status;
        if (index == 0U && replay->prepend_config != 0U &&
            sample_index != SIZE_MAX) {
            return AVIFDEC_INVALID_DATA;
        }
        ++state->observed_spans;
    }
    memset(info, 0, sizeof(*info));
    info->width = replay->alpha
        ? state->alpha_width : 2U;
    info->height = replay->alpha
        ? state->alpha_height : 2U;
    info->bit_depth = 8U;
    info->monochrome = replay->alpha;
    info->color_range = 1U;
    info->workspace_required = replay->alpha ? 96U : 64U;
    ++state->query_calls;
    return AVIFDEC_OK;
}

static AvifdecStatus replay_decode_callback(
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
    DecodeState *state = (DecodeState *)user_data;
    size_t index;

    (void)limits;
    (void)executor;
    (void)workspace;
    (void)workspace_size;
    (void)error;
    if (image->planes[0] != NULL) {
        image->planes[0][0] =
            replay->alpha != 0U ? 222U : 111U;
    }
    if (replay->alpha != 0U && state->fail_alpha != 0U) {
        return AVIFDEC_INVALID_DATA;
    }
    for (index = 0U; index < replay->span_count; ++index) {
        AvifdecSpan span;
        size_t sample_index;
        AvifdecStatus status = avif_sequence_replay_span_query(
            replay, index, &span, &sample_index, error);

        (void)sample_index;
        if (status != AVIFDEC_OK || span.size == 0U) return status;
    }
    image->widths[0] = info->width;
    image->heights[0] = info->height;
    image->bit_depth = info->bit_depth;
    image->monochrome = info->monochrome;
    trace->frame_count = 1U;
    ++state->decode_calls;
    return AVIFDEC_OK;
}

static int test_replay_plan_and_decode_commit(void) {
    TestBuffer file;
    TestTrack tracks[2];
    ValidationState validation;
    InitializedIndex initialized;
    AvifSequenceSelection selection;
    AvifSequenceDecodeCallbacks callbacks;
    AvifSequenceDecodePlan plan;
    AvifSequencePresentationInfo presentation;
    AvifdecImage image;
    AvifdecImage original_image;
    AvifdecEntropyTrace trace;
    AvifdecError error;
    DecodeState decode_state;
    unsigned char workspace[96];
    uint16_t main_plane[4];
    uint16_t alpha_plane[4];
    AvifdecStatus status;

    track_defaults(&tracks[0], 1U);
    track_defaults(&tracks[1], 2U);
    tracks[1].handler = TEST_FOURCC('a', 'u', 'x', 'v');
    tracks[1].alpha = 1U;
    tracks[1].sample_count = 1U;
    tracks[1].sync_count = 1U;
    tracks[1].sample_duration = 20U;
    tracks[1].media_duration = 20U;
    tracks[1].refs[0].type = TEST_FOURCC('a', 'u', 'x', 'l');
    tracks[1].refs[0].target = 1U;
    tracks[1].refs_count = 1U;
    build_classic_file(&file, tracks, 2U, 0, 0);
    validation.tracks = tracks;
    validation.track_count = 2U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    CHECK(init_index(&file, &validation, &initialized));
    status = avif_sequence_select(
        &initialized.index, NULL, &selection, &error);
    CHECK(status == AVIFDEC_OK);
    memset(&decode_state, 0, sizeof(decode_state));
    decode_state.alpha_width = 2U;
    decode_state.alpha_height = 2U;
    callbacks.user_data = &decode_state;
    callbacks.query = replay_query_callback;
    callbacks.decode = replay_decode_callback;
    status = avif_sequence_decode_plan_query(
        &initialized.index, &selection, NULL, 1U,
        &callbacks, &plan, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(plan.workspace_required == 96U);
    CHECK(plan.main.first_sample_index == 0U);
    CHECK(plan.main.last_sample_index == 1U);
    CHECK(plan.main.span_count == 3U);
    CHECK(plan.alpha.first_sample_index == 0U);
    CHECK(plan.alpha.last_sample_index == 0U);
    CHECK(plan.alpha.span_count == 2U);
    CHECK(plan.alpha_image.width == 2U);
    CHECK(plan.alpha_image.height == 2U);
    CHECK(plan.alpha_image.crop.x == 0U);
    CHECK(plan.alpha_image.crop.width == 2U);

    memset(&image, 0, sizeof(image));
    memset(main_plane, 0, sizeof(main_plane));
    memset(alpha_plane, 0, sizeof(alpha_plane));
    image.planes[0] = main_plane;
    image.strides[0] = 2U;
    image.alpha_plane = alpha_plane;
    image.alpha_stride = 2U;
    image.widths[0] = 77U;
    original_image = image;
    memset(&presentation, 0x5a, sizeof(presentation));
    status = avif_sequence_decode_presentation(
        &initialized.index, &selection, NULL, 1U,
        &callbacks, workspace, 95U, &image, &trace,
        &presentation, &error);
    CHECK(status == AVIFDEC_OUT_OF_MEMORY);
    CHECK(image.widths[0] == original_image.widths[0]);
    CHECK(presentation.presentation_index != 1U);

    decode_state.fail_alpha = 1U;
    status = avif_sequence_decode_presentation(
        &initialized.index, &selection, NULL, 1U,
        &callbacks, workspace, sizeof(workspace), &image, &trace,
        &presentation, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    CHECK(image.widths[0] == original_image.widths[0]);
    CHECK(presentation.presentation_index != 1U);
    CHECK(main_plane[0] == 111U);
    CHECK(alpha_plane[0] == 222U);

    decode_state.fail_alpha = 0U;
    status = avif_sequence_decode_presentation(
        &initialized.index, &selection, NULL, 1U,
        &callbacks, workspace, sizeof(workspace), &image, &trace,
        &presentation, &error);
    CHECK(status == AVIFDEC_OK);
    CHECK(image.widths[0] == 2U);
    CHECK(image.alpha_width == 2U);
    CHECK(image.alpha_height == 2U);
    CHECK(presentation.presentation_index == 1U);
    CHECK(trace.frame_count == 1U);
    CHECK(decode_state.decode_calls == 3U);
    destroy_index(&initialized);
    return 1;
}

static int test_mandatory_box_bodies(void) {
    TestBuffer file;
    TestTrack track;
    ValidationState validation;
    AvifSequenceValidationCallbacks callbacks;
    AvifSequenceIndexInfo info;
    AvifdecError error;
    AvifdecStatus status;
    size_t position;

    track_defaults(&track, 1U);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    callbacks = validation_callbacks(&validation);

    build_classic_file(&file, &track, 1U, 0, 0);
    position = buffer_find_type(
        &file, TEST_FOURCC('m', 'v', 'h', 'd'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u32(&file, position + 24U, 0U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    build_classic_file(&file, &track, 1U, 0, 0);
    position = buffer_find_type(
        &file, TEST_FOURCC('m', 'd', 'h', 'd'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u16(&file, position + 24U, 0U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    build_classic_file(&file, &track, 1U, 0, 0);
    position = buffer_find_type(
        &file, TEST_FOURCC('h', 'd', 'l', 'r'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u32(&file, position + 16U, 1U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    build_classic_file(&file, &track, 1U, 0, 0);
    position = buffer_find_type(
        &file, TEST_FOURCC('a', 'v', '0', '1'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u32(&file, position + 32U, 0U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track_defaults(&track, 1U);
    build_classic_file(&file, &track, 1U, 0, 0);
    position = buffer_find_type(
        &file, TEST_FOURCC('t', 'k', 'h', 'd'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u32(&file, position + 24U, 21U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);

    track.tkhd_version = 1U;
    build_classic_file(&file, &track, 1U, 0, 0);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_OK);
    position = buffer_find_type(
        &file, TEST_FOURCC('t', 'k', 'h', 'd'));
    CHECK(position != SIZE_MAX);
    buffer_patch_u64(&file, position + 32U, 21U);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    return 1;
}

static int test_configured_limits_and_corruption(void) {
    TestBuffer file;
    TestTrack track;
    ValidationState validation;
    AvifSequenceValidationCallbacks callbacks;
    AvifSequenceIndexInfo info;
    AvifdecLimits limits;
    AvifdecError error;
    AvifdecStatus status;
    size_t position;

    track_defaults(&track, 1U);
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    validation.calls = 0U;
    validation.semantic_bias = 0U;
    callbacks = validation_callbacks(&validation);
    memset(&limits, 0, sizeof(limits));
    limits.max_frames = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    memset(&limits, 0, sizeof(limits));
    limits.max_width = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    memset(&limits, 0, sizeof(limits));
    limits.max_height = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    memset(&limits, 0, sizeof(limits));
    limits.max_pixels = 3U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    memset(&limits, 0, sizeof(limits));
    limits.max_obus = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    memset(&limits, 0, sizeof(limits));
    limits.max_properties = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    track_defaults(&track, 1U);
    track.edit_count = 2U;
    track.edits[0].segment_duration = 10U;
    track.edits[0].media_time = 0;
    track.edits[0].rate_integer = 1U;
    track.edits[1].segment_duration = 10U;
    track.edits[1].media_time = 10;
    track.edits[1].rate_integer = 1U;
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    callbacks = validation_callbacks(&validation);
    memset(&limits, 0, sizeof(limits));
    limits.max_edits = 1U;
    status = avif_sequence_index_query(
        file.data, file.size, &limits, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_LIMIT_EXCEEDED);

    {
        TestTrack tracks[2];

        track_defaults(&tracks[0], 1U);
        track_defaults(&tracks[1], 3U);
        build_classic_file(&file, tracks, 2U, 0, 0);
        validation.tracks = tracks;
        validation.track_count = 2U;
        callbacks = validation_callbacks(&validation);
        memset(&limits, 0, sizeof(limits));
        limits.max_tracks = 1U;
        status = avif_sequence_index_query(
            file.data, file.size, &limits, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_LIMIT_EXCEEDED);
    }

    {
        FragmentOptions fragment_options;

        memset(&fragment_options, 0, sizeof(fragment_options));
        fragment_options.second_fragment = 1U;
        build_fragment_file(&file, &track, &fragment_options);
        validation.tracks = &track;
        validation.track_count = 1U;
        callbacks = validation_callbacks(&validation);
        memset(&limits, 0, sizeof(limits));
        limits.max_extents = 1U;
        status = avif_sequence_index_query(
            file.data, file.size, &limits, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_LIMIT_EXCEEDED);
        memset(&limits, 0, sizeof(limits));
        limits.max_fragments = 1U;
        status = avif_sequence_index_query(
            file.data, file.size, &limits, &callbacks, &info, &error);
        CHECK(status == AVIFDEC_LIMIT_EXCEEDED);
    }

    track_defaults(&track, 1U);
    track.co64 = 1U;
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    callbacks = validation_callbacks(&validation);
    for (position = 4U; position + 20U < file.size; ++position) {
        if (file.data[position] == 'c' &&
            file.data[position + 1U] == 'o' &&
            file.data[position + 2U] == '6' &&
            file.data[position + 3U] == '4') {
            buffer_patch_u64(&file, position + 12U, UINT64_MAX);
            break;
        }
    }
    CHECK(position + 20U < file.size);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_OVERFLOW);

    track_defaults(&track, 1U);
    build_classic_file(&file, &track, 1U, 0, 0);
    validation.tracks = &track;
    validation.track_count = 1U;
    callbacks = validation_callbacks(&validation);
    for (position = 4U; position + 8U < file.size; ++position) {
        if (file.data[position] == 's' &&
            file.data[position + 1U] == 't' &&
            file.data[position + 2U] == 's' &&
            file.data[position + 3U] == 'd') {
            buffer_patch_u32(&file, position + 8U, 2U);
            break;
        }
    }
    CHECK(position + 8U < file.size);
    status = avif_sequence_index_query(
        file.data, file.size, NULL, &callbacks, &info, &error);
    CHECK(status == AVIFDEC_INVALID_DATA);
    return 1;
}

int main(void) {
    int passed = 1;

    passed &= test_child_iterator();
    passed &= test_classic_tables_workspace_and_stale();
    passed &= test_config_obu_and_rejections();
    passed &= test_edits_and_timestamps();
    passed &= test_matrices_multiple_tracks_and_alternates();
    passed &= test_independent_alpha_mapping();
    passed &= test_fragments_defaults_offsets_and_malformed();
    passed &= test_replay_plan_and_decode_commit();
    passed &= test_mandatory_box_bodies();
    passed &= test_configured_limits_and_corruption();
    if (!passed) return 1;
    printf("sequence index private tests passed\n");
    return 0;
}
