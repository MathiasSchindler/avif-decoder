#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// m1 goal: parse enough HEIF item metadata inside `meta` to locate the primary item and its payload extents.
// This is not a full HEIF/MIAF implementation; it must be robust (bounds-checked) and fail clearly.

static void print_usage(FILE *out) {
    fprintf(out,
            "Usage: avif_metadump [--extract-primary OUT] <file.avif>\n"
            "\n"
            "Parses AVIF/HEIF item metadata in the `meta` box (m1).\n"
            "Prints a summary of: hdlr, pitm, iinf/infe, iloc, iprp/ipco/ipma.\n"
            "\n"
            "Options:\n"
            "  --extract-primary OUT   Write concatenated primary item payload to OUT (best-effort;\n"
            "                         supports iloc construction_method 0 (file offsets) and 1 (idat)).\n");
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

static bool read_be_n(FILE *f, uint64_t nbytes, uint64_t *out) {
    if (nbytes > 8) {
        return false;
    }
    uint8_t buf[8] = {0};
    if (nbytes == 0) {
        *out = 0;
        return true;
    }
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

static bool read_fullbox_header(FILE *f, uint8_t *out_version, uint32_t *out_flags) {
    uint8_t vf[4];
    if (!read_exact(f, vf, 4)) {
        return false;
    }
    *out_version = vf[0];
    *out_flags = ((uint32_t)vf[1] << 16) | ((uint32_t)vf[2] << 8) | (uint32_t)vf[3];
    return true;
}

static void print_type(const char type[4]) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)type[i];
        if (c >= 32 && c <= 126) {
            fputc(c, stdout);
        } else {
            fputc('.', stdout);
        }
    }
}

static bool type_equals(const char t[4], const char *lit4) {
    return memcmp(t, lit4, 4) == 0;
}

typedef struct {
    uint64_t offset;
    uint64_t size;
    char type[4];
    bool has_uuid;
    uint8_t uuid[16];
    uint64_t header_size;
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
    bool has_index;
    uint64_t index;
} Extent;

typedef struct {
    uint32_t prop_index;
    bool essential;
} PropAssoc;

typedef struct {
    uint32_t item_id;
    bool has_type;
    char item_type[4];

    bool has_iloc;
    uint8_t iloc_version;
    uint8_t construction_method; // 0=file, 1=idat, 2=other
    uint16_t data_reference_index;
    uint64_t base_offset;

    Extent *extents;
    size_t extent_count;
    size_t extent_cap;

    PropAssoc *props;
    size_t prop_count;
    size_t prop_cap;
} Item;

typedef struct {
    char type[4];
    uint64_t box_offset;
    uint64_t box_size;

    // Parsed summary for a few known properties.
    bool has_ispe;
    uint32_t ispe_width;
    uint32_t ispe_height;

    bool has_pixi;
    uint8_t pixi_channels;
    uint8_t pixi_depth[16];

    bool has_av1c;
    uint8_t av1c_version;
    uint8_t av1c_seq_profile;
    uint8_t av1c_seq_level_idx_0;
    uint8_t av1c_seq_tier_0;
    bool av1c_high_bitdepth;
    bool av1c_twelve_bit;
    bool av1c_monochrome;
    bool av1c_subsampling_x;
    bool av1c_subsampling_y;
    uint8_t av1c_chroma_sample_position;
    bool av1c_initial_presentation_delay_present;
    uint8_t av1c_initial_presentation_delay_minus_one;
} Property;

typedef struct {
    Item *items;
    size_t item_count;
    size_t item_cap;

    Property *props; // ipco properties, 1-based indexing in ipma
    size_t prop_count;
    size_t prop_cap;

    bool has_primary;
    uint32_t primary_item_id;

    bool has_hdlr;
    char hdlr_handler_type[4];

    bool has_idat;
    uint64_t idat_payload_off;
    uint64_t idat_payload_size;

    unsigned warning_count;
} MetaState;

static void warnf(MetaState *st, const char *msg) {
    st->warning_count++;
    fprintf(stderr, "WARNING: %s\n", msg);
}

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

static bool item_add_prop(Item *it, PropAssoc assoc) {
    if (it->prop_count == it->prop_cap) {
        size_t new_cap = it->prop_cap ? it->prop_cap * 2 : 4;
        PropAssoc *p = (PropAssoc *)realloc(it->props, new_cap * sizeof(PropAssoc));
        if (!p) {
            return false;
        }
        it->props = p;
        it->prop_cap = new_cap;
    }
    it->props[it->prop_count++] = assoc;
    return true;
}

static Property *add_property(MetaState *st, const char type[4], uint64_t box_offset, uint64_t box_size) {
    if (st->prop_count == st->prop_cap) {
        size_t new_cap = st->prop_cap ? st->prop_cap * 2 : 32;
        Property *p = (Property *)realloc(st->props, new_cap * sizeof(Property));
        if (!p) {
            return NULL;
        }
        st->props = p;
        st->prop_cap = new_cap;
    }

    Property *pr = &st->props[st->prop_count++];
    memset(pr, 0, sizeof(*pr));
    memcpy(pr->type, type, 4);
    pr->box_offset = box_offset;
    pr->box_size = box_size;
    return pr;
}

static bool parse_hdlr(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek hdlr failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read hdlr FullBox failed");
        return false;
    }

    (void)version;
    (void)flags;

    uint32_t pre_defined;
    if (!read_u32_be(f, &pre_defined)) {
        snprintf(err, err_cap, "read hdlr pre_defined failed");
        return false;
    }
    (void)pre_defined;

    uint8_t handler_type[4];
    if (!read_exact(f, handler_type, 4)) {
        snprintf(err, err_cap, "read hdlr handler_type failed");
        return false;
    }

    memcpy(st->hdlr_handler_type, handler_type, 4);
    st->has_hdlr = true;

    // Skip the rest (reserved + name).
    uint64_t cur;
    if (!file_tell(f, &cur)) {
        snprintf(err, err_cap, "tell failed");
        return false;
    }
    if (cur > payload_end) {
        snprintf(err, err_cap, "hdlr overruns payload");
        return false;
    }

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

    // infe is a box; read its header first.
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
        // Unknown infe version: skip.
        char w[128];
        snprintf(w, sizeof(w), "unsupported infe version=%u (skipping item metadata)", (unsigned)version);
        warnf(st, w);
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
    } else {
        // infe v0/v1 do not carry item_type; keep it unknown.
    }

    // Remaining fields (item_name, content_type, etc.) are variable strings; we do not parse them in m1.
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

    // infe boxes follow.
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

        // Parse infe by reading from its offset again.
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
    uint8_t index_size = b & 0x0F; // only meaningful for v1/v2

    if (!(offset_size == 0 || offset_size == 4 || offset_size == 8 || offset_size == 1 || offset_size == 2 || offset_size == 3 || offset_size == 5 || offset_size == 6 || offset_size == 7)) {
        // Just a sanity guard; we still allow any 0..8 below.
    }

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
            Extent ex;
            memset(&ex, 0, sizeof(ex));

            if ((version == 1 || version == 2) && index_size > 0) {
                uint64_t idx;
                if (!read_be_n(f, index_size, &idx)) {
                    snprintf(err, err_cap, "read iloc extent_index failed");
                    return false;
                }
                ex.has_index = true;
                ex.index = idx;
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

static bool parse_ispe(FILE *f, uint64_t payload_off, uint64_t payload_end, Property *pr, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek ispe failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read ispe FullBox failed");
        return false;
    }
    (void)flags;

    if (version != 0) {
        // In m1, treat unknown versions as "details unavailable" (but keep property record).
        (void)payload_end;
        return true;
    }

    uint32_t w, h;
    if (!read_u32_be(f, &w) || !read_u32_be(f, &h)) {
        snprintf(err, err_cap, "read ispe width/height failed");
        return false;
    }

    uint64_t cur;
    if (!file_tell(f, &cur) || cur > payload_end) {
        snprintf(err, err_cap, "ispe overruns payload");
        return false;
    }

    pr->has_ispe = true;
    pr->ispe_width = w;
    pr->ispe_height = h;
    return true;
}

static bool parse_pixi(FILE *f, uint64_t payload_off, uint64_t payload_end, Property *pr, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek pixi failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read pixi FullBox failed");
        return false;
    }
    (void)flags;

    if (version != 0) {
        (void)payload_end;
        return true;
    }

    uint8_t num_channels;
    if (!read_u8(f, &num_channels)) {
        snprintf(err, err_cap, "read pixi num_channels failed");
        return false;
    }

    pr->has_pixi = true;
    pr->pixi_channels = num_channels;
    for (uint8_t i = 0; i < num_channels; i++) {
        uint8_t depth;
        if (!read_u8(f, &depth)) {
            snprintf(err, err_cap, "read pixi bit_depth failed");
            return false;
        }
        if (i < (uint8_t)sizeof(pr->pixi_depth)) {
            pr->pixi_depth[i] = depth;
        }
    }

    uint64_t cur;
    if (!file_tell(f, &cur) || cur > payload_end) {
        snprintf(err, err_cap, "pixi overruns payload");
        return false;
    }

    return true;
}

static bool parse_av1c(FILE *f, uint64_t payload_off, uint64_t payload_end, Property *pr, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek av1C failed");
        return false;
    }

    // AV1CodecConfigurationRecord is 4 bytes.
    uint8_t b0, b1, b2, b3;
    if (!read_u8(f, &b0) || !read_u8(f, &b1) || !read_u8(f, &b2) || !read_u8(f, &b3)) {
        snprintf(err, err_cap, "read av1C bytes failed");
        return false;
    }

    pr->has_av1c = true;
    pr->av1c_version = (uint8_t)(b0 & 0x7F);
    pr->av1c_seq_profile = (uint8_t)((b1 >> 5) & 0x07);
    pr->av1c_seq_level_idx_0 = (uint8_t)(b1 & 0x1F);
    pr->av1c_seq_tier_0 = (uint8_t)((b2 >> 7) & 0x01);
    pr->av1c_high_bitdepth = ((b2 >> 6) & 0x01) != 0;
    pr->av1c_twelve_bit = ((b2 >> 5) & 0x01) != 0;
    pr->av1c_monochrome = ((b2 >> 4) & 0x01) != 0;
    pr->av1c_subsampling_x = ((b2 >> 3) & 0x01) != 0;
    pr->av1c_subsampling_y = ((b2 >> 2) & 0x01) != 0;
    pr->av1c_chroma_sample_position = (uint8_t)(b2 & 0x03);

    pr->av1c_initial_presentation_delay_present = ((b3 >> 4) & 0x01) != 0;
    if (pr->av1c_initial_presentation_delay_present) {
        pr->av1c_initial_presentation_delay_minus_one = (uint8_t)(b3 & 0x0F);
    }

    uint64_t cur;
    if (!file_tell(f, &cur) || cur > payload_end) {
        snprintf(err, err_cap, "av1C overruns payload");
        return false;
    }

    return true;
}

static bool parse_ipco(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    uint64_t cursor = payload_off;
    while (cursor < payload_end) {
        if (!file_seek(f, cursor)) {
            snprintf(err, err_cap, "seek ipco child failed");
            return false;
        }

        BoxHdr hdr;
        if (!read_box_header(f, (uint64_t)-1, payload_end, &hdr, err, err_cap)) {
            return false;
        }

        uint64_t child_payload_off = hdr.offset + hdr.header_size;
        uint64_t child_payload_end = hdr.offset + hdr.size;

        Property *pr = add_property(st, hdr.type, hdr.offset, hdr.size);
        if (!pr) {
            snprintf(err, err_cap, "OOM adding property");
            return false;
        }

        if (type_equals(hdr.type, "ispe")) {
            if (!parse_ispe(f, child_payload_off, child_payload_end, pr, err, err_cap)) {
                return false;
            }
        } else if (type_equals(hdr.type, "pixi")) {
            if (!parse_pixi(f, child_payload_off, child_payload_end, pr, err, err_cap)) {
                return false;
            }
        } else if (type_equals(hdr.type, "av1C")) {
            if (!parse_av1c(f, child_payload_off, child_payload_end, pr, err, err_cap)) {
                return false;
            }
        } else {
            // Unknown properties are allowed; we record type and offsets only.
        }

        cursor = hdr.offset + hdr.size;
    }

    return true;
}

static bool parse_ipma(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    if (!file_seek(f, payload_off)) {
        snprintf(err, err_cap, "seek ipma failed");
        return false;
    }

    uint8_t version;
    uint32_t flags;
    if (!read_fullbox_header(f, &version, &flags)) {
        snprintf(err, err_cap, "read ipma FullBox failed");
        return false;
    }

    bool assoc_16bit = (flags & 1u) != 0;

    uint32_t entry_count;
    if (!read_u32_be(f, &entry_count)) {
        snprintf(err, err_cap, "read ipma entry_count failed");
        return false;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t item_id = 0;
        if (version == 0) {
            uint16_t id16;
            if (!read_u16_be(f, &id16)) {
                snprintf(err, err_cap, "read ipma item_ID(v0) failed");
                return false;
            }
            item_id = id16;
        } else if (version == 1) {
            uint32_t id32;
            if (!read_u32_be(f, &id32)) {
                snprintf(err, err_cap, "read ipma item_ID(v1) failed");
                return false;
            }
            item_id = id32;
        } else {
            snprintf(err, err_cap, "unsupported ipma version=%u", (unsigned)version);
            return false;
        }

        uint8_t assoc_count;
        if (!read_u8(f, &assoc_count)) {
            snprintf(err, err_cap, "read ipma association_count failed");
            return false;
        }

        Item *it = get_or_add_item(st, item_id);
        if (!it) {
            snprintf(err, err_cap, "OOM adding item (ipma)");
            return false;
        }

        for (uint8_t a = 0; a < assoc_count; a++) {
            PropAssoc assoc;
            memset(&assoc, 0, sizeof(assoc));

            if (assoc_16bit) {
                uint16_t v;
                if (!read_u16_be(f, &v)) {
                    snprintf(err, err_cap, "read ipma assoc16 failed");
                    return false;
                }
                assoc.essential = (v & 0x8000u) != 0;
                assoc.prop_index = (uint32_t)(v & 0x7FFFu);
            } else {
                uint8_t v;
                if (!read_u8(f, &v)) {
                    snprintf(err, err_cap, "read ipma assoc8 failed");
                    return false;
                }
                assoc.essential = (v & 0x80u) != 0;
                assoc.prop_index = (uint32_t)(v & 0x7Fu);
            }

            if (!item_add_prop(it, assoc)) {
                snprintf(err, err_cap, "OOM adding item prop assoc");
                return false;
            }
        }

        uint64_t cur;
        if (!file_tell(f, &cur) || cur > payload_end) {
            snprintf(err, err_cap, "ipma overruns payload");
            return false;
        }
    }

    return true;
}

static bool parse_iprp(FILE *f, uint64_t payload_off, uint64_t payload_end, MetaState *st, char *err, size_t err_cap) {
    uint64_t cursor = payload_off;
    while (cursor < payload_end) {
        if (!file_seek(f, cursor)) {
            snprintf(err, err_cap, "seek iprp child failed");
            return false;
        }

        BoxHdr hdr;
        if (!read_box_header(f, (uint64_t)-1, payload_end, &hdr, err, err_cap)) {
            return false;
        }

        uint64_t child_payload_off = hdr.offset + hdr.header_size;
        uint64_t child_payload_end = hdr.offset + hdr.size;

        if (type_equals(hdr.type, "ipco")) {
            if (!parse_ipco(f, child_payload_off, child_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(hdr.type, "ipma")) {
            if (!parse_ipma(f, child_payload_off, child_payload_end, st, err, err_cap)) {
                return false;
            }
        } else {
            // ignore
        }

        cursor = hdr.offset + hdr.size;
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

    // meta is FullBox
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

        if (type_equals(ch.type, "hdlr")) {
            if (!parse_hdlr(f, ch_payload_off, ch_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(ch.type, "pitm")) {
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
        } else if (type_equals(ch.type, "iprp")) {
            if (!parse_iprp(f, ch_payload_off, ch_payload_end, st, err, err_cap)) {
                return false;
            }
        } else if (type_equals(ch.type, "idat")) {
            // idat is a simple box, payload is raw bytes.
            st->has_idat = true;
            st->idat_payload_off = ch_payload_off;
            st->idat_payload_size = ch_payload_end - ch_payload_off;
        } else {
            // ignore
        }

        cursor = ch.offset + ch.size;
    }

    return true;
}

static const Property *get_property_by_index(const MetaState *st, uint32_t index) {
    if (index == 0) {
        return NULL;
    }
    // st->props is 0-based; ipma is 1-based.
    if (index > st->prop_count) {
        return NULL;
    }
    return &st->props[index - 1];
}

static void dump_summary(const char *path, const MetaState *st) {
    printf("File: %s\n", path);

    if (st->has_hdlr) {
        printf("meta.hdlr.handler_type: '");
        print_type(st->hdlr_handler_type);
        printf("'\n");
    } else {
        printf("meta.hdlr: (missing)\n");
    }

    if (st->has_primary) {
        printf("meta.pitm.primary_item_id: %" PRIu32 "\n", st->primary_item_id);
    } else {
        printf("meta.pitm: (missing primary item)\n");
    }

    if (st->has_idat) {
        printf("meta.idat: payload @ [%" PRIu64 "+%" PRIu64 "]\n", st->idat_payload_off, st->idat_payload_size);
    }

    printf("items: %zu\n", st->item_count);
    for (size_t i = 0; i < st->item_count; i++) {
        const Item *it = &st->items[i];
        printf("  item_id=%" PRIu32 " type=", it->item_id);
        if (it->has_type) {
            printf("'");
            print_type(it->item_type);
            printf("'");
        } else {
            printf("(unknown)");
        }

        if (it->has_iloc) {
            printf(" iloc(v%u cm=%u dref=%u base=%" PRIu64 ") extents=%zu",
                   (unsigned)it->iloc_version,
                   (unsigned)it->construction_method,
                   (unsigned)it->data_reference_index,
                   it->base_offset,
                   it->extent_count);
        } else {
            printf(" iloc=(none)");
        }

        if (it->prop_count > 0) {
            printf(" props=%zu", it->prop_count);
        }

        printf("\n");

        // Print a few extents for visibility.
        const size_t max_extents = 4;
        size_t shown = it->extent_count < max_extents ? it->extent_count : max_extents;
        for (size_t e = 0; e < shown; e++) {
            const Extent *ex = &it->extents[e];
            printf("    extent[%zu]: off=%" PRIu64 " len=%" PRIu64, e, ex->offset, ex->length);
            if (ex->has_index) {
                printf(" idx=%" PRIu64, ex->index);
            }
            printf("\n");
        }
        if (it->extent_count > shown) {
            printf("    ... (%zu more extents)\n", it->extent_count - shown);
        }

        // Print properties for primary item (and for any item with essential properties).
        if (st->has_primary && it->item_id == st->primary_item_id) {
            printf("  primary item properties:\n");
            for (size_t p = 0; p < it->prop_count; p++) {
                const PropAssoc *pa = &it->props[p];
                const Property *pr = get_property_by_index(st, pa->prop_index);
                printf("    - ");
                if (pr) {
                    printf("prop_index=%" PRIu32 " type='", pa->prop_index);
                    print_type(pr->type);
                    printf("'");
                } else {
                    printf("prop_index=%" PRIu32 " type=(unknown)", pa->prop_index);
                }
                printf(" essential=%s", pa->essential ? "true" : "false");

                if (pr && pr->has_ispe) {
                    printf(" ispe=%ux%u", pr->ispe_width, pr->ispe_height);
                }
                if (pr && pr->has_pixi) {
                    printf(" pixi_channels=%u", (unsigned)pr->pixi_channels);
                    printf(" pixi_depths=");
                    uint8_t n = pr->pixi_channels;
                    if (n > (uint8_t)sizeof(pr->pixi_depth)) {
                        n = (uint8_t)sizeof(pr->pixi_depth);
                    }
                    for (uint8_t i = 0; i < n; i++) {
                        if (i) {
                            printf(",");
                        }
                        printf("%u", (unsigned)pr->pixi_depth[i]);
                    }
                    if (pr->pixi_channels > (uint8_t)sizeof(pr->pixi_depth)) {
                        printf(",...");
                    }
                }
                if (pr && pr->has_av1c) {
                    printf(" av1C(profile=%u level=%u tier=%u hb=%u tb=%u mono=%u subsamp=%u%u)",
                           (unsigned)pr->av1c_seq_profile,
                           (unsigned)pr->av1c_seq_level_idx_0,
                           (unsigned)pr->av1c_seq_tier_0,
                           pr->av1c_high_bitdepth ? 1u : 0u,
                           pr->av1c_twelve_bit ? 1u : 0u,
                           pr->av1c_monochrome ? 1u : 0u,
                           pr->av1c_subsampling_x ? 1u : 0u,
                           pr->av1c_subsampling_y ? 1u : 0u);
                }

                printf("\n");
            }
        }
    }

    if (st->has_primary) {
        const Item *primary = NULL;
        for (size_t i = 0; i < st->item_count; i++) {
            if (st->items[i].item_id == st->primary_item_id) {
                primary = &st->items[i];
                break;
            }
        }

        if (!primary) {
            printf("primary item: not found in item table (unexpected)\n");
        } else if (!primary->has_type) {
            printf("primary item: type unknown (missing infe?)\n");
        } else if (!type_equals(primary->item_type, "av01")) {
            printf("primary item: not a coded 'av01' item (type='");
            print_type(primary->item_type);
            printf("') => likely derived/sequence/aux; unsupported in m1 for extraction\n");
        } else {
            printf("primary item: coded 'av01' (simple still path candidate)\n");
        }
    }
}

static bool extract_primary(FILE *f, uint64_t file_size, const MetaState *st, const char *out_path, char *err, size_t err_cap) {
    if (!st->has_primary) {
        snprintf(err, err_cap, "no primary item");
        return false;
    }

    const Item *primary = NULL;
    for (size_t i = 0; i < st->item_count; i++) {
        if (st->items[i].item_id == st->primary_item_id) {
            primary = &st->items[i];
            break;
        }
    }

    if (!primary) {
        snprintf(err, err_cap, "primary item not found");
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

    if (!(primary->construction_method == 0 || primary->construction_method == 1)) {
        snprintf(err,
                 err_cap,
                 "iloc construction_method=%u unsupported",
                 (unsigned)primary->construction_method);
        return false;
    }

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        snprintf(err, err_cap, "failed to open %s: %s", out_path, strerror(errno));
        return false;
    }

    bool ok = true;
    uint8_t buf[64 * 1024];

    for (size_t i = 0; i < primary->extent_count; i++) {
        const Extent *ex = &primary->extents[i];

        if (ex->has_index) {
            snprintf(err, err_cap, "extent_index present (unsupported in m1)");
            ok = false;
            break;
        }

        if (ex->length == 0) {
            // Length size may be 0 in some files, implying length is known elsewhere.
            // We don't support that in m1.
            snprintf(err, err_cap, "extent length=0 (unsupported in m1)");
            ok = false;
            break;
        }

        uint64_t src_off = 0;
        if (primary->construction_method == 0) {
            src_off = ex->offset;
        } else if (primary->construction_method == 1) {
            if (!st->has_idat) {
                snprintf(err, err_cap, "construction_method=1 but no idat box found");
                ok = false;
                break;
            }
            // For idat-based construction, offsets are relative to the idat payload.
            if (ex->offset > st->idat_payload_size) {
                snprintf(err, err_cap, "idat extent offset out of range");
                ok = false;
                break;
            }
            if (ex->offset + ex->length > st->idat_payload_size) {
                snprintf(err, err_cap, "idat extent overruns idat payload");
                ok = false;
                break;
            }
            src_off = st->idat_payload_off + ex->offset;
        }

        if (src_off + ex->length > file_size) {
            snprintf(err, err_cap, "extent overruns file");
            ok = false;
            break;
        }

        if (!file_seek(f, src_off)) {
            snprintf(err, err_cap, "seek extent failed");
            ok = false;
            break;
        }

        uint64_t remaining = ex->length;
        while (remaining > 0) {
            size_t chunk = (size_t)(remaining > sizeof(buf) ? sizeof(buf) : remaining);
            if (!read_exact(f, buf, chunk)) {
                snprintf(err, err_cap, "read extent bytes failed");
                ok = false;
                break;
            }
            if (fwrite(buf, 1, chunk, out) != chunk) {
                snprintf(err, err_cap, "write to %s failed", out_path);
                ok = false;
                break;
            }
            remaining -= (uint64_t)chunk;
        }
        if (!ok) {
            break;
        }
    }

    fclose(out);
    return ok;
}

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *extract_out = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--extract-primary") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--extract-primary requires an output path\n");
                return 2;
            }
            extract_out = argv[++i];
            continue;
        }
        if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (!path) {
        print_usage(stderr);
        return 2;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    uint64_t file_size;
    if (!get_file_size(f, &file_size)) {
        fprintf(stderr, "failed to get file size: %s\n", strerror(errno));
        fclose(f);
        return 1;
    }

    // Find top-level meta box.
    bool found_meta = false;
    uint64_t cursor = 0;
    uint64_t meta_off = 0;
    uint64_t meta_end = 0;

    while (cursor < file_size) {
        if (!file_seek(f, cursor)) {
            fprintf(stderr, "seek failed at offset=%" PRIu64 ": %s\n", cursor, strerror(errno));
            fclose(f);
            return 1;
        }

        BoxHdr hdr;
        char err[256];
        if (!read_box_header(f, file_size, file_size, &hdr, err, sizeof(err))) {
            fprintf(stderr, "ERROR: %s\n", err);
            fclose(f);
            return 1;
        }

        if (type_equals(hdr.type, "meta")) {
            found_meta = true;
            meta_off = hdr.offset;
            meta_end = hdr.offset + hdr.size;
            break;
        }

        cursor = hdr.offset + hdr.size;
    }

    if (!found_meta) {
        fprintf(stderr, "no top-level 'meta' box found (unsupported)\n");
        fclose(f);
        return 1;
    }

    MetaState st;
    memset(&st, 0, sizeof(st));

    char err[256];
    if (!parse_meta(f, file_size, meta_off, meta_end, &st, err, sizeof(err))) {
        fprintf(stderr, "m1 parse failed: %s\n", err);
        fclose(f);
        return 1;
    }

    dump_summary(path, &st);

    int rc = 0;
    if (extract_out) {
        if (!extract_primary(f, file_size, &st, extract_out, err, sizeof(err))) {
            fprintf(stderr, "extract failed: %s\n", err);
            rc = 1;
        } else {
            fprintf(stderr, "extracted primary payload to %s\n", extract_out);
        }
    }

    // cleanup
    for (size_t i = 0; i < st.item_count; i++) {
        free(st.items[i].extents);
        free(st.items[i].props);
    }
    free(st.items);
    free(st.props);

    fclose(f);
    return rc;
}
