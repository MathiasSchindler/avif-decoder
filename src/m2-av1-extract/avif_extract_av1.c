#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// m2 goal: extract primary AV1 Image Item Data (av01) byte-for-byte and perform minimal OBU validation.
// No external dependencies.

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: avif_extract_av1 <in.avif> <out.av1>\n"
            "\n"
            "Extracts the primary 'av01' item payload and performs a minimal OBU scan.\n");
}

static bool read_exact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

static bool file_seek(FILE *f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}

static bool file_tell(FILE *f, uint64_t *out) {
#if defined(_WIN32)
    long long p = _ftelli64(f);
    if (p < 0) {
        return false;
    }
    *out = (uint64_t)p;
    return true;
#else
    off_t p = ftello(f);
    if (p < 0) {
        return false;
    }
    *out = (uint64_t)p;
    return true;
#endif
}

static bool get_file_size(FILE *f, uint64_t *out_size) {
    uint64_t cur;
    if (!file_tell(f, &cur)) {
        return false;
    }
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        return false;
    }
    long long end = _ftelli64(f);
    if (end < 0) {
        return false;
    }
    *out_size = (uint64_t)end;
    return _fseeki64(f, (long long)cur, SEEK_SET) == 0;
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        return false;
    }
    off_t end = ftello(f);
    if (end < 0) {
        return false;
    }
    *out_size = (uint64_t)end;
    return fseeko(f, (off_t)cur, SEEK_SET) == 0;
#endif
}

static uint32_t read_u32_be_from_buf(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint64_t read_u64_be_from_buf(const uint8_t b[8]) {
    return ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
           ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) | ((uint64_t)b[6] << 8) | (uint64_t)b[7];
}

static bool read_u8(FILE *f, uint8_t *out) {
    return read_exact(f, out, 1);
}

static bool read_u16_be(FILE *f, uint16_t *out) {
    uint8_t b[2];
    if (!read_exact(f, b, 2)) {
        return false;
    }
    *out = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
    return true;
}

static bool read_u32_be(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (!read_exact(f, b, 4)) {
        return false;
    }
    *out = read_u32_be_from_buf(b);
    return true;
}

static bool read_be_n(FILE *f, uint64_t nbytes, uint64_t *out) {
    if (nbytes > 8) {
        return false;
    }
    if (nbytes == 0) {
        *out = 0;
        return true;
    }
    uint8_t buf[8] = {0};
    if (!read_exact(f, buf, (size_t)nbytes)) {
        return false;
    }
    uint64_t v = 0;
    for (uint64_t i = 0; i < nbytes; i++) {
        v = (v << 8) | (uint64_t)buf[i];
    }
    *out = v;
    return true;
}

static bool read_fullbox_header(FILE *f, uint8_t *out_version, uint32_t *out_flags) {
    uint8_t vf[4];
    if (!read_exact(f, vf, 4)) {
        return false;
    }
    *out_version = vf[0];
    *out_flags = ((uint32_t)vf[1] << 16) | ((uint32_t)vf[2] << 8) | (uint32_t)vf[3];
    return true;
}

static bool type_equals(const char t[4], const char *lit4) {
    return memcmp(t, lit4, 4) == 0;
}

typedef struct {
    uint64_t offset;
    uint64_t size;
    char type[4];
    uint64_t header_size;
    bool has_uuid;
    uint8_t uuid[16];
} BoxHdr;

static bool read_box_header(FILE *f, uint64_t file_size, uint64_t parent_end, BoxHdr *out, char *err, size_t err_cap) {
    memset(out, 0, sizeof(*out));

    uint64_t start;
    if (!file_tell(f, &start)) {
        snprintf(err, err_cap, "ftell failed: %s", strerror(errno));
        return false;
    }

    if (start + 8 > parent_end || start + 8 > file_size) {
        snprintf(err, err_cap, "truncated box header at offset=%" PRIu64, start);
        return false;
    }

    uint8_t header8[8];
    if (!read_exact(f, header8, sizeof(header8))) {
        snprintf(err, err_cap, "read failed at offset=%" PRIu64 ": %s", start, strerror(errno));
        return false;
    }

    uint32_t size32 = read_u32_be_from_buf(header8);
    memcpy(out->type, header8 + 4, 4);
    out->offset = start;
    out->header_size = 8;

    uint64_t box_size = 0;
    if (size32 == 0) {
        box_size = parent_end - start;
    } else if (size32 == 1) {
        if (start + 16 > parent_end || start + 16 > file_size) {
            snprintf(err, err_cap, "truncated largesize at offset=%" PRIu64, start);
            return false;
        }
        uint8_t size8[8];
        if (!read_exact(f, size8, sizeof(size8))) {
            snprintf(err, err_cap, "read largesize failed at offset=%" PRIu64, start);
            return false;
        }
        box_size = read_u64_be_from_buf(size8);
        out->header_size = 16;
    } else {
        box_size = (uint64_t)size32;
    }

    if (box_size < out->header_size) {
        snprintf(err, err_cap, "invalid box size=%" PRIu64 " < header_size=%" PRIu64 " at offset=%" PRIu64,
                 box_size, out->header_size, start);
        return false;
    }

    if (start + box_size > parent_end || start + box_size > file_size) {
        snprintf(err, err_cap, "box overruns parent/file: offset=%" PRIu64 " size=%" PRIu64, start, box_size);
        return false;
    }

    out->size = box_size;

    if (type_equals(out->type, "uuid")) {
        if (out->header_size + 16 > out->size) {
            snprintf(err, err_cap, "uuid box too small at offset=%" PRIu64, start);
            return false;
        }
        if (!read_exact(f, out->uuid, 16)) {
            snprintf(err, err_cap, "read uuid failed at offset=%" PRIu64, start);
            return false;
        }
        out->has_uuid = true;
        out->header_size += 16;
    }

    return true;
}

typedef struct {
    uint64_t offset;
    uint64_t length;
} Extent;

typedef struct {
    uint32_t item_id;
    bool has_type;
    char item_type[4];

    bool has_iloc;
    uint8_t iloc_version;
    uint8_t construction_method;
    uint16_t data_reference_index;
    uint64_t base_offset;

    Extent *extents;
    size_t extent_count;
    size_t extent_cap;
} Item;

typedef struct {
    Item *items;
    size_t item_count;
    size_t item_cap;

    bool has_primary;
    uint32_t primary_item_id;

    bool has_idat;
    uint64_t idat_payload_off;
    uint64_t idat_payload_size;
} MetaState;

static Item *get_or_add_item(MetaState *st, uint32_t item_id) {
    for (size_t i = 0; i < st->item_count; i++) {
        if (st->items[i].item_id == item_id) {
            return &st->items[i];
        }
    }

    if (st->item_count == st->item_cap) {
        size_t new_cap = st->item_cap ? st->item_cap * 2 : 32;
        Item *p = (Item *)realloc(st->items, new_cap * sizeof(Item));
        if (!p) {
            return NULL;
        }
        st->items = p;
        st->item_cap = new_cap;
    }

    Item *it = &st->items[st->item_count++];
    memset(it, 0, sizeof(*it));
    it->item_id = item_id;
    return it;
}

static bool item_add_extent(Item *it, Extent ex) {
    if (it->extent_count == it->extent_cap) {
        size_t new_cap = it->extent_cap ? it->extent_cap * 2 : 4;
        Extent *p = (Extent *)realloc(it->extents, new_cap * sizeof(Extent));
        if (!p) {
            return false;
        }
        it->extents = p;
        it->extent_cap = new_cap;
    }
    it->extents[it->extent_count++] = ex;
    return true;
}

static bool parse_pitm(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek pitm failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read pitm FullBox failed");
        return false;
    }
    (void)flags;

    uint32_t item_id = 0;
    if (version == 0) {
        uint16_t id16;
        if (!read_u16_be(f, &id16)) {
            snprintf(err, err_cap, "read pitm item_ID(v0) failed");
            return false;
        }
        item_id = id16;
    } else if (version == 1) {
        uint32_t id32;
        if (!read_u32_be(f, &id32)) {
            snprintf(err, err_cap, "read pitm item_ID(v1) failed");
            return false;
        }
        item_id = id32;
    } else {
        snprintf(err, err_cap, "unsupported pitm version=%u", (unsigned)version);
        return false;
    }

    uint64_t cur;
    if (!file_tell(f, &cur) || cur > payload_end) {
        snprintf(err, err_cap, "pitm overruns payload");
        return false;
    }

    st->has_primary = true;
    st->primary_item_id = item_id;
    return true;
}

static bool parse_infe(FILE *f, uint64_t infe_off, uint64_t infe_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, infe_off)) {
        snprintf(err, err_cap, "seek infe failed");
        return false;
    }

    BoxHdr hdr;
    if (!read_box_header(f, (uint64_t)-1, infe_end, &hdr, err, err_cap)) {
        return false;
    }
    if (!type_equals(hdr.type, "infe")) {
        snprintf(err, err_cap, "expected infe box");
        return false;
    }

    uint64_t payload_off = hdr.offset + hdr.header_size;

    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek infe payload failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read infe FullBox failed");
        return false;
    }
    (void)flags;

    uint32_t item_id = 0;
    if (version == 0 || version == 1 || version == 2) {
        uint16_t id16;
        if (!read_u16_be(f, &id16)) {
            snprintf(err, err_cap, "read infe item_ID(v%u) failed", (unsigned)version);
            return false;
        }
        item_id = id16;
    } else if (version == 3) {
        uint32_t id32;
        if (!read_u32_be(f, &id32)) {
            snprintf(err, err_cap, "read infe item_ID(v3) failed");
            return false;
        }
        item_id = id32;
    } else {
        // Skip unknown versions.
        return true;
    }

    uint16_t item_protection_index;
    if (!read_u16_be(f, &item_protection_index)) {
        snprintf(err, err_cap, "read infe item_protection_index failed");
        return false;
    }
    (void)item_protection_index;

    Item *it = get_or_add_item(st, item_id);
    if (!it) {
        snprintf(err, err_cap, "OOM adding item");
        return false;
    }

    if (version == 2 || version == 3) {
        char item_type[4];
        if (!read_exact(f, item_type, 4)) {
            snprintf(err, err_cap, "read infe item_type failed");
            return false;
        }
        it->has_type = true;
        memcpy(it->item_type, item_type, 4);
    }

    return true;
}

static bool parse_iinf(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek iinf failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read iinf FullBox failed");
        return false;
    }
    (void)flags;

    uint32_t entry_count = 0;
    if (version == 0) {
        uint16_t c16;
        if (!read_u16_be(f, &c16)) {
            snprintf(err, err_cap, "read iinf entry_count(v0) failed");
            return false;
        }
        entry_count = c16;
    } else if (version == 1) {
        uint32_t c32;
        if (!read_u32_be(f, &c32)) {
            snprintf(err, err_cap, "read iinf entry_count(v1) failed");
            return false;
        }
        entry_count = c32;
    } else {
        snprintf(err, err_cap, "unsupported iinf version=%u", (unsigned)version);
        return false;
    }

    uint64_t cursor;
    if (!file_tell(f, &cursor)) {
        snprintf(err, err_cap, "tell failed");
        return false;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        if (cursor >= payload_end) {
            snprintf(err, err_cap, "iinf: ran out of data before reading infe[%u/%u]", i, entry_count);
            return false;
        }
        if (!file_seek(f, cursor)) {
            snprintf(err, err_cap, "seek infe[%u] failed", i);
            return false;
        }

        BoxHdr hdr;
        if (!read_box_header(f, (uint64_t)-1, payload_end, &hdr, err, err_cap)) {
            return false;
        }

        if (!type_equals(hdr.type, "infe")) {
            snprintf(err, err_cap, "iinf: expected infe box, got '%.4s'", hdr.type);
            return false;
        }

        if (!parse_infe(f, hdr.offset, payload_end, st, err, err_cap)) {
            return false;
        }

        cursor = hdr.offset + hdr.size;
    }

    return true;
}

static bool parse_iloc(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek iloc failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read iloc FullBox failed");
        return false;
    }
    (void)flags;

    uint8_t a, b;
    if (!read_u8(f, &a) || !read_u8(f, &b)) {
        snprintf(err, err_cap, "read iloc size fields failed");
        return false;
    }

    uint8_t offset_size = (a >> 4) & 0x0F;
    uint8_t length_size = a & 0x0F;
    uint8_t base_offset_size = (b >> 4) & 0x0F;
    uint8_t index_size = b & 0x0F;

    uint32_t item_count = 0;
    if (version == 0 || version == 1) {
        uint16_t c16;
        if (!read_u16_be(f, &c16)) {
            snprintf(err, err_cap, "read iloc item_count(v0/v1) failed");
            return false;
        }
        item_count = c16;
    } else if (version == 2) {
        uint32_t c32;
        if (!read_u32_be(f, &c32)) {
            snprintf(err, err_cap, "read iloc item_count(v2) failed");
            return false;
        }
        item_count = c32;
    } else {
        snprintf(err, err_cap, "unsupported iloc version=%u", (unsigned)version);
        return false;
    }

    for (uint32_t i = 0; i < item_count; i++) {
        uint32_t item_id = 0;
        if (version == 2) {
            uint32_t id32;
            if (!read_u32_be(f, &id32)) {
                snprintf(err, err_cap, "read iloc item_ID(v2) failed");
                return false;
            }
            item_id = id32;
        } else {
            uint16_t id16;
            if (!read_u16_be(f, &id16)) {
                snprintf(err, err_cap, "read iloc item_ID(v0/v1) failed");
                return false;
            }
            item_id = id16;
        }

        uint8_t construction_method = 0;
        if (version == 1 || version == 2) {
            uint16_t tmp;
            if (!read_u16_be(f, &tmp)) {
                snprintf(err, err_cap, "read iloc construction_method failed");
                return false;
            }
            construction_method = (uint8_t)(tmp & 0x000F);
        }

        uint16_t data_reference_index;
        if (!read_u16_be(f, &data_reference_index)) {
            snprintf(err, err_cap, "read iloc data_reference_index failed");
            return false;
        }

        uint64_t base_offset = 0;
        if (!read_be_n(f, base_offset_size, &base_offset)) {
            snprintf(err, err_cap, "read iloc base_offset failed");
            return false;
        }

        uint16_t extent_count;
        if (!read_u16_be(f, &extent_count)) {
            snprintf(err, err_cap, "read iloc extent_count failed");
            return false;
        }

        Item *it = get_or_add_item(st, item_id);
        if (!it) {
            snprintf(err, err_cap, "OOM adding item (iloc)");
            return false;
        }
        it->has_iloc = true;
        it->iloc_version = version;
        it->construction_method = construction_method;
        it->data_reference_index = data_reference_index;
        it->base_offset = base_offset;

        for (uint16_t e = 0; e < extent_count; e++) {
            if ((version == 1 || version == 2) && index_size > 0) {
                uint64_t idx;
                if (!read_be_n(f, index_size, &idx)) {
                    snprintf(err, err_cap, "read iloc extent_index failed");
                    return false;
                }
                (void)idx;
            }

            uint64_t extent_offset = 0;
            uint64_t extent_length = 0;
            if (!read_be_n(f, offset_size, &extent_offset)) {
                snprintf(err, err_cap, "read iloc extent_offset failed");
                return false;
            }
            if (!read_be_n(f, length_size, &extent_length)) {
                snprintf(err, err_cap, "read iloc extent_length failed");
                return false;
            }

            Extent ex;
            ex.offset = base_offset + extent_offset;
            ex.length = extent_length;

            if (!item_add_extent(it, ex)) {
                snprintf(err, err_cap, "OOM adding extent");
                return false;
            }
        }

        uint64_t cur;
        if (!file_tell(f, &cur) || cur > payload_end) {
            snprintf(err, err_cap, "iloc overruns payload");
            return false;
        }
    }

    return true;
}

static bool parse_meta(FILE *f, uint64_t file_size, uint64_t meta_off, uint64_t meta_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, meta_off)) {
        snprintf(err, err_cap, "seek meta failed");
        return false;
    }

    BoxHdr hdr;
    if (!read_box_header(f, file_size, meta_end, &hdr, err, err_cap)) {
        return false;
    }
    if (!type_equals(hdr.type, "meta")) {
        snprintf(err, err_cap, "expected meta box");
        return false;
    }

    uint64_t payload_off = hdr.offset + hdr.header_size;
    uint64_t payload_end = hdr.offset + hdr.size;

    if (payload_off + 4 > payload_end) {
        snprintf(err, err_cap, "meta too small for FullBox fields");
        return false;
    }

    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek meta payload failed");
        return false;
    }
    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read meta FullBox failed");
        return false;
    }
    (void)version;
    (void)flags;

    uint64_t cursor;
    if (!file_tell(f, &cursor)) {
        snprintf(err, err_cap, "tell meta cursor failed");
        return false;
    }

    while (cursor < payload_end) {
        if (!file_seek(f, cursor)) {
            snprintf(err, err_cap, "seek meta child failed");
            return false;
        }

        BoxHdr ch;
        if (!read_box_header(f, file_size, payload_end, &ch, err, err_cap)) {
            return false;
        }

        uint64_t ch_payload_off = ch.offset + ch.header_size;
        uint64_t ch_payload_end = ch.offset + ch.size;

        if (type_equals(ch.type, "pitm")) {
            if (!parse_pitm(f, ch_payload_off, ch_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(ch.type, "iinf")) {
            if (!parse_iinf(f, ch_payload_off, ch_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(ch.type, "iloc")) {
            if (!parse_iloc(f, ch_payload_off, ch_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(ch.type, "idat")) {
            st->has_idat = true;
            st->idat_payload_off = ch_payload_off;
            st->idat_payload_size = ch_payload_end - ch_payload_off;
        }

        cursor = ch.offset + ch.size;
    }

    return true;
}

static bool find_top_level_box(FILE *f, uint64_t file_size, const char *box_type4, uint64_t *out_off, uint64_t *out_end, char *err, size_t err_cap) {
    uint64_t cursor = 0;
    while (cursor < file_size) {
        if (!file_seek(f, cursor)) {
            snprintf(err, err_cap, "seek failed at offset=%" PRIu64 ": %s", cursor, strerror(errno));
            return false;
        }

        BoxHdr hdr;
        if (!read_box_header(f, file_size, file_size, &hdr, err, err_cap)) {
            return false;
        }

        if (type_equals(hdr.type, box_type4)) {
            *out_off = hdr.offset;
            *out_end = hdr.offset + hdr.size;
            return true;
        }

        cursor = hdr.offset + hdr.size;
    }

    snprintf(err, err_cap, "no top-level '%.4s' box found", box_type4);
    return false;
}

static const Item *find_item(const MetaState *st, uint32_t item_id) {
    for (size_t i = 0; i < st->item_count; i++) {
        if (st->items[i].item_id == item_id) {
            return &st->items[i];
        }
    }
    return NULL;
}

static bool extract_primary_to_file(FILE *f, uint64_t file_size, const MetaState *st, FILE *out, char *err, size_t err_cap, uint64_t *out_size) {
    if (!st->has_primary) {
        snprintf(err, err_cap, "no primary item");
        return false;
    }

    const Item *primary = find_item(st, st->primary_item_id);
    if (!primary) {
        snprintf(err, err_cap, "primary item not found in item table");
        return false;
    }
    if (!primary->has_type || !type_equals(primary->item_type, "av01")) {
        snprintf(err, err_cap, "primary item is not a coded 'av01' item");
        return false;
    }
    if (!primary->has_iloc || primary->extent_count == 0) {
        snprintf(err, err_cap, "primary item has no iloc extents");
        return false;
    }
    if (primary->data_reference_index != 0) {
        snprintf(err, err_cap, "primary item uses external data_reference_index=%u (unsupported)", (unsigned)primary->data_reference_index);
        return false;
    }
    if (primary->construction_method == 2) {
        snprintf(err, err_cap, "iloc construction_method=2 (item-based construction) unsupported");
        return false;
    }

    uint64_t total = 0;
    uint8_t buf[64 * 1024];

    for (size_t i = 0; i < primary->extent_count; i++) {
        const Extent *ex = &primary->extents[i];
        if (ex->length == 0) {
            snprintf(err, err_cap, "extent length=0 (implicit/unknown length unsupported)");
            return false;
        }

        uint64_t src_off = 0;
        if (primary->construction_method == 0) {
            src_off = ex->offset;
        } else if (primary->construction_method == 1) {
            if (!st->has_idat) {
                snprintf(err, err_cap, "construction_method=1 but no idat box found");
                return false;
            }
            if (ex->offset + ex->length > st->idat_payload_size) {
                snprintf(err, err_cap, "idat extent overruns idat payload");
                return false;
            }
            src_off = st->idat_payload_off + ex->offset;
        }

        if (src_off + ex->length > file_size) {
            snprintf(err, err_cap, "extent overruns file");
            return false;
        }

        if (!file_seek(f, src_off)) {
            snprintf(err, err_cap, "seek extent failed");
            return false;
        }

        uint64_t remaining = ex->length;
        while (remaining > 0) {
            size_t chunk = (size_t)(remaining > sizeof(buf) ? sizeof(buf) : remaining);
            if (!read_exact(f, buf, chunk)) {
                snprintf(err, err_cap, "read extent bytes failed");
                return false;
            }
            if (fwrite(buf, 1, chunk, out) != chunk) {
                snprintf(err, err_cap, "write failed");
                return false;
            }
            remaining -= (uint64_t)chunk;
            total += (uint64_t)chunk;
        }
    }

    *out_size = total;
    return true;
}

// --- Minimal AV1 OBU scan ---

// LEB128 for OBU size.
static bool read_leb128_u64(const uint8_t *data, size_t data_len, size_t *io_off, uint64_t *out) {
    uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10; i++) {
        if (*io_off >= data_len) {
            return false;
        }
        uint8_t byte = data[(*io_off)++];
        value |= (uint64_t)(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

typedef struct {
    uint32_t obu_count;
    uint32_t seq_hdr_count;
} ObuStats;

static bool scan_obus(const uint8_t *data, size_t data_len, ObuStats *stats, char *err, size_t err_cap) {
    memset(stats, 0, sizeof(*stats));

    size_t off = 0;
    while (off < data_len) {
        // Allow trailing zero padding.
        if (data[off] == 0) {
            size_t z = off;
            while (z < data_len && data[z] == 0) {
                z++;
            }
            if (z == data_len) {
                return true;
            }
        }

        uint8_t header = data[off++];
        uint8_t forbidden = (header >> 7) & 1u;
        uint8_t obu_type = (header >> 3) & 0x0Fu;
        uint8_t extension_flag = (header >> 2) & 1u;
        uint8_t has_size_field = (header >> 1) & 1u;

        if (forbidden != 0) {
            snprintf(err, err_cap, "OBU forbidden bit set");
            return false;
        }
        if (!has_size_field) {
            snprintf(err, err_cap, "OBU has_size_field=0 (unsupported framing)");
            return false;
        }

        if (extension_flag) {
            if (off >= data_len) {
                snprintf(err, err_cap, "truncated OBU extension header");
                return false;
            }
            off++;
        }

        uint64_t obu_size = 0;
        if (!read_leb128_u64(data, data_len, &off, &obu_size)) {
            snprintf(err, err_cap, "failed to read OBU size LEB128");
            return false;
        }

        if (obu_size > (uint64_t)(data_len - off)) {
            snprintf(err, err_cap, "OBU payload overruns buffer");
            return false;
        }

        stats->obu_count++;
        if (obu_type == 1) {
            stats->seq_hdr_count++;
        }

        off += (size_t)obu_size;
    }

    return true;
}

int main(int argc, char **argv) {
    const char *in_path = NULL;
    const char *out_path = NULL;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(stdout);
        return 0;
    }

    if (argc != 3) {
        print_usage(stderr);
        return 2;
    }

    in_path = argv[1];
    out_path = argv[2];

    FILE *f = fopen(in_path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open %s: %s\n", in_path, strerror(errno));
        return 1;
    }

    uint64_t file_size;
    if (!get_file_size(f, &file_size)) {
        fprintf(stderr, "failed to get file size: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }

    uint64_t meta_off = 0, meta_end = 0;
    char err[256];
    if (!find_top_level_box(f, file_size, "meta", &meta_off, &meta_end, err, sizeof(err))) {
        fprintf(stderr, "%s (unsupported)\n", err);
        fclose(f);
        return 1;
    }

    MetaState st;
    memset(&st, 0, sizeof(st));
    if (!parse_meta(f, file_size, meta_off, meta_end, &st, err, sizeof(err))) {
        fprintf(stderr, "m2 parse failed: %s\n", err);
        fclose(f);
        return 1;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "failed to open %s: %s\n", out_path, strerror(errno));
        fclose(f);
        return 1;
    }

    uint64_t out_size = 0;
    if (!extract_primary_to_file(f, file_size, &st, out, err, sizeof(err), &out_size)) {
        fprintf(stderr, "extract failed: %s\n", err);
        fclose(out);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    fclose(out);

    // Read back for OBU scan (simple and deterministic; avoids buffering during extraction).
    FILE *obuf = fopen(out_path, "rb");
    if (!obuf) {
        fprintf(stderr, "failed to reopen %s for validation: %s\n", out_path, strerror(errno));
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    if (out_size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "extracted AV1 too large to validate in-memory (%" PRIu64 " bytes)\n", out_size);
        fclose(obuf);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    uint8_t *bytes = (uint8_t *)malloc((size_t)out_size);
    if (!bytes) {
        fprintf(stderr, "OOM reading extracted AV1 (%" PRIu64 " bytes)\n", out_size);
        fclose(obuf);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    if (!read_exact(obuf, bytes, (size_t)out_size)) {
        fprintf(stderr, "failed to read extracted AV1 bytes\n");
        free(bytes);
        fclose(obuf);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }
    fclose(obuf);

    ObuStats stats;
    if (!scan_obus(bytes, (size_t)out_size, &stats, err, sizeof(err))) {
        fprintf(stderr, "OBU scan failed: %s\n", err);
        free(bytes);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    // For the simple still-image path in m2, expect exactly one Sequence Header OBU.
    if (stats.seq_hdr_count != 1) {
        fprintf(stderr, "OBU validation failed: expected exactly 1 Sequence Header OBU, got %u\n", stats.seq_hdr_count);
        free(bytes);
        fclose(f);
        for (size_t i = 0; i < st.item_count; i++) {
            free(st.items[i].extents);
        }
        free(st.items);
        return 1;
    }

    fprintf(stderr, "OK: extracted %" PRIu64 " bytes; OBUs=%u; seq_hdr=%u\n", out_size, stats.obu_count, stats.seq_hdr_count);

    free(bytes);
    fclose(f);
    for (size_t i = 0; i < st.item_count; i++) {
        free(st.items[i].extents);
    }
    free(st.items);

    return 0;
}
