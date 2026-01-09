#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "av1_decode_tile.h"
#include "av1_symbol.h"

// m3b (step 1): parse AV1 Sequence Header + enough of the uncompressed frame header
// to determine the coded frame size for reduced still-picture bitstreams.
//
// This does NOT decode pixels. It is a prerequisite tool to safely locate tile data later.

static void usage(FILE *out) {
    fprintf(out,
            "Usage: av1_framehdr [--dump-tiles DIR] [--check-tile-trailing] [--check-tile-trailing-strict] [--tile-consume-bools N] [--check-tile-trailingbits] [--check-tile-trailingbits-strict] [--decode-tile-syntax] [--decode-tile-syntax-strict] <in.av1>\n"
            "\n"
            "Parses a size-delimited AV1 OBU stream and prints basic frame header info.\n"
            "Current scope: still_picture=1 (reduced-still or non-reduced keyframe).\n"
            "Also parses tile_info() and, when tiles are carried in OBU_TILE_GROUP OBUs,\n"
            "prints per-tile payload byte ranges.\n"
            "\n"
            "Options:\n"
            "  --dump-tiles DIR        Write each tile payload as a .bin file into DIR\n"
            "  --check-tile-trailing   Probe tile payload with init/exit_symbol (may fail until tile decode exists)\n"
            "  --check-tile-trailing-strict   Same as above, but fails on first violation\n"
            "  --tile-consume-bools N  When used with --check-tile-trailing*, decode N bool symbols before exit_symbol()\n"
            "  --check-tile-trailingbits      Check tile trailing bits pattern (meaningful without tile decode)\n"
            "  --check-tile-trailingbits-strict   Same as above, but fails on first violation\n"
            "  --decode-tile-syntax    Call the m3b tile syntax probe (currently expected to report UNSUPPORTED)\n"
            "  --decode-tile-syntax-strict   Same as above, but fails on first UNSUPPORTED/ERROR\n");
}
static const char *tx_size_name(uint32_t tx_size) {
    // Matches common AV1 TxSize ordering (see m3b tile probe).
    switch (tx_size) {
    case 0:
        return "TX_4X4";
    case 1:
        return "TX_8X8";
    case 2:
        return "TX_16X16";
    case 3:
        return "TX_32X32";
    case 4:
        return "TX_64X64";
    case 5:
        return "TX_4X8";
    case 6:
        return "TX_8X4";
    case 7:
        return "TX_8X16";
    case 8:
        return "TX_16X8";
    case 9:
        return "TX_16X32";
    case 10:
        return "TX_32X16";
    case 11:
        return "TX_32X64";
    case 12:
        return "TX_64X32";
    case 13:
        return "TX_4X16";
    case 14:
        return "TX_16X4";
    case 15:
        return "TX_8X32";
    case 16:
        return "TX_32X8";
    case 17:
        return "TX_16X64";
    case 18:
        return "TX_64X16";
    default:
        return "TX_INVALID";
    }
}

static bool write_bytes_file(const char *path, const uint8_t *data, size_t len, char *err, size_t err_cap) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_cap, "failed to open %s: %s", path, strerror(errno));
        return false;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fclose(f);
        snprintf(err, err_cap, "failed to write %s", path);
        return false;
    }
    fclose(f);
    return true;
}

// write_text_file_frame_info() is defined later, after the SeqHdr/FrameHdr/TileInfo typedefs.

typedef struct {
    const char *dir;
    unsigned tg_index;
    unsigned tiles_written;
} TileDumpCtx;

static bool get_file_size(FILE *f, uint64_t *out_size) {
    off_t cur = ftello(f);
    if (cur < 0) {
        return false;
    }
    if (fseeko(f, 0, SEEK_END) != 0) {
        return false;
    }
    off_t end = ftello(f);
    if (end < 0) {
        return false;
    }
    if (fseeko(f, cur, SEEK_SET) != 0) {
        return false;
    }
    *out_size = (uint64_t)end;
    return true;
}

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
    const uint8_t *data;
    size_t size;
    uint64_t bitpos;
} BitReader;

static bool br_read_bit(BitReader *br, uint32_t *out) {
    if ((br->bitpos >> 3) >= br->size) {
        return false;
    }
    uint8_t byte = br->data[br->bitpos >> 3];
    unsigned shift = 7u - (unsigned)(br->bitpos & 7u);
    *out = (byte >> shift) & 1u;
    br->bitpos++;
    return true;
}

static bool br_read_bits(BitReader *br, unsigned n, uint32_t *out) {
    if (n == 0) {
        *out = 0;
        return true;
    }
    if (n > 32) {
        return false;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t b;
        if (!br_read_bit(br, &b)) {
            return false;
        }
        v = (v << 1) | b;
    }
    *out = v;
    return true;
}

static bool br_byte_align(BitReader *br) {
    uint64_t mod = br->bitpos & 7u;
    if (mod == 0) {
        return true;
    }
    uint64_t next = br->bitpos + (8u - mod);
    if ((next >> 3) > br->size) {
        return false;
    }
    br->bitpos = next;
    return true;
}

static bool br_byte_align_zero(BitReader *br, char *err, size_t err_cap);

static uint32_t floor_log2_u32(uint32_t n) {
    uint32_t r = 0;
    while (n >= 2) {
        n >>= 1;
        r++;
    }
    return r;
}

// ns(n): unsigned encoded integer in [0, n-1]
static bool br_read_ns(BitReader *br, uint32_t n, uint32_t *out) {
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        *out = 0;
        return true;
    }
    uint32_t w = floor_log2_u32(n) + 1;
    uint32_t m = (1u << w) - n;

    uint32_t v;
    if (!br_read_bits(br, w - 1, &v)) {
        return false;
    }
    if (v < m) {
        *out = v;
        return true;
    }
    uint32_t extra;
    if (!br_read_bit(br, &extra)) {
        return false;
    }
    *out = (v << 1) - m + extra;
    return true;
}

static void keep_helpers_linked(void) {
    // Keep helpers available (next m3b steps will use them).
    (void)br_byte_align;
    (void)br_read_ns;
}

typedef struct {
    bool found;
    uint64_t payload_off;
    uint64_t payload_size;
} ObuPayload;

static bool find_first_obu_payload(const uint8_t *data,
                                  size_t data_len,
                                  uint8_t wanted_type,
                                  ObuPayload *out,
                                  char *err,
                                  size_t err_cap) {
    out->found = false;
    out->payload_off = 0;
    out->payload_size = 0;

    size_t off = 0;
    while (off < data_len) {
        if (data[off] == 0) {
            size_t z = off;
            while (z < data_len && data[z] == 0) {
                z++;
            }
            if (z == data_len) {
                return true;
            }
        }

        if (off >= data_len) {
            break;
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
            snprintf(err, err_cap, "OBU has_size_field=0 (unsupported)");
            return false;
        }

        if (extension_flag) {
            if (off >= data_len) {
                snprintf(err, err_cap, "truncated obu_extension_header");
                return false;
            }
            off += 1;
        }

        uint64_t obu_size;
        if (!read_leb128_u64(data, data_len, &off, &obu_size)) {
            snprintf(err, err_cap, "failed to read obu_size");
            return false;
        }
        if (obu_size > (uint64_t)(data_len - off)) {
            snprintf(err, err_cap, "obu_size exceeds remaining bytes");
            return false;
        }

        if (obu_type == wanted_type) {
            out->found = true;
            out->payload_off = (uint64_t)off;
            out->payload_size = obu_size;
            return true;
        }

        off += (size_t)obu_size;
    }
    return true;
}

typedef struct {
    // Sequence header essentials for our reduced-still frame header parsing.
    uint32_t still_picture;
    uint32_t reduced_still_picture_header;

    uint32_t frame_width_bits_minus_1;
    uint32_t frame_height_bits_minus_1;

    uint32_t timing_info_present_flag;
    uint32_t decoder_model_info_present_flag;
    uint32_t equal_picture_interval;

    uint32_t frame_id_numbers_present_flag;
    uint32_t additional_frame_id_length_minus_1;
    uint32_t delta_frame_id_length_minus_2;

    uint32_t max_frame_width_minus_1;
    uint32_t max_frame_height_minus_1;

    uint32_t enable_order_hint;
    uint32_t order_hint_bits_minus_1;

    // Values: 0/1, or 2 for SELECT_*
    uint32_t seq_force_screen_content_tools;
    uint32_t seq_force_integer_mv;

    uint32_t use_128x128_superblock;
    uint32_t enable_filter_intra;
    uint32_t enable_intra_edge_filter;
    uint32_t enable_superres;

    // Needed to skip the remainder of the uncompressed header (m3b.3: tiles embedded in OBU_FRAME).
    uint32_t enable_cdef;
    uint32_t enable_restoration;

    // color_config() essentials
    uint32_t mono_chrome;
    uint32_t num_planes;
    uint32_t subsampling_x;
    uint32_t subsampling_y;
    uint32_t separate_uv_delta_q;

    // Sequence header tail
    uint32_t film_grain_params_present;
} SeqHdr;

static bool br_read_su(BitReader *br, unsigned n, int32_t *out) {
    if (n == 0 || n > 32) {
        return false;
    }
    uint32_t u;
    if (!br_read_bits(br, n, &u)) {
        return false;
    }
    if (n == 32) {
        *out = (int32_t)u;
        return true;
    }
    uint32_t sign_bit = 1u << (n - 1u);
    if (u & sign_bit) {
        uint32_t ext_mask = ~((1u << n) - 1u);
        u |= ext_mask;
    }
    *out = (int32_t)u;
    return true;
}

static int32_t i32_clip3(int32_t lo, int32_t hi, int32_t x) {
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static bool parse_color_config_min(BitReader *br,
                                   uint32_t seq_profile,
                                   SeqHdr *out,
                                   char *err,
                                   size_t err_cap) {
    uint32_t high_bitdepth;
    if (!br_read_bit(br, &high_bitdepth)) {
        snprintf(err, err_cap, "truncated high_bitdepth");
        return false;
    }

    uint32_t BitDepth = 8;
    if (seq_profile == 2 && high_bitdepth) {
        uint32_t twelve_bit;
        if (!br_read_bit(br, &twelve_bit)) {
            snprintf(err, err_cap, "truncated twelve_bit");
            return false;
        }
        BitDepth = twelve_bit ? 12 : 10;
    } else if (seq_profile <= 2) {
        BitDepth = high_bitdepth ? 10 : 8;
    } else {
        snprintf(err, err_cap, "unsupported seq_profile");
        return false;
    }

    uint32_t mono_chrome = 0;
    if (seq_profile == 1) {
        mono_chrome = 0;
    } else {
        if (!br_read_bit(br, &mono_chrome)) {
            snprintf(err, err_cap, "truncated mono_chrome");
            return false;
        }
    }
    out->mono_chrome = mono_chrome;
    out->num_planes = mono_chrome ? 1u : 3u;

    uint32_t color_description_present_flag;
    if (!br_read_bit(br, &color_description_present_flag)) {
        snprintf(err, err_cap, "truncated color_description_present_flag");
        return false;
    }

    uint32_t color_primaries = 2;              // CP_UNSPECIFIED
    uint32_t transfer_characteristics = 2;     // TC_UNSPECIFIED
    uint32_t matrix_coefficients = 2;          // MC_UNSPECIFIED
    if (color_description_present_flag) {
        if (!br_read_bits(br, 8, &color_primaries) || !br_read_bits(br, 8, &transfer_characteristics) ||
            !br_read_bits(br, 8, &matrix_coefficients)) {
            snprintf(err, err_cap, "truncated color_description");
            return false;
        }
    }

    if (mono_chrome) {
        uint32_t color_range;
        if (!br_read_bit(br, &color_range)) {
            snprintf(err, err_cap, "truncated color_range");
            return false;
        }
        (void)color_range;
        out->subsampling_x = 1;
        out->subsampling_y = 1;
        out->separate_uv_delta_q = 0;
        return true;
    }

    // Special-case: RGB identity (no extra bits here except separate_uv_delta_q below).
    if (color_primaries == 1 /* CP_BT_709 */ && transfer_characteristics == 13 /* TC_SRGB */ &&
        matrix_coefficients == 0 /* MC_IDENTITY */) {
        out->subsampling_x = 0;
        out->subsampling_y = 0;
    } else {
        uint32_t color_range;
        if (!br_read_bit(br, &color_range)) {
            snprintf(err, err_cap, "truncated color_range");
            return false;
        }
        (void)color_range;

        uint32_t subsampling_x = 0;
        uint32_t subsampling_y = 0;
        if (seq_profile == 0) {
            subsampling_x = 1;
            subsampling_y = 1;
        } else if (seq_profile == 1) {
            subsampling_x = 0;
            subsampling_y = 0;
        } else {
            if (BitDepth == 12) {
                if (!br_read_bit(br, &subsampling_x)) {
                    snprintf(err, err_cap, "truncated subsampling_x");
                    return false;
                }
                if (subsampling_x) {
                    if (!br_read_bit(br, &subsampling_y)) {
                        snprintf(err, err_cap, "truncated subsampling_y");
                        return false;
                    }
                } else {
                    subsampling_y = 0;
                }
            } else {
                subsampling_x = 1;
                subsampling_y = 0;
            }
        }
        out->subsampling_x = subsampling_x;
        out->subsampling_y = subsampling_y;
        if (subsampling_x && subsampling_y) {
            uint32_t chroma_sample_position;
            if (!br_read_bits(br, 2, &chroma_sample_position)) {
                snprintf(err, err_cap, "truncated chroma_sample_position");
                return false;
            }
        }
    }

    uint32_t separate_uv_delta_q;
    if (!br_read_bit(br, &separate_uv_delta_q)) {
        snprintf(err, err_cap, "truncated separate_uv_delta_q");
        return false;
    }
    out->separate_uv_delta_q = separate_uv_delta_q;
    return true;
}

static bool parse_seq_hdr_min(const uint8_t *payload,
                              size_t payload_len,
                              SeqHdr *out,
                              char *err,
                              size_t err_cap) {
    memset(out, 0, sizeof(*out));

    BitReader br = {payload, payload_len, 0};

    uint32_t seq_profile;
    uint32_t still_picture;
    uint32_t reduced_still_picture_header;
    if (!br_read_bits(&br, 3, &seq_profile) || !br_read_bit(&br, &still_picture) ||
        !br_read_bit(&br, &reduced_still_picture_header)) {
        snprintf(err, err_cap, "truncated sequence header");
        return false;
    }
    (void)seq_profile;

    out->still_picture = still_picture;
    out->reduced_still_picture_header = reduced_still_picture_header;

    uint32_t timing_info_present_flag = 0;
    uint32_t decoder_model_info_present_flag = 0;
    uint32_t equal_picture_interval = 0;
    uint32_t buffer_delay_length_minus_1 = 0;
    uint32_t initial_display_delay_present_flag = 0;
    uint32_t operating_points_cnt_minus_1 = 0;

    if (reduced_still_picture_header) {
        // Reduced still-picture: several fields are forced, but parsing continues after the branch.
        // seq_level_idx[0]
        uint32_t tmp;
        if (!br_read_bits(&br, 5, &tmp)) {
            snprintf(err, err_cap, "truncated seq_level_idx");
            return false;
        }
        timing_info_present_flag = 0;
        decoder_model_info_present_flag = 0;
        equal_picture_interval = 0;
        initial_display_delay_present_flag = 0;
        operating_points_cnt_minus_1 = 0;
    } else {
        if (!br_read_bit(&br, &timing_info_present_flag)) {
            snprintf(err, err_cap, "truncated timing_info_present_flag");
            return false;
        }
        if (timing_info_present_flag) {
            uint32_t tmp32;
            if (!br_read_bits(&br, 32, &tmp32) || !br_read_bits(&br, 32, &tmp32)) {
                snprintf(err, err_cap, "truncated timing_info");
                return false;
            }
            if (!br_read_bit(&br, &equal_picture_interval)) {
                return false;
            }
            if (equal_picture_interval) {
                uint32_t leading = 0;
                uint32_t b;
                while (true) {
                    if (!br_read_bit(&br, &b)) {
                        return false;
                    }
                    if (b == 1) {
                        break;
                    }
                    leading++;
                    if (leading > 31) {
                        snprintf(err, err_cap, "uvlc too long");
                        return false;
                    }
                }
                if (leading) {
                    uint32_t tmp;
                    if (!br_read_bits(&br, leading, &tmp)) {
                        return false;
                    }
                }
            }
            if (!br_read_bit(&br, &decoder_model_info_present_flag)) {
                return false;
            }
            if (decoder_model_info_present_flag) {
                uint32_t t;
                if (!br_read_bits(&br, 5, &t)) {
                    return false;
                }
                buffer_delay_length_minus_1 = t;
                if (!br_read_bits(&br, 32, &t) || !br_read_bits(&br, 5, &t) || !br_read_bits(&br, 5, &t)) {
                    return false;
                }
            }
        }

        if (!br_read_bit(&br, &initial_display_delay_present_flag)) {
            return false;
        }
        if (!br_read_bits(&br, 5, &operating_points_cnt_minus_1)) {
            return false;
        }

        for (uint32_t i = 0; i <= operating_points_cnt_minus_1; i++) {
            uint32_t tmp;
            if (!br_read_bits(&br, 12, &tmp) || !br_read_bits(&br, 5, &tmp)) {
                return false;
            }
            if (tmp > 7) {
                if (!br_read_bit(&br, &tmp)) {
                    return false;
                }
            }
            if (decoder_model_info_present_flag) {
                uint32_t present;
                if (!br_read_bit(&br, &present)) {
                    return false;
                }
                if (present) {
                    unsigned n = buffer_delay_length_minus_1 + 1;
                    if (n > 32) {
                        snprintf(err, err_cap, "unsupported buffer_delay_length_minus_1");
                        return false;
                    }
                    if (!br_read_bits(&br, n, &tmp) || !br_read_bits(&br, n, &tmp) || !br_read_bit(&br, &tmp)) {
                        return false;
                    }
                }
            }
            if (initial_display_delay_present_flag) {
                uint32_t present;
                if (!br_read_bit(&br, &present)) {
                    return false;
                }
                if (present) {
                    if (!br_read_bits(&br, 4, &tmp)) {
                        return false;
                    }
                }
            }
        }
    }

    // choose_operating_point() is a function, not a bitstream element; for reduced-still, it is effectively 0.
    // Parse max frame width/height.
    uint32_t frame_width_bits_minus_1;
    uint32_t frame_height_bits_minus_1;
    if (!br_read_bits(&br, 4, &frame_width_bits_minus_1) || !br_read_bits(&br, 4, &frame_height_bits_minus_1)) {
        return false;
    }
    out->frame_width_bits_minus_1 = frame_width_bits_minus_1;
    out->frame_height_bits_minus_1 = frame_height_bits_minus_1;
    uint32_t mw, mh;
    if (!br_read_bits(&br, frame_width_bits_minus_1 + 1, &mw) || !br_read_bits(&br, frame_height_bits_minus_1 + 1, &mh)) {
        return false;
    }
    out->max_frame_width_minus_1 = mw;
    out->max_frame_height_minus_1 = mh;

    // frame_id_numbers_present_flag
    if (reduced_still_picture_header) {
        out->frame_id_numbers_present_flag = 0;
        out->additional_frame_id_length_minus_1 = 0;
        out->delta_frame_id_length_minus_2 = 0;
    } else {
        uint32_t frame_id_numbers_present_flag;
        if (!br_read_bit(&br, &frame_id_numbers_present_flag)) {
            return false;
        }
        out->frame_id_numbers_present_flag = frame_id_numbers_present_flag;
        if (frame_id_numbers_present_flag) {
            uint32_t a, b;
            if (!br_read_bits(&br, 4, &b) || !br_read_bits(&br, 3, &a)) {
                return false;
            }
            out->delta_frame_id_length_minus_2 = b;
            out->additional_frame_id_length_minus_1 = a;
        }
    }

    // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    uint32_t tmp;
    if (!br_read_bit(&br, &tmp)) {
        return false;
    }
    out->use_128x128_superblock = tmp;
    if (!br_read_bit(&br, &tmp)) {
        return false;
    }
    out->enable_filter_intra = tmp;
    if (!br_read_bit(&br, &tmp)) {
        return false;
    }
    out->enable_intra_edge_filter = tmp;

    if (reduced_still_picture_header) {
        // enable_interintra_compound/masked/warped/dual_filter/order_hint/jnt_comp/ref_frame_mvs are forced to 0.
        out->enable_order_hint = 0;
        out->order_hint_bits_minus_1 = 0;

        // For reduced still, forced to SELECT_*.
        out->seq_force_screen_content_tools = 2;
        out->seq_force_integer_mv = 2;
    } else {
        // Skip: enable_interintra_compound, enable_masked_compound, enable_warped_motion, enable_dual_filter
        for (int i = 0; i < 4; i++) {
            if (!br_read_bit(&br, &tmp)) {
                return false;
            }
        }

        // enable_order_hint
        uint32_t enable_order_hint;
        if (!br_read_bit(&br, &enable_order_hint)) {
            return false;
        }
        out->enable_order_hint = enable_order_hint;

        if (enable_order_hint) {
            // enable_jnt_comp, enable_ref_frame_mvs
            if (!br_read_bit(&br, &tmp) || !br_read_bit(&br, &tmp)) {
                return false;
            }
        }

        // seq_choose_screen_content_tools / seq_force_screen_content_tools
        uint32_t seq_choose_screen_content_tools;
        if (!br_read_bit(&br, &seq_choose_screen_content_tools)) {
            return false;
        }
        if (seq_choose_screen_content_tools) {
            out->seq_force_screen_content_tools = 2; // SELECT_SCREEN_CONTENT_TOOLS
        } else {
            uint32_t v;
            if (!br_read_bit(&br, &v)) {
                return false;
            }
            out->seq_force_screen_content_tools = v;
        }

        // seq_choose_integer_mv / seq_force_integer_mv
        if (out->seq_force_screen_content_tools > 0) {
            uint32_t seq_choose_integer_mv;
            if (!br_read_bit(&br, &seq_choose_integer_mv)) {
                return false;
            }
            if (seq_choose_integer_mv) {
                out->seq_force_integer_mv = 2; // SELECT_INTEGER_MV
            } else {
                uint32_t v;
                if (!br_read_bit(&br, &v)) {
                    return false;
                }
                out->seq_force_integer_mv = v;
            }
        } else {
            // Note: per spec, this is set to SELECT_INTEGER_MV.
            out->seq_force_integer_mv = 2;
        }

        if (enable_order_hint) {
            // order_hint_bits_minus_1
            if (!br_read_bits(&br, 3, &tmp)) {
                return false;
            }
            out->order_hint_bits_minus_1 = tmp;
        } else {
            out->order_hint_bits_minus_1 = 0;
        }
    }

    // enable_superres, enable_cdef, enable_restoration
    uint32_t enable_superres;
    if (!br_read_bit(&br, &enable_superres)) {
        return false;
    }
    out->enable_superres = enable_superres;

    uint32_t enable_cdef;
    uint32_t enable_restoration;
    if (!br_read_bit(&br, &enable_cdef) || !br_read_bit(&br, &enable_restoration)) {
        return false;
    }
    out->enable_cdef = enable_cdef;
    out->enable_restoration = enable_restoration;

    if (!parse_color_config_min(&br, seq_profile, out, err, err_cap)) {
        return false;
    }

    uint32_t film_grain_params_present;
    if (!br_read_bit(&br, &film_grain_params_present)) {
        snprintf(err, err_cap, "truncated film_grain_params_present");
        return false;
    }
    out->film_grain_params_present = film_grain_params_present;

    out->timing_info_present_flag = timing_info_present_flag;
    out->decoder_model_info_present_flag = decoder_model_info_present_flag;
    out->equal_picture_interval = equal_picture_interval;

    return true;
}

typedef struct {
    uint32_t frame_type;
    uint32_t show_frame;
    uint32_t error_resilient_mode;

    uint32_t disable_cdf_update;

    uint32_t frame_width;
    uint32_t frame_height;

    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t upscaled_width;

    uint32_t mi_cols;
    uint32_t mi_rows;

    uint32_t allow_screen_content_tools;

    uint32_t allow_intrabc;

    // Derived per spec from quantization + segmentation state.
    uint32_t coded_lossless;

    // Quantization state (useful for coeff CDF init later).
    uint32_t base_q_idx;

    // read_tx_mode() derived state:
    // 0 = ONLY_4X4, 1 = TX_MODE_LARGEST, 2 = TX_MODE_SELECT.
    uint32_t tx_mode;

    // reduced_tx_set (syntax element; used by get_tx_set() for tx_type parsing).
    uint32_t reduced_tx_set;
} FrameHdr;

enum {
    MAX_SEGMENTS = 8,
    SEG_LVL_MAX = 8,
    SEG_LVL_ALT_Q = 0,
};

typedef struct {
    uint32_t base_q_idx;
    int32_t DeltaQYDc;
    int32_t DeltaQUDc;
    int32_t DeltaQUAc;
    int32_t DeltaQVDc;
    int32_t DeltaQVAc;

    uint32_t delta_q_present;
} QuantizationState;

typedef struct {
    uint32_t segmentation_enabled;
    int32_t feature_data_alt_q[MAX_SEGMENTS];
    uint8_t feature_enabled_alt_q[MAX_SEGMENTS];
} SegmentationState;

static bool parse_quantization_params_skip(BitReader *br,
                                           const SeqHdr *seq,
                                           QuantizationState *qs,
                                           char *err,
                                           size_t err_cap);

static bool parse_segmentation_params_skip(BitReader *br,
                                           SegmentationState *ss,
                                           char *err,
                                           size_t err_cap);

static uint32_t compute_coded_lossless(const QuantizationState *qs, const SegmentationState *ss);

static bool read_delta_q(BitReader *br, int32_t *out, char *err, size_t err_cap) {
    uint32_t delta_coded;
    if (!br_read_bit(br, &delta_coded)) {
        snprintf(err, err_cap, "truncated delta_coded");
        return false;
    }
    if (!delta_coded) {
        *out = 0;
        return true;
    }
    int32_t delta_q;
    if (!br_read_su(br, 7, &delta_q)) {
        snprintf(err, err_cap, "truncated delta_q");
        return false;
    }
    *out = delta_q;
    return true;
}

static bool parse_quantization_params_skip(BitReader *br,
                                           const SeqHdr *seq,
                                           QuantizationState *qs,
                                           char *err,
                                           size_t err_cap) {
    memset(qs, 0, sizeof(*qs));

    uint32_t base_q_idx;
    if (!br_read_bits(br, 8, &base_q_idx)) {
        snprintf(err, err_cap, "truncated base_q_idx");
        return false;
    }
    qs->base_q_idx = base_q_idx;

    if (!read_delta_q(br, &qs->DeltaQYDc, err, err_cap)) {
        return false;
    }

    if (seq->num_planes > 1) {
        uint32_t diff_uv_delta = 0;
        if (seq->separate_uv_delta_q) {
            if (!br_read_bit(br, &diff_uv_delta)) {
                snprintf(err, err_cap, "truncated diff_uv_delta");
                return false;
            }
        }
        if (!read_delta_q(br, &qs->DeltaQUDc, err, err_cap) || !read_delta_q(br, &qs->DeltaQUAc, err, err_cap)) {
            return false;
        }
        if (diff_uv_delta) {
            if (!read_delta_q(br, &qs->DeltaQVDc, err, err_cap) || !read_delta_q(br, &qs->DeltaQVAc, err, err_cap)) {
                return false;
            }
        } else {
            qs->DeltaQVDc = qs->DeltaQUDc;
            qs->DeltaQVAc = qs->DeltaQUAc;
        }
    } else {
        qs->DeltaQUDc = 0;
        qs->DeltaQUAc = 0;
        qs->DeltaQVDc = 0;
        qs->DeltaQVAc = 0;
    }

    uint32_t using_qmatrix;
    if (!br_read_bit(br, &using_qmatrix)) {
        snprintf(err, err_cap, "truncated using_qmatrix");
        return false;
    }
    if (using_qmatrix) {
        uint32_t tmp;
        if (!br_read_bits(br, 4, &tmp) || !br_read_bits(br, 4, &tmp)) {
            snprintf(err, err_cap, "truncated qmatrix");
            return false;
        }
        if (seq->separate_uv_delta_q) {
            if (!br_read_bits(br, 4, &tmp)) {
                snprintf(err, err_cap, "truncated qm_v");
                return false;
            }
        }
    }

    return true;
}

static bool parse_segmentation_params_skip(BitReader *br,
                                           SegmentationState *ss,
                                           char *err,
                                           size_t err_cap) {
    memset(ss, 0, sizeof(*ss));

    uint32_t segmentation_enabled;
    if (!br_read_bit(br, &segmentation_enabled)) {
        snprintf(err, err_cap, "truncated segmentation_enabled");
        return false;
    }
    ss->segmentation_enabled = segmentation_enabled;
    if (!segmentation_enabled) {
        return true;
    }

    // For our still-picture subset, primary_ref_frame is PRIMARY_REF_NONE.
    uint32_t segmentation_update_data = 1;

    if (segmentation_update_data) {
        // Parse only what we need for CodedLossless: SEG_LVL_ALT_Q.
        // Still we must consume all bits of segmentation_update_data.
        static const uint8_t Segmentation_Feature_Bits[SEG_LVL_MAX] = {8, 6, 6, 6, 6, 3, 0, 0};
        static const uint8_t Segmentation_Feature_Signed[SEG_LVL_MAX] = {1, 1, 1, 1, 1, 0, 0, 0};
        static const int32_t Segmentation_Feature_Max[SEG_LVL_MAX] = {255, 63, 63, 63, 63, 7, 0, 0};

        for (uint32_t i = 0; i < MAX_SEGMENTS; i++) {
            for (uint32_t j = 0; j < SEG_LVL_MAX; j++) {
                uint32_t feature_enabled;
                if (!br_read_bit(br, &feature_enabled)) {
                    snprintf(err, err_cap, "truncated feature_enabled");
                    return false;
                }
                if (!feature_enabled) {
                    continue;
                }
                uint8_t bitsToRead = Segmentation_Feature_Bits[j];
                int32_t limit = Segmentation_Feature_Max[j];
                int32_t clippedValue = 0;
                if (Segmentation_Feature_Signed[j]) {
                    int32_t feature_value;
                    if (!br_read_su(br, 1u + (unsigned)bitsToRead, &feature_value)) {
                        snprintf(err, err_cap, "truncated signed feature_value");
                        return false;
                    }
                    clippedValue = i32_clip3(-limit, limit, feature_value);
                } else {
                    uint32_t feature_value_u;
                    if (bitsToRead > 0) {
                        if (!br_read_bits(br, bitsToRead, &feature_value_u)) {
                            snprintf(err, err_cap, "truncated feature_value");
                            return false;
                        }
                    } else {
                        feature_value_u = 0;
                    }
                    clippedValue = (int32_t)i32_clip3(0, limit, (int32_t)feature_value_u);
                }

                if (j == SEG_LVL_ALT_Q) {
                    ss->feature_enabled_alt_q[i] = 1;
                    ss->feature_data_alt_q[i] = clippedValue;
                }
            }
        }
    }

    return true;
}

static bool parse_delta_q_params_skip(BitReader *br,
                                      QuantizationState *qs,
                                      char *err,
                                      size_t err_cap) {
    uint32_t delta_q_present = 0;
    if (qs->base_q_idx > 0) {
        if (!br_read_bit(br, &delta_q_present)) {
            snprintf(err, err_cap, "truncated delta_q_present");
            return false;
        }
    }
    qs->delta_q_present = delta_q_present;
    if (delta_q_present) {
        uint32_t tmp;
        if (!br_read_bits(br, 2, &tmp)) {
            snprintf(err, err_cap, "truncated delta_q_res");
            return false;
        }
    }
    return true;
}

static bool parse_delta_lf_params_skip(BitReader *br,
                                       const QuantizationState *qs,
                                       uint32_t allow_intrabc,
                                       char *err,
                                       size_t err_cap) {
    if (!qs->delta_q_present) {
        return true;
    }
    if (!allow_intrabc) {
        uint32_t delta_lf_present;
        if (!br_read_bit(br, &delta_lf_present)) {
            snprintf(err, err_cap, "truncated delta_lf_present");
            return false;
        }
        if (delta_lf_present) {
            uint32_t tmp;
            if (!br_read_bits(br, 2, &tmp) || !br_read_bit(br, &tmp)) {
                snprintf(err, err_cap, "truncated delta_lf_res/multi");
                return false;
            }
        }
    }
    return true;
}

static uint32_t compute_coded_lossless(const QuantizationState *qs, const SegmentationState *ss) {
    // Derived per spec from LosslessArray[] computed over all segments.
    for (uint32_t segmentId = 0; segmentId < MAX_SEGMENTS; segmentId++) {
        int32_t qindex = (int32_t)qs->base_q_idx;
        if (ss->segmentation_enabled && ss->feature_enabled_alt_q[segmentId]) {
            qindex += ss->feature_data_alt_q[segmentId];
        }
        qindex = i32_clip3(0, 255, qindex);
        bool lossless = (qindex == 0) && (qs->DeltaQYDc == 0) && (qs->DeltaQUDc == 0) && (qs->DeltaQUAc == 0) &&
                        (qs->DeltaQVDc == 0) && (qs->DeltaQVAc == 0);
        if (!lossless) {
            return 0;
        }
    }
    return 1;
}

static bool parse_loop_filter_params_skip(BitReader *br,
                                          uint32_t CodedLossless,
                                          uint32_t allow_intrabc,
                                          uint32_t NumPlanes,
                                          char *err,
                                          size_t err_cap) {
    if (CodedLossless || allow_intrabc) {
        return true;
    }
    uint32_t level0;
    uint32_t level1;
    uint32_t tmp;
    if (!br_read_bits(br, 6, &level0) || !br_read_bits(br, 6, &level1)) {
        snprintf(err, err_cap, "truncated loop_filter_level[0/1]");
        return false;
    }
    if (NumPlanes > 1) {
        if (level0 || level1) {
            if (!br_read_bits(br, 6, &tmp) || !br_read_bits(br, 6, &tmp)) {
                snprintf(err, err_cap, "truncated loop_filter_level[2/3]");
                return false;
            }
        }
    }
    if (!br_read_bits(br, 3, &tmp) || !br_read_bit(br, &tmp)) {
        snprintf(err, err_cap, "truncated loop_filter_sharpness/delta_enabled");
        return false;
    }
    uint32_t loop_filter_delta_enabled = tmp;
    if (loop_filter_delta_enabled) {
        uint32_t loop_filter_delta_update;
        if (!br_read_bit(br, &loop_filter_delta_update)) {
            snprintf(err, err_cap, "truncated loop_filter_delta_update");
            return false;
        }
        if (loop_filter_delta_update) {
            // TOTAL_REFS_PER_FRAME = 8
            for (uint32_t i = 0; i < 8; i++) {
                uint32_t update_ref_delta;
                if (!br_read_bit(br, &update_ref_delta)) {
                    snprintf(err, err_cap, "truncated update_ref_delta");
                    return false;
                }
                if (update_ref_delta) {
                    int32_t v;
                    if (!br_read_su(br, 7, &v)) {
                        snprintf(err, err_cap, "truncated loop_filter_ref_deltas");
                        return false;
                    }
                }
            }
            for (uint32_t i = 0; i < 2; i++) {
                uint32_t update_mode_delta;
                if (!br_read_bit(br, &update_mode_delta)) {
                    snprintf(err, err_cap, "truncated update_mode_delta");
                    return false;
                }
                if (update_mode_delta) {
                    int32_t v;
                    if (!br_read_su(br, 7, &v)) {
                        snprintf(err, err_cap, "truncated loop_filter_mode_deltas");
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool parse_cdef_params_skip(BitReader *br,
                                   uint32_t CodedLossless,
                                   uint32_t allow_intrabc,
                                   uint32_t enable_cdef,
                                   uint32_t NumPlanes,
                                   char *err,
                                   size_t err_cap) {
    if (CodedLossless || allow_intrabc || !enable_cdef) {
        return true;
    }
    uint32_t tmp;
    if (!br_read_bits(br, 2, &tmp) || !br_read_bits(br, 2, &tmp)) {
        snprintf(err, err_cap, "truncated cdef_damping_minus_3/cdef_bits");
        return false;
    }
    uint32_t cdef_bits = tmp;
    uint32_t n = 1u << cdef_bits;
    for (uint32_t i = 0; i < n; i++) {
        if (!br_read_bits(br, 4, &tmp) || !br_read_bits(br, 2, &tmp)) {
            snprintf(err, err_cap, "truncated cdef y strengths");
            return false;
        }
        if (NumPlanes > 1) {
            if (!br_read_bits(br, 4, &tmp) || !br_read_bits(br, 2, &tmp)) {
                snprintf(err, err_cap, "truncated cdef uv strengths");
                return false;
            }
        }
    }
    return true;
}

static bool parse_lr_params_skip(BitReader *br,
                                 uint32_t AllLossless,
                                 uint32_t allow_intrabc,
                                 uint32_t enable_restoration,
                                 uint32_t NumPlanes,
                                 uint32_t use_128x128_superblock,
                                 uint32_t subsampling_x,
                                 uint32_t subsampling_y,
                                 char *err,
                                 size_t err_cap) {
    if (AllLossless || allow_intrabc || !enable_restoration) {
        return true;
    }
    uint32_t UsesLr = 0;
    uint32_t usesChromaLr = 0;
    for (uint32_t i = 0; i < NumPlanes; i++) {
        uint32_t lr_type;
        if (!br_read_bits(br, 2, &lr_type)) {
            snprintf(err, err_cap, "truncated lr_type");
            return false;
        }
        if (lr_type != 0) {
            UsesLr = 1;
            if (i > 0) {
                usesChromaLr = 1;
            }
        }
    }
    if (UsesLr) {
        uint32_t lr_unit_shift;
        if (!br_read_bit(br, &lr_unit_shift)) {
            snprintf(err, err_cap, "truncated lr_unit_shift");
            return false;
        }
        if (use_128x128_superblock) {
            // lr_unit_shift++ in spec; no extra bits.
        } else {
            if (lr_unit_shift) {
                uint32_t lr_unit_extra_shift;
                if (!br_read_bit(br, &lr_unit_extra_shift)) {
                    snprintf(err, err_cap, "truncated lr_unit_extra_shift");
                    return false;
                }
            }
        }
        if (subsampling_x && subsampling_y && usesChromaLr) {
            uint32_t lr_uv_shift;
            if (!br_read_bit(br, &lr_uv_shift)) {
                snprintf(err, err_cap, "truncated lr_uv_shift");
                return false;
            }
        }
    }
    return true;
}

static bool parse_read_tx_mode(BitReader *br, uint32_t CodedLossless, uint32_t *out_tx_mode, char *err, size_t err_cap) {
    if (!out_tx_mode) {
        snprintf(err, err_cap, "invalid out_tx_mode");
        return false;
    }
    if (CodedLossless) {
        *out_tx_mode = 0; // ONLY_4X4
        return true;
    }

    uint32_t tx_mode_select;
    if (!br_read_bit(br, &tx_mode_select)) {
        snprintf(err, err_cap, "truncated tx_mode_select");
        return false;
    }

    *out_tx_mode = tx_mode_select ? 2u : 1u;
    return true;
}

static bool parse_film_grain_params_skip(BitReader *br,
                                         const SeqHdr *seq,
                                         uint32_t frame_type,
                                         uint32_t show_frame,
                                         char *err,
                                         size_t err_cap) {
    if (!seq->film_grain_params_present || (!show_frame /* && !showable_frame */)) {
        return true;
    }
    uint32_t apply_grain;
    if (!br_read_bit(br, &apply_grain)) {
        snprintf(err, err_cap, "truncated apply_grain");
        return false;
    }
    if (!apply_grain) {
        return true;
    }

    // film_grain_params() (AV1 spec): we only need to skip it safely.
    uint32_t tmp;
    if (!br_read_bits(br, 16, &tmp)) {
        snprintf(err, err_cap, "truncated grain_seed");
        return false;
    }

    uint32_t update_grain = 1;
    if (frame_type == 2 /* INTER_FRAME */) {
        if (!br_read_bit(br, &update_grain)) {
            snprintf(err, err_cap, "truncated update_grain");
            return false;
        }
    }

    if (!update_grain) {
        if (!br_read_bits(br, 3, &tmp)) {
            snprintf(err, err_cap, "truncated film_grain_params_ref_idx");
            return false;
        }
        return true;
    }

    uint32_t num_y_points;
    if (!br_read_bits(br, 4, &num_y_points)) {
        snprintf(err, err_cap, "truncated num_y_points");
        return false;
    }
    for (uint32_t i = 0; i < num_y_points; i++) {
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated point_y_value");
            return false;
        }
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated point_y_scaling");
            return false;
        }
    }

    uint32_t chroma_scaling_from_luma = 0;
    if (seq->mono_chrome) {
        chroma_scaling_from_luma = 0;
    } else {
        if (!br_read_bit(br, &chroma_scaling_from_luma)) {
            snprintf(err, err_cap, "truncated chroma_scaling_from_luma");
            return false;
        }
    }

    uint32_t num_cb_points = 0;
    uint32_t num_cr_points = 0;
    if (seq->mono_chrome || chroma_scaling_from_luma ||
        (seq->subsampling_x == 1 && seq->subsampling_y == 1 && num_y_points == 0)) {
        num_cb_points = 0;
        num_cr_points = 0;
    } else {
        if (!br_read_bits(br, 4, &num_cb_points)) {
            snprintf(err, err_cap, "truncated num_cb_points");
            return false;
        }
        for (uint32_t i = 0; i < num_cb_points; i++) {
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated point_cb_value");
                return false;
            }
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated point_cb_scaling");
                return false;
            }
        }
        if (!br_read_bits(br, 4, &num_cr_points)) {
            snprintf(err, err_cap, "truncated num_cr_points");
            return false;
        }
        for (uint32_t i = 0; i < num_cr_points; i++) {
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated point_cr_value");
                return false;
            }
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated point_cr_scaling");
                return false;
            }
        }
    }

    // grain_scaling_minus_8
    if (!br_read_bits(br, 2, &tmp)) {
        snprintf(err, err_cap, "truncated grain_scaling_minus_8");
        return false;
    }

    // ar_coeff_lag
    uint32_t ar_coeff_lag;
    if (!br_read_bits(br, 2, &ar_coeff_lag)) {
        snprintf(err, err_cap, "truncated ar_coeff_lag");
        return false;
    }
    uint32_t num_pos_luma = 2u * ar_coeff_lag * (ar_coeff_lag + 1u);
    uint32_t num_pos_chroma;
    if (num_y_points) {
        num_pos_chroma = num_pos_luma + 1u;
        for (uint32_t i = 0; i < num_pos_luma; i++) {
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated ar_coeffs_y_plus_128");
                return false;
            }
        }
    } else {
        num_pos_chroma = num_pos_luma;
    }

    if (chroma_scaling_from_luma || num_cb_points) {
        for (uint32_t i = 0; i < num_pos_chroma; i++) {
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated ar_coeffs_cb_plus_128");
                return false;
            }
        }
    }
    if (chroma_scaling_from_luma || num_cr_points) {
        for (uint32_t i = 0; i < num_pos_chroma; i++) {
            if (!br_read_bits(br, 8, &tmp)) {
                snprintf(err, err_cap, "truncated ar_coeffs_cr_plus_128");
                return false;
            }
        }
    }

    // ar_coeff_shift_minus_6
    if (!br_read_bits(br, 2, &tmp)) {
        snprintf(err, err_cap, "truncated ar_coeff_shift_minus_6");
        return false;
    }

    // grain_scale_shift
    if (!br_read_bits(br, 2, &tmp)) {
        snprintf(err, err_cap, "truncated grain_scale_shift");
        return false;
    }

    // overlap_flag, clip_to_restricted_range
    if (!br_read_bit(br, &tmp)) {
        snprintf(err, err_cap, "truncated overlap_flag");
        return false;
    }
    if (!br_read_bit(br, &tmp)) {
        snprintf(err, err_cap, "truncated clip_to_restricted_range");
        return false;
    }

    if (num_cb_points) {
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated cb_mult");
            return false;
        }
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated cb_luma_mult");
            return false;
        }
        if (!br_read_bits(br, 9, &tmp)) {
            snprintf(err, err_cap, "truncated cb_offset");
            return false;
        }
    }
    if (num_cr_points) {
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated cr_mult");
            return false;
        }
        if (!br_read_bits(br, 8, &tmp)) {
            snprintf(err, err_cap, "truncated cr_luma_mult");
            return false;
        }
        if (!br_read_bits(br, 9, &tmp)) {
            snprintf(err, err_cap, "truncated cr_offset");
            return false;
        }
    }

    (void)chroma_scaling_from_luma;
    (void)num_cb_points;
    (void)num_cr_points;
    return true;
}

static bool skip_uncompressed_header_after_tile_info(BitReader *br,
                                                     const SeqHdr *seq,
                                                     FrameHdr *fh,
                                                     char *err,
                                                     size_t err_cap) {
    QuantizationState qs;
    SegmentationState ss;
    if (!parse_quantization_params_skip(br, seq, &qs, err, err_cap)) {
        return false;
    }
    if (!parse_segmentation_params_skip(br, &ss, err, err_cap)) {
        return false;
    }
    if (!parse_delta_q_params_skip(br, &qs, err, err_cap)) {
        return false;
    }
    if (!parse_delta_lf_params_skip(br, &qs, fh->allow_intrabc, err, err_cap)) {
        return false;
    }

    uint32_t CodedLossless = compute_coded_lossless(&qs, &ss);
    uint32_t AllLossless = CodedLossless && (fh->frame_width == fh->upscaled_width);

    if (!parse_loop_filter_params_skip(br, CodedLossless, fh->allow_intrabc, seq->num_planes, err, err_cap)) {
        return false;
    }
    if (!parse_cdef_params_skip(br, CodedLossless, fh->allow_intrabc, seq->enable_cdef, seq->num_planes, err, err_cap)) {
        return false;
    }
    if (!parse_lr_params_skip(br,
                              AllLossless,
                              fh->allow_intrabc,
                              seq->enable_restoration,
                              seq->num_planes,
                              seq->use_128x128_superblock,
                              seq->subsampling_x,
                              seq->subsampling_y,
                              err,
                              err_cap)) {
        return false;
    }
    if (!parse_read_tx_mode(br, CodedLossless, &fh->tx_mode, err, err_cap)) {
        return false;
    }

    // frame_reference_mode(): FrameIsIntra => no bits.
    // skip_mode_params(): FrameIsIntra => no bits.

    // allow_warped_motion is derived to 0 for FrameIsIntra.

    // reduced_tx_set
    uint32_t tmp;
    if (!br_read_bit(br, &tmp)) {
        snprintf(err, err_cap, "truncated reduced_tx_set");
        return false;
    }
    fh->reduced_tx_set = tmp;

    // global_motion_params(): FrameIsIntra => no bits.

    if (!parse_film_grain_params_skip(br, seq, fh->frame_type, fh->show_frame, err, err_cap)) {
        return false;
    }

    // frame_obu() byte_alignment() between frame_header_obu() and tile_group_obu().
    if (!br_byte_align_zero(br, err, err_cap)) {
        return false;
    }

    return true;
}

enum {
    MAX_TILE_COLS = 64,
    MAX_TILE_ROWS = 64,
    MAX_TILE_WIDTH = 4096,
    MAX_TILE_AREA = 4096 * 2304,
};

typedef struct {
    uint32_t tile_cols;
    uint32_t tile_rows;
    uint32_t tile_cols_log2;
    uint32_t tile_rows_log2;
    uint32_t tile_size_bytes; // 1..4 when multiple tiles; 0 when single-tile
    uint32_t context_update_tile_id;

    // For later steps, and for mapping TileNum -> MI ranges.
    uint32_t mi_col_starts[MAX_TILE_COLS + 1];
    uint32_t mi_row_starts[MAX_TILE_ROWS + 1];
} TileInfo;

static bool write_text_file_frame_info(const char *dir,
                                      const SeqHdr *seq,
                                      const FrameHdr *fh,
                                      const TileInfo *ti,
                                      char *err,
                                      size_t err_cap) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_info.txt", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_cap, "failed to open %s: %s", path, strerror(errno));
        return false;
    }

    fprintf(f, "still_picture=%u\n", (unsigned)seq->still_picture);
    fprintf(f, "reduced_still_picture_header=%u\n", (unsigned)seq->reduced_still_picture_header);
    fprintf(f, "mono_chrome=%u\n", (unsigned)seq->mono_chrome);
    fprintf(f, "num_planes=%u\n", (unsigned)seq->num_planes);
    fprintf(f, "subsampling_x=%u\n", (unsigned)seq->subsampling_x);
    fprintf(f, "subsampling_y=%u\n", (unsigned)seq->subsampling_y);

    fprintf(f, "frame_width=%u\n", (unsigned)fh->frame_width);
    fprintf(f, "frame_height=%u\n", (unsigned)fh->frame_height);
    fprintf(f, "coded_width=%u\n", (unsigned)fh->coded_width);
    fprintf(f, "coded_height=%u\n", (unsigned)fh->coded_height);
    fprintf(f, "upscaled_width=%u\n", (unsigned)fh->upscaled_width);

    fprintf(f, "tile_cols=%u\n", (unsigned)ti->tile_cols);
    fprintf(f, "tile_rows=%u\n", (unsigned)ti->tile_rows);
    fprintf(f, "tile_size_bytes=%u\n", (unsigned)ti->tile_size_bytes);

    fclose(f);
    return true;
}

static uint32_t u32_min(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static uint32_t u32_max(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static uint32_t tile_log2_u32(uint32_t blkSize, uint32_t target) {
    uint32_t k = 0;
    while (((uint64_t)blkSize << k) < target) {
        k++;
        if (k > 31) {
            break;
        }
    }
    return k;
}

static bool br_byte_align_zero(BitReader *br, char *err, size_t err_cap) {
    while ((br->bitpos & 7u) != 0) {
        uint32_t b;
        if (!br_read_bit(br, &b)) {
            snprintf(err, err_cap, "truncated byte_alignment");
            return false;
        }
        if (b != 0) {
            snprintf(err, err_cap, "nonzero alignment bit");
            return false;
        }
    }
    return true;
}

static bool parse_tile_info(BitReader *br,
                            const SeqHdr *seq,
                            const FrameHdr *fh,
                            TileInfo *out,
                            char *err,
                            size_t err_cap) {
    memset(out, 0, sizeof(*out));

    // tile_info(): computed values
    const uint32_t MiCols = fh->mi_cols;
    const uint32_t MiRows = fh->mi_rows;

    uint32_t sbCols = seq->use_128x128_superblock ? ((MiCols + 31u) >> 5) : ((MiCols + 15u) >> 4);
    uint32_t sbRows = seq->use_128x128_superblock ? ((MiRows + 31u) >> 5) : ((MiRows + 15u) >> 4);
    uint32_t sbShift = seq->use_128x128_superblock ? 5u : 4u;
    uint32_t sbSize = sbShift + 2u;

    if (sbSize >= 31) {
        snprintf(err, err_cap, "unsupported: sbSize too large");
        return false;
    }

    uint32_t maxTileWidthSb = (uint32_t)(MAX_TILE_WIDTH >> sbSize);
    uint32_t maxTileAreaSb = (uint32_t)(MAX_TILE_AREA >> (2u * sbSize));

    uint32_t minLog2TileCols = tile_log2_u32(maxTileWidthSb, sbCols);
    uint32_t maxLog2TileCols = tile_log2_u32(1u, u32_min(sbCols, MAX_TILE_COLS));
    uint32_t maxLog2TileRows = tile_log2_u32(1u, u32_min(sbRows, MAX_TILE_ROWS));
    uint32_t minLog2Tiles = u32_max(minLog2TileCols, tile_log2_u32(maxTileAreaSb, sbRows * sbCols));

    uint32_t uniform_tile_spacing_flag;
    if (!br_read_bit(br, &uniform_tile_spacing_flag)) {
        snprintf(err, err_cap, "truncated uniform_tile_spacing_flag");
        return false;
    }

    uint32_t TileColsLog2 = 0;
    uint32_t TileRowsLog2 = 0;
    uint32_t TileCols = 0;
    uint32_t TileRows = 0;

    if (uniform_tile_spacing_flag) {
        TileColsLog2 = minLog2TileCols;
        while (TileColsLog2 < maxLog2TileCols) {
            uint32_t inc;
            if (!br_read_bit(br, &inc)) {
                snprintf(err, err_cap, "truncated increment_tile_cols_log2");
                return false;
            }
            if (inc == 1) {
                TileColsLog2++;
            } else {
                break;
            }
        }

        uint32_t tileWidthSb = (sbCols + (1u << TileColsLog2) - 1u) >> TileColsLog2;
        uint32_t i = 0;
        for (uint32_t startSb = 0; startSb < sbCols; startSb += tileWidthSb) {
            if (i >= MAX_TILE_COLS) {
                snprintf(err, err_cap, "tile_cols exceeds MAX_TILE_COLS");
                return false;
            }
            out->mi_col_starts[i] = startSb << sbShift;
            i += 1;
        }
        out->mi_col_starts[i] = MiCols;
        TileCols = i;

        uint32_t minLog2TileRows = u32_max((minLog2Tiles > TileColsLog2) ? (minLog2Tiles - TileColsLog2) : 0u, 0u);
        TileRowsLog2 = minLog2TileRows;
        while (TileRowsLog2 < maxLog2TileRows) {
            uint32_t inc;
            if (!br_read_bit(br, &inc)) {
                snprintf(err, err_cap, "truncated increment_tile_rows_log2");
                return false;
            }
            if (inc == 1) {
                TileRowsLog2++;
            } else {
                break;
            }
        }

        uint32_t tileHeightSb = (sbRows + (1u << TileRowsLog2) - 1u) >> TileRowsLog2;
        uint32_t irow = 0;
        for (uint32_t startSb = 0; startSb < sbRows; startSb += tileHeightSb) {
            if (irow >= MAX_TILE_ROWS) {
                snprintf(err, err_cap, "tile_rows exceeds MAX_TILE_ROWS");
                return false;
            }
            out->mi_row_starts[irow] = startSb << sbShift;
            irow += 1;
        }
        out->mi_row_starts[irow] = MiRows;
        TileRows = irow;
    } else {
        uint32_t widestTileSb = 0;
        uint32_t startSb = 0;
        uint32_t i = 0;
        while (startSb < sbCols) {
            if (i >= MAX_TILE_COLS) {
                snprintf(err, err_cap, "tile_cols exceeds MAX_TILE_COLS");
                return false;
            }
            out->mi_col_starts[i] = startSb << sbShift;

            uint32_t maxWidth = u32_min(sbCols - startSb, maxTileWidthSb);
            uint32_t width_in_sbs_minus_1;
            if (!br_read_ns(br, maxWidth, &width_in_sbs_minus_1)) {
                snprintf(err, err_cap, "truncated width_in_sbs_minus_1");
                return false;
            }
            uint32_t sizeSb = width_in_sbs_minus_1 + 1;
            widestTileSb = u32_max(sizeSb, widestTileSb);
            startSb += sizeSb;
            i++;
        }
        out->mi_col_starts[i] = MiCols;
        TileCols = i;
        TileColsLog2 = tile_log2_u32(1u, TileCols);

        if (minLog2Tiles > 0) {
            maxTileAreaSb = (sbRows * sbCols) >> (minLog2Tiles + 1);
        } else {
            maxTileAreaSb = sbRows * sbCols;
        }
        uint32_t maxTileHeightSb = u32_max((widestTileSb == 0) ? 1u : (maxTileAreaSb / widestTileSb), 1u);

        uint32_t startSbRow = 0;
        uint32_t irow = 0;
        while (startSbRow < sbRows) {
            if (irow >= MAX_TILE_ROWS) {
                snprintf(err, err_cap, "tile_rows exceeds MAX_TILE_ROWS");
                return false;
            }
            out->mi_row_starts[irow] = startSbRow << sbShift;
            uint32_t maxHeight = u32_min(sbRows - startSbRow, maxTileHeightSb);
            uint32_t height_in_sbs_minus_1;
            if (!br_read_ns(br, maxHeight, &height_in_sbs_minus_1)) {
                snprintf(err, err_cap, "truncated height_in_sbs_minus_1");
                return false;
            }
            uint32_t sizeSb = height_in_sbs_minus_1 + 1;
            startSbRow += sizeSb;
            irow++;
        }
        out->mi_row_starts[irow] = MiRows;
        TileRows = irow;
        TileRowsLog2 = tile_log2_u32(1u, TileRows);
    }

    out->tile_cols = TileCols;
    out->tile_rows = TileRows;
    out->tile_cols_log2 = TileColsLog2;
    out->tile_rows_log2 = TileRowsLog2;

    if (TileColsLog2 > 0 || TileRowsLog2 > 0) {
        uint32_t bits = TileColsLog2 + TileRowsLog2;
        uint32_t context_update_tile_id = 0;
        if (bits > 0) {
            if (!br_read_bits(br, bits, &context_update_tile_id)) {
                snprintf(err, err_cap, "truncated context_update_tile_id");
                return false;
            }
        }
        uint32_t tile_size_bytes_minus_1;
        if (!br_read_bits(br, 2, &tile_size_bytes_minus_1)) {
            snprintf(err, err_cap, "truncated tile_size_bytes_minus_1");
            return false;
        }
        out->tile_size_bytes = tile_size_bytes_minus_1 + 1;
        out->context_update_tile_id = context_update_tile_id;

        if (out->context_update_tile_id >= (out->tile_cols * out->tile_rows)) {
            snprintf(err, err_cap, "invalid context_update_tile_id");
            return false;
        }
    } else {
        out->tile_size_bytes = 0;
        out->context_update_tile_id = 0;
    }

    return true;
}

static bool parse_tile_group_obu_and_print(const uint8_t *payload,
                                           size_t payload_len,
                                           uint64_t abs_payload_off,
                                           const SeqHdr *seq,
                                           const FrameHdr *fh,
                                           const TileInfo *ti,
                                           TileDumpCtx *dump,
                                           bool check_trailing,
                                           bool check_trailing_strict,
                                           uint32_t consume_bools,
                                           bool check_trailingbits,
                                           bool check_trailingbits_strict,
                                           bool decode_tile_syntax,
                                           bool decode_tile_syntax_strict,
                                           char *err,
                                           size_t err_cap) {
    uint32_t NumTiles = ti->tile_cols * ti->tile_rows;
    if (NumTiles == 0) {
        snprintf(err, err_cap, "invalid NumTiles=0");
        return false;
    }
    if (NumTiles > (MAX_TILE_COLS * MAX_TILE_ROWS)) {
        snprintf(err, err_cap, "NumTiles exceeds limits");
        return false;
    }

    BitReader br = (BitReader){payload, payload_len, 0};
    uint64_t startBitPos = br.bitpos;

    uint32_t tile_start_and_end_present_flag = 0;
    if (NumTiles > 1) {
        if (!br_read_bit(&br, &tile_start_and_end_present_flag)) {
            snprintf(err, err_cap, "truncated tile_start_and_end_present_flag");
            return false;
        }
    }

    uint32_t tg_start = 0;
    uint32_t tg_end = NumTiles - 1;
    if (NumTiles > 1 && tile_start_and_end_present_flag) {
        uint32_t tileBits = ti->tile_cols_log2 + ti->tile_rows_log2;
        if (tileBits == 0 || tileBits > 31) {
            snprintf(err, err_cap, "invalid tileBits");
            return false;
        }
        if (!br_read_bits(&br, tileBits, &tg_start) || !br_read_bits(&br, tileBits, &tg_end)) {
            snprintf(err, err_cap, "truncated tg_start/tg_end");
            return false;
        }
    }
    if (tg_start > tg_end || tg_end >= NumTiles) {
        snprintf(err, err_cap, "invalid tg_start/tg_end");
        return false;
    }

    if (!br_byte_align_zero(&br, err, err_cap)) {
        return false;
    }

    uint64_t endBitPos = br.bitpos;
    uint64_t headerBytes = (endBitPos - startBitPos) / 8u;
    if (headerBytes > payload_len) {
        snprintf(err, err_cap, "tile_group header exceeds payload");
        return false;
    }

    size_t cur = (size_t)headerBytes;
    size_t remaining = payload_len - (size_t)headerBytes;

    printf("  tile_group: tg_start=%u tg_end=%u headerBytes=%" PRIu64 " payloadBytes=%zu\n",
           tg_start,
           tg_end,
           headerBytes,
           payload_len);

    for (uint32_t TileNum = tg_start; TileNum <= tg_end; TileNum++) {
        uint32_t tileRow = TileNum / ti->tile_cols;
        uint32_t tileCol = TileNum % ti->tile_cols;
        bool lastTile = TileNum == tg_end;

        uint64_t tile_data_off_abs = abs_payload_off + (uint64_t)cur;
        uint64_t tileSize = 0;

        if (lastTile) {
            if (cur > payload_len) {
                snprintf(err, err_cap, "internal error: cur exceeds payload_len");
                return false;
            }
            tileSize = (uint64_t)(payload_len - cur);

            if (dump && dump->dir) {
                char out_path[1024];
                snprintf(out_path,
                         sizeof(out_path),
                         "%s/tg%u_tile%u_r%u_c%u.bin",
                         dump->dir,
                         dump->tg_index,
                         TileNum,
                         tileRow,
                         tileCol);
                if (!write_bytes_file(out_path, payload + cur, (size_t)tileSize, err, err_cap)) {
                    return false;
                }
                dump->tiles_written++;
            }

            if (check_trailing) {
                Av1SymbolDecoder sd;
                if (!av1_symbol_init(&sd, payload + cur, (size_t)tileSize, true, err, err_cap)) {
                    return false;
                }

                for (uint32_t i = 0; i < consume_bools; i++) {
                    uint32_t b;
                    if (!av1_symbol_read_bool(&sd, &b, err, err_cap)) {
                        if (check_trailing_strict) {
                            return false;
                        }
                        printf("    tile[%u] r%u c%u: read_bool(%u/%u) FAILED: %s\n",
                               TileNum,
                               tileRow,
                               tileCol,
                               i + 1,
                               consume_bools,
                               err[0] ? err : "(unknown)");
                        err[0] = 0;
                        break;
                    }
                }

                if (err[0] == 0 && !av1_symbol_exit(&sd, err, err_cap)) {
                    if (check_trailing_strict) {
                        return false;
                    }
                    printf("    tile[%u] r%u c%u: exit_symbol probe FAILED: %s\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           err[0] ? err : "(unknown)");
                    err[0] = 0;
                }
            }

            if (check_trailingbits) {
                if (!av1_symbol_check_trailing_bits(payload + cur, (size_t)tileSize, err, err_cap)) {
                    if (check_trailingbits_strict) {
                        return false;
                    }
                    printf("    tile[%u] r%u c%u: trailing-bits check FAILED: %s\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           err[0] ? err : "(unknown)");
                    err[0] = 0;
                }
            }

            if (decode_tile_syntax) {
                Av1TileDecodeParams p;
                memset(&p, 0, sizeof(p));
                p.mi_col_start = ti->mi_col_starts[tileCol];
                p.mi_col_end = ti->mi_col_starts[tileCol + 1];
                p.mi_row_start = ti->mi_row_starts[tileRow];
                p.mi_row_end = ti->mi_row_starts[tileRow + 1];
                p.use_128x128_superblock = seq->use_128x128_superblock;
                p.mono_chrome = seq->mono_chrome;
                p.subsampling_x = seq->subsampling_x;
                p.subsampling_y = seq->subsampling_y;
                p.coded_lossless = fh->coded_lossless;
                p.enable_filter_intra = seq->enable_filter_intra;
                p.allow_screen_content_tools = fh->allow_screen_content_tools;
                p.disable_cdf_update = fh->disable_cdf_update;
                p.base_q_idx = fh->base_q_idx;
                p.tx_mode = fh->tx_mode;
                p.reduced_tx_set = fh->reduced_tx_set;
                Av1TileSyntaxProbeStats st;
                Av1TileSyntaxProbeStatus s = av1_tile_syntax_probe(payload + cur,
                                                                   (size_t)tileSize,
                                                                   &p,
                                                                   consume_bools,
                                                                   &st,
                                                                   err,
                                                                   err_cap);
                if (s == AV1_TILE_SYNTAX_PROBE_OK) {
                    printf("    tile[%u] r%u c%u: decode-tile-syntax OK (bools=%u)\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           st.bools_read);
                } else {
                    const char *label = s == AV1_TILE_SYNTAX_PROBE_ERROR ? "ERROR" : "UNSUPPORTED";
                    if (decode_tile_syntax_strict) {
                        return false;
                    }
                    printf(
                        "    tile[%u] r%u c%u: decode-tile-syntax %s: %s (bools=%u/%u, tile_mi=%ux%u, sb=%ux%u, root_part=%s%u, part_syms=%u forced_splits=%u leafs=%u blocks_decoded=%u, block0_skip=%s%u ctx=%u, block0_y_mode=%s%u ctx=%u, block0_uv_mode=%s%u, palY=%s%u size=%s%u, palUV=%s%u size=%s%u, tx_mode=%u tx_depth=%s%u tx_size=%s tx_type=%s%u, txb_skip=%s%u block0_txb_skip_ctx=%s%u, block0_tx_blocks=%u, block0_tx1_txb_skip=%s%u block0_tx1_txb_skip_ctx=%s%u block0_tx1_xy=(%u,%u), block1_txb_skip=%s%u block1_txb_skip_ctx=%s%u block1_xy=(%u,%u), block1_eob_pt=%s%u block1_eob_pt_ctx=%s%u, block1_eob=%s%u, block1_coeff_base_eob=%s%u block1_coeff_base_eob_ctx=%s%u, block1_coeff_base=%s%u block1_coeff_base_ctx=%s%u, eob_pt=%s%u, eob=%s%u, coeff_base_eob=%s%u ctx=%u, coeff_base=%s%u ctx=%u, coeff_br=%s%u ctx=%u, dc_sign=%s%u ctx=%u, cfl=%s(signs=%u u=%d v=%d), filt=%s%u mode=%s%u, angle_y=%s%d, angle_uv=%s%d @(%u,%u) %ux%u)\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           label,
                           err[0] ? err : "(unknown)",
                           st.bools_read,
                           st.bools_requested,
                           st.tile_mi_cols,
                           st.tile_mi_rows,
                           st.sb_cols,
                           st.sb_rows,
                           st.partition_decoded ? (st.partition_forced ? "forced " : "") : "n/a ",
                           st.partition_decoded ? st.partition_symbol : 0u,
                           st.partition_symbols_read,
                           st.partition_forced_splits,
                           st.leaf_blocks,
                           st.blocks_decoded,
                           st.block0_skip_decoded ? "" : "n/a ",
                           st.block0_skip_decoded ? st.block0_skip : 0u,
                           st.block0_skip_decoded ? st.block0_skip_ctx : 0u,
                           st.block0_y_mode_decoded ? "" : "n/a ",
                           st.block0_y_mode_decoded ? st.block0_y_mode : 0u,
                           st.block0_y_mode_decoded ? st.block0_y_mode_ctx : 0u,
                           st.block0_uv_mode_decoded ? "" : "n/a ",
                           st.block0_uv_mode_decoded ? st.block0_uv_mode : 0u,
                           st.block0_has_palette_y_decoded ? "" : "n/a ",
                           st.block0_has_palette_y_decoded ? st.block0_has_palette_y : 0u,
                           st.block0_palette_size_y_decoded ? "" : "n/a ",
                           st.block0_palette_size_y_decoded ? st.block0_palette_size_y : 0u,
                           st.block0_has_palette_uv_decoded ? "" : "n/a ",
                           st.block0_has_palette_uv_decoded ? st.block0_has_palette_uv : 0u,
                           st.block0_palette_size_uv_decoded ? "" : "n/a ",
                           st.block0_palette_size_uv_decoded ? st.block0_palette_size_uv : 0u,
                           st.block0_tx_mode,
                           st.block0_tx_depth_decoded ? "" : "n/a ",
                           st.block0_tx_depth_decoded ? st.block0_tx_depth : 0u,
                           st.block0_tx_size_decoded ? tx_size_name(st.block0_tx_size) : "n/a",
                           st.block0_tx_type_decoded ? "" : "n/a ",
                           st.block0_tx_type_decoded ? st.block0_tx_type : 0u,
                           st.block0_txb_skip_decoded ? "" : "n/a ",
                           st.block0_txb_skip_decoded ? st.block0_txb_skip : 0u,
                           st.block0_txb_skip_decoded ? "" : "n/a ",
                           st.block0_txb_skip_decoded ? st.block0_txb_skip_ctx : 0u,
                           st.block0_tx_blocks_decoded,
                           st.block0_tx1_txb_skip_decoded ? "" : "n/a ",
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_txb_skip : 0u,
                           st.block0_tx1_txb_skip_decoded ? "" : "n/a ",
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_txb_skip_ctx : 0u,
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_y4 : 0u,
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_x4 : 0u,
                           st.block1_txb_skip_decoded ? "" : "n/a ",
                           st.block1_txb_skip_decoded ? st.block1_txb_skip : 0u,
                           st.block1_txb_skip_decoded ? "" : "n/a ",
                           st.block1_txb_skip_decoded ? st.block1_txb_skip_ctx : 0u,
                           st.block1_txb_skip_decoded ? st.block1_r_mi : 0u,
                           st.block1_txb_skip_decoded ? st.block1_c_mi : 0u,
                           st.block1_eob_pt_decoded ? "" : "n/a ",
                           st.block1_eob_pt_decoded ? st.block1_eob_pt : 0u,
                           st.block1_eob_pt_decoded ? "" : "n/a ",
                           st.block1_eob_pt_decoded ? st.block1_eob_pt_ctx : 0u,
                           st.block1_eob_decoded ? "" : "n/a ",
                           st.block1_eob_decoded ? st.block1_eob : 0u,
                           st.block1_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block1_coeff_base_eob_decoded ? st.block1_coeff_base_eob_level : 0u,
                           st.block1_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block1_coeff_base_eob_decoded ? st.block1_coeff_base_eob_ctx : 0u,
                           st.block1_coeff_base_decoded ? "" : "n/a ",
                           st.block1_coeff_base_decoded ? st.block1_coeff_base_level : 0u,
                           st.block1_coeff_base_decoded ? "" : "n/a ",
                           st.block1_coeff_base_decoded ? st.block1_coeff_base_ctx : 0u,
                           st.block0_eob_pt_decoded ? "" : "n/a ",
                           st.block0_eob_pt_decoded ? st.block0_eob_pt : 0u,
                           st.block0_eob_decoded ? "" : "n/a ",
                           st.block0_eob_decoded ? st.block0_eob : 0u,
                           st.block0_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block0_coeff_base_eob_decoded ? st.block0_coeff_base_eob_level : 0u,
                           st.block0_coeff_base_eob_decoded ? st.block0_coeff_base_eob_ctx : 0u,
                           st.block0_coeff_base_decoded ? "" : "n/a ",
                           st.block0_coeff_base_decoded ? st.block0_coeff_base_level : 0u,
                           st.block0_coeff_base_decoded ? st.block0_coeff_base_ctx : 0u,
                           st.block0_coeff_br_decoded ? "" : "n/a ",
                           st.block0_coeff_br_decoded ? st.block0_coeff_br_sym : 0u,
                           st.block0_coeff_br_decoded ? st.block0_coeff_br_ctx : 0u,
                           st.block0_dc_sign_decoded ? "" : "n/a ",
                           st.block0_dc_sign_decoded ? st.block0_dc_sign : 0u,
                           st.block0_dc_sign_decoded ? st.block0_dc_sign_ctx : 0u,
                           st.block0_cfl_alphas_decoded ? "" : "n/a ",
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_signs : 0u,
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_u : 0,
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_v : 0,
                           st.block0_use_filter_intra_decoded ? "" : "n/a ",
                           st.block0_use_filter_intra_decoded ? st.block0_use_filter_intra : 0u,
                           st.block0_filter_intra_mode_decoded ? "" : "n/a ",
                           st.block0_filter_intra_mode_decoded ? st.block0_filter_intra_mode : 0u,
                           st.block0_angle_delta_y_decoded ? "" : "n/a ",
                           st.block0_angle_delta_y_decoded ? st.block0_angle_delta_y : 0,
                           st.block0_angle_delta_uv_decoded ? "" : "n/a ",
                           st.block0_angle_delta_uv_decoded ? st.block0_angle_delta_uv : 0,
                           st.block0_skip_decoded ? st.block0_r_mi : 0u,
                           st.block0_skip_decoded ? st.block0_c_mi : 0u,
                           st.block0_skip_decoded ? (1u << st.block0_wlog2) : 0u,
                           st.block0_skip_decoded ? (1u << st.block0_hlog2) : 0u);
                    err[0] = 0;
                }
            }
            printf("    tile[%u] r%u c%u: off=%" PRIu64 " size=%" PRIu64 "\n",
                   TileNum,
                   tileRow,
                   tileCol,
                   tile_data_off_abs,
                   tileSize);
            cur += (size_t)tileSize;
            remaining = 0;
        } else {
            uint32_t TileSizeBytes = ti->tile_size_bytes;
            if (TileSizeBytes == 0 || TileSizeBytes > 4) {
                snprintf(err, err_cap, "invalid TileSizeBytes");
                return false;
            }
            if (cur + TileSizeBytes > payload_len) {
                snprintf(err, err_cap, "truncated tile_size_minus_1");
                return false;
            }
            uint32_t tile_size_minus_1 = 0;
            for (uint32_t i = 0; i < TileSizeBytes; i++) {
                tile_size_minus_1 |= (uint32_t)payload[cur + i] << (8u * i);
            }
            tileSize = (uint64_t)tile_size_minus_1 + 1u;
            cur += TileSizeBytes;

            if (tileSize > (uint64_t)(payload_len - cur)) {
                snprintf(err, err_cap, "tileSize exceeds remaining tile_group payload");
                return false;
            }
            tile_data_off_abs = abs_payload_off + (uint64_t)cur;

            if (dump && dump->dir) {
                char out_path[1024];
                snprintf(out_path,
                         sizeof(out_path),
                         "%s/tg%u_tile%u_r%u_c%u.bin",
                         dump->dir,
                         dump->tg_index,
                         TileNum,
                         tileRow,
                         tileCol);
                if (!write_bytes_file(out_path, payload + cur, (size_t)tileSize, err, err_cap)) {
                    return false;
                }
                dump->tiles_written++;
            }

            if (check_trailing) {
                Av1SymbolDecoder sd;
                if (!av1_symbol_init(&sd, payload + cur, (size_t)tileSize, true, err, err_cap)) {
                    return false;
                }

                for (uint32_t i = 0; i < consume_bools; i++) {
                    uint32_t b;
                    if (!av1_symbol_read_bool(&sd, &b, err, err_cap)) {
                        if (check_trailing_strict) {
                            return false;
                        }
                        printf("    tile[%u] r%u c%u: read_bool(%u/%u) FAILED: %s\n",
                               TileNum,
                               tileRow,
                               tileCol,
                               i + 1,
                               consume_bools,
                               err[0] ? err : "(unknown)");
                        err[0] = 0;
                        break;
                    }
                }

                if (err[0] == 0 && !av1_symbol_exit(&sd, err, err_cap)) {
                    if (check_trailing_strict) {
                        return false;
                    }
                    printf("    tile[%u] r%u c%u: exit_symbol probe FAILED: %s\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           err[0] ? err : "(unknown)");
                    err[0] = 0;
                }
            }

            if (check_trailingbits) {
                if (!av1_symbol_check_trailing_bits(payload + cur, (size_t)tileSize, err, err_cap)) {
                    if (check_trailingbits_strict) {
                        return false;
                    }
                    printf("    tile[%u] r%u c%u: trailing-bits check FAILED: %s\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           err[0] ? err : "(unknown)");
                    err[0] = 0;
                }
            }

            if (decode_tile_syntax) {
                Av1TileDecodeParams p;
                memset(&p, 0, sizeof(p));
                p.mi_col_start = ti->mi_col_starts[tileCol];
                p.mi_col_end = ti->mi_col_starts[tileCol + 1];
                p.mi_row_start = ti->mi_row_starts[tileRow];
                p.mi_row_end = ti->mi_row_starts[tileRow + 1];
                p.use_128x128_superblock = seq->use_128x128_superblock;
                p.mono_chrome = seq->mono_chrome;
                p.subsampling_x = seq->subsampling_x;
                p.subsampling_y = seq->subsampling_y;
                p.coded_lossless = fh->coded_lossless;
                p.enable_filter_intra = seq->enable_filter_intra;
                p.allow_screen_content_tools = fh->allow_screen_content_tools;
                p.disable_cdf_update = fh->disable_cdf_update;
                p.base_q_idx = fh->base_q_idx;
                p.tx_mode = fh->tx_mode;
                p.reduced_tx_set = fh->reduced_tx_set;
                Av1TileSyntaxProbeStats st;
                Av1TileSyntaxProbeStatus s = av1_tile_syntax_probe(payload + cur,
                                                                   (size_t)tileSize,
                                                                   &p,
                                                                   consume_bools,
                                                                   &st,
                                                                   err,
                                                                   err_cap);
                if (s == AV1_TILE_SYNTAX_PROBE_OK) {
                    printf("    tile[%u] r%u c%u: decode-tile-syntax OK (bools=%u)\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           st.bools_read);
                } else {
                    const char *label = s == AV1_TILE_SYNTAX_PROBE_ERROR ? "ERROR" : "UNSUPPORTED";
                    if (decode_tile_syntax_strict) {
                        return false;
                    }
                    printf(
                        "    tile[%u] r%u c%u: decode-tile-syntax %s: %s (bools=%u/%u, tile_mi=%ux%u, sb=%ux%u, root_part=%s%u, part_syms=%u forced_splits=%u leafs=%u blocks_decoded=%u, block0_skip=%s%u ctx=%u, block0_y_mode=%s%u ctx=%u, block0_uv_mode=%s%u, palY=%s%u size=%s%u, palUV=%s%u size=%s%u, tx_mode=%u tx_depth=%s%u tx_size=%s tx_type=%s%u, txb_skip=%s%u block0_txb_skip_ctx=%s%u, block0_tx_blocks=%u, block0_tx1_txb_skip=%s%u block0_tx1_txb_skip_ctx=%s%u block0_tx1_xy=(%u,%u), block1_txb_skip=%s%u block1_txb_skip_ctx=%s%u block1_xy=(%u,%u), block1_eob_pt=%s%u block1_eob_pt_ctx=%s%u, block1_eob=%s%u, block1_coeff_base_eob=%s%u block1_coeff_base_eob_ctx=%s%u, block1_coeff_base=%s%u block1_coeff_base_ctx=%s%u, eob_pt=%s%u, eob=%s%u, coeff_base_eob=%s%u ctx=%u, coeff_base=%s%u ctx=%u, coeff_br=%s%u ctx=%u, dc_sign=%s%u ctx=%u, cfl=%s(signs=%u u=%d v=%d), filt=%s%u mode=%s%u, angle_y=%s%d, angle_uv=%s%d @(%u,%u) %ux%u)\n",
                           TileNum,
                           tileRow,
                           tileCol,
                           label,
                           err[0] ? err : "(unknown)",
                           st.bools_read,
                           st.bools_requested,
                           st.tile_mi_cols,
                           st.tile_mi_rows,
                           st.sb_cols,
                           st.sb_rows,
                           st.partition_decoded ? (st.partition_forced ? "forced " : "") : "n/a ",
                           st.partition_decoded ? st.partition_symbol : 0u,
                           st.partition_symbols_read,
                           st.partition_forced_splits,
                           st.leaf_blocks,
                           st.blocks_decoded,
                           st.block0_skip_decoded ? "" : "n/a ",
                           st.block0_skip_decoded ? st.block0_skip : 0u,
                           st.block0_skip_decoded ? st.block0_skip_ctx : 0u,
                           st.block0_y_mode_decoded ? "" : "n/a ",
                           st.block0_y_mode_decoded ? st.block0_y_mode : 0u,
                           st.block0_y_mode_decoded ? st.block0_y_mode_ctx : 0u,
                           st.block0_uv_mode_decoded ? "" : "n/a ",
                           st.block0_uv_mode_decoded ? st.block0_uv_mode : 0u,
                           st.block0_has_palette_y_decoded ? "" : "n/a ",
                           st.block0_has_palette_y_decoded ? st.block0_has_palette_y : 0u,
                           st.block0_palette_size_y_decoded ? "" : "n/a ",
                           st.block0_palette_size_y_decoded ? st.block0_palette_size_y : 0u,
                           st.block0_has_palette_uv_decoded ? "" : "n/a ",
                           st.block0_has_palette_uv_decoded ? st.block0_has_palette_uv : 0u,
                           st.block0_palette_size_uv_decoded ? "" : "n/a ",
                           st.block0_palette_size_uv_decoded ? st.block0_palette_size_uv : 0u,
                           st.block0_tx_mode,
                           st.block0_tx_depth_decoded ? "" : "n/a ",
                           st.block0_tx_depth_decoded ? st.block0_tx_depth : 0u,
                           st.block0_tx_size_decoded ? tx_size_name(st.block0_tx_size) : "n/a",
                           st.block0_tx_type_decoded ? "" : "n/a ",
                           st.block0_tx_type_decoded ? st.block0_tx_type : 0u,
                           st.block0_txb_skip_decoded ? "" : "n/a ",
                           st.block0_txb_skip_decoded ? st.block0_txb_skip : 0u,
                           st.block0_txb_skip_decoded ? "" : "n/a ",
                           st.block0_txb_skip_decoded ? st.block0_txb_skip_ctx : 0u,
                           st.block0_tx_blocks_decoded,
                           st.block0_tx1_txb_skip_decoded ? "" : "n/a ",
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_txb_skip : 0u,
                           st.block0_tx1_txb_skip_decoded ? "" : "n/a ",
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_txb_skip_ctx : 0u,
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_y4 : 0u,
                           st.block0_tx1_txb_skip_decoded ? st.block0_tx1_x4 : 0u,
                           st.block1_txb_skip_decoded ? "" : "n/a ",
                           st.block1_txb_skip_decoded ? st.block1_txb_skip : 0u,
                           st.block1_txb_skip_decoded ? "" : "n/a ",
                           st.block1_txb_skip_decoded ? st.block1_txb_skip_ctx : 0u,
                           st.block1_txb_skip_decoded ? st.block1_r_mi : 0u,
                           st.block1_txb_skip_decoded ? st.block1_c_mi : 0u,
                           st.block1_eob_pt_decoded ? "" : "n/a ",
                           st.block1_eob_pt_decoded ? st.block1_eob_pt : 0u,
                           st.block1_eob_pt_decoded ? "" : "n/a ",
                           st.block1_eob_pt_decoded ? st.block1_eob_pt_ctx : 0u,
                           st.block1_eob_decoded ? "" : "n/a ",
                           st.block1_eob_decoded ? st.block1_eob : 0u,
                           st.block1_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block1_coeff_base_eob_decoded ? st.block1_coeff_base_eob_level : 0u,
                           st.block1_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block1_coeff_base_eob_decoded ? st.block1_coeff_base_eob_ctx : 0u,
                           st.block1_coeff_base_decoded ? "" : "n/a ",
                           st.block1_coeff_base_decoded ? st.block1_coeff_base_level : 0u,
                           st.block1_coeff_base_decoded ? "" : "n/a ",
                           st.block1_coeff_base_decoded ? st.block1_coeff_base_ctx : 0u,
                           st.block0_eob_pt_decoded ? "" : "n/a ",
                           st.block0_eob_pt_decoded ? st.block0_eob_pt : 0u,
                           st.block0_eob_decoded ? "" : "n/a ",
                           st.block0_eob_decoded ? st.block0_eob : 0u,
                           st.block0_coeff_base_eob_decoded ? "" : "n/a ",
                           st.block0_coeff_base_eob_decoded ? st.block0_coeff_base_eob_level : 0u,
                           st.block0_coeff_base_eob_decoded ? st.block0_coeff_base_eob_ctx : 0u,
                           st.block0_coeff_base_decoded ? "" : "n/a ",
                           st.block0_coeff_base_decoded ? st.block0_coeff_base_level : 0u,
                           st.block0_coeff_base_decoded ? st.block0_coeff_base_ctx : 0u,
                           st.block0_coeff_br_decoded ? "" : "n/a ",
                           st.block0_coeff_br_decoded ? st.block0_coeff_br_sym : 0u,
                           st.block0_coeff_br_decoded ? st.block0_coeff_br_ctx : 0u,
                           st.block0_dc_sign_decoded ? "" : "n/a ",
                           st.block0_dc_sign_decoded ? st.block0_dc_sign : 0u,
                           st.block0_dc_sign_decoded ? st.block0_dc_sign_ctx : 0u,
                           st.block0_cfl_alphas_decoded ? "" : "n/a ",
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_signs : 0u,
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_u : 0,
                           st.block0_cfl_alphas_decoded ? st.block0_cfl_alpha_v : 0,
                           st.block0_use_filter_intra_decoded ? "" : "n/a ",
                           st.block0_use_filter_intra_decoded ? st.block0_use_filter_intra : 0u,
                           st.block0_filter_intra_mode_decoded ? "" : "n/a ",
                           st.block0_filter_intra_mode_decoded ? st.block0_filter_intra_mode : 0u,
                           st.block0_angle_delta_y_decoded ? "" : "n/a ",
                           st.block0_angle_delta_y_decoded ? st.block0_angle_delta_y : 0,
                           st.block0_angle_delta_uv_decoded ? "" : "n/a ",
                           st.block0_angle_delta_uv_decoded ? st.block0_angle_delta_uv : 0,
                           st.block0_skip_decoded ? st.block0_r_mi : 0u,
                           st.block0_skip_decoded ? st.block0_c_mi : 0u,
                           st.block0_skip_decoded ? (1u << st.block0_wlog2) : 0u,
                           st.block0_skip_decoded ? (1u << st.block0_hlog2) : 0u);
                    err[0] = 0;
                }
            }

            printf("    tile[%u] r%u c%u: off=%" PRIu64 " size=%" PRIu64 "\n",
                   TileNum,
                   tileRow,
                   tileCol,
                   tile_data_off_abs,
                   tileSize);

            cur += (size_t)tileSize;
            if (remaining < (size_t)(tileSize + TileSizeBytes)) {
                snprintf(err, err_cap, "internal remaining underflow");
                return false;
            }
            remaining -= (size_t)(tileSize + TileSizeBytes);
        }
    }

    if (cur != payload_len || remaining != 0) {
        snprintf(err,
                 err_cap,
                 "tile_group payload not fully consumed (ended at %zu of %zu bytes)",
                 cur,
                 payload_len);
        return false;
    }
    return true;
}

static bool parse_frame_size_render_and_superres(BitReader *br,
                                                 const SeqHdr *seq,
                                                 uint32_t frame_size_override_flag,
                                                 FrameHdr *out,
                                                 char *err,
                                                 size_t err_cap) {
    // frame_size()
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    if (frame_size_override_flag) {
        uint32_t w_minus_1, h_minus_1;
        unsigned wn = (unsigned)(seq->frame_width_bits_minus_1 + 1);
        unsigned hn = (unsigned)(seq->frame_height_bits_minus_1 + 1);
        if (wn > 32 || hn > 32) {
            snprintf(err, err_cap, "unsupported: frame_width_bits/height_bits too large");
            return false;
        }
        if (!br_read_bits(br, wn, &w_minus_1) || !br_read_bits(br, hn, &h_minus_1)) {
            snprintf(err, err_cap, "truncated frame_size override");
            return false;
        }
        frame_width = w_minus_1 + 1;
        frame_height = h_minus_1 + 1;
    } else {
        frame_width = seq->max_frame_width_minus_1 + 1;
        frame_height = seq->max_frame_height_minus_1 + 1;
    }

    // superres_params()
    uint32_t use_superres = 0;
    if (seq->enable_superres) {
        if (!br_read_bit(br, &use_superres)) {
            snprintf(err, err_cap, "truncated use_superres");
            return false;
        }
    }

    uint32_t upscaled_width = frame_width;
    uint32_t coded_width = frame_width;
    uint32_t superres_denom = 8; // SUPERRES_NUM
    if (use_superres) {
        uint32_t coded_denom;
        if (!br_read_bits(br, 3, &coded_denom)) {
            snprintf(err, err_cap, "truncated coded_denom");
            return false;
        }
        superres_denom = coded_denom + 9; // SUPERRES_DENOM_MIN
        coded_width = (upscaled_width * 8u + (superres_denom / 2u)) / superres_denom;
    }

    // compute_image_size()
    out->coded_width = coded_width;
    out->coded_height = frame_height;
    out->upscaled_width = upscaled_width;
    out->mi_cols = 2u * ((coded_width + 7u) >> 3);
    out->mi_rows = 2u * ((frame_height + 7u) >> 3);

    // render_size()
    uint32_t render_and_frame_size_different;
    if (!br_read_bit(br, &render_and_frame_size_different)) {
        snprintf(err, err_cap, "truncated render_and_frame_size_different");
        return false;
    }
    if (render_and_frame_size_different) {
        uint32_t rw_minus_1, rh_minus_1;
        if (!br_read_bits(br, 16, &rw_minus_1) || !br_read_bits(br, 16, &rh_minus_1)) {
            snprintf(err, err_cap, "truncated render_size override");
            return false;
        }
        out->frame_width = rw_minus_1 + 1;
        out->frame_height = rh_minus_1 + 1;
    } else {
        out->frame_width = upscaled_width;
        out->frame_height = frame_height;
    }

    (void)superres_denom;
    return true;
}

static bool parse_uncompressed_header_reduced_still(const uint8_t *payload,
                                                    size_t payload_len,
                                                    const SeqHdr *seq,
                                                    FrameHdr *out,
                                                    TileInfo *tile,
                                                    uint64_t *out_header_bytes,
                                                    char *err,
                                                    size_t err_cap) {
    BitReader br = {payload, payload_len, 0};

    // reduced_still_picture_header implies:
    // show_existing_frame=0, frame_type=KEY_FRAME, FrameIsIntra=1, show_frame=1, showable_frame=0
    out->frame_type = 0; // KEY_FRAME
    out->show_frame = 1;
    out->error_resilient_mode = 1;

    // Many fields still appear; for m3b step1 we only parse a minimal prefix.

    // disable_cdf_update
    uint32_t tmp;
    uint32_t disable_cdf_update = 0;
    if (!br_read_bit(&br, &disable_cdf_update)) {
        snprintf(err, err_cap, "truncated disable_cdf_update");
        return false;
    }
    out->disable_cdf_update = disable_cdf_update;

    // allow_screen_content_tools
    uint32_t allow_screen_content_tools = 0;
    if (seq->seq_force_screen_content_tools == 2) {
        if (!br_read_bit(&br, &allow_screen_content_tools)) {
            snprintf(err, err_cap, "truncated allow_screen_content_tools");
            return false;
        }
    } else {
        allow_screen_content_tools = seq->seq_force_screen_content_tools;
    }
    out->allow_screen_content_tools = allow_screen_content_tools;
    if (allow_screen_content_tools) {
        // force_integer_mv
        if (seq->seq_force_integer_mv == 2) {
            if (!br_read_bit(&br, &tmp)) {
                snprintf(err, err_cap, "truncated force_integer_mv");
                return false;
            }
        }
    }

    // frame_id_numbers_present_flag: for reduced still sequence headers we treat as absent.
    if (seq->frame_id_numbers_present_flag) {
        snprintf(err, err_cap, "unsupported: frame_id_numbers_present_flag with reduced still");
        return false;
    }

    // frame_size_override_flag = 0 in reduced still.

    // order_hint: f(OrderHintBits)
    unsigned order_hint_bits = seq->enable_order_hint ? (unsigned)(seq->order_hint_bits_minus_1 + 1) : 0;
    if (order_hint_bits > 0) {
        if (!br_read_bits(&br, order_hint_bits, &tmp)) {
            snprintf(err, err_cap, "truncated order_hint");
            return false;
        }
    }

    // primary_ref_frame is none (no bits).

    if (seq->decoder_model_info_present_flag) {
        // buffer_removal_time_present_flag
        uint32_t present;
        if (!br_read_bit(&br, &present)) {
            snprintf(err, err_cap, "truncated buffer_removal_time_present_flag");
            return false;
        }
        if (present) {
            snprintf(err, err_cap, "unsupported: buffer_removal_time_present_flag=1");
            return false;
        }
    }

    // refresh_frame_flags = allFrames for KEY_FRAME.

    // reduced_still_picture_header implies frame_size_override_flag=0.
    if (!parse_frame_size_render_and_superres(&br, seq, 0 /* frame_size_override_flag */, out, err, err_cap)) {
        return false;
    }

    // allow_intrabc (only when allow_screen_content_tools && UpscaledWidth == FrameWidth)
    out->allow_intrabc = 0;
    if (allow_screen_content_tools && out->upscaled_width == out->frame_width) {
        if (!br_read_bit(&br, &tmp)) {
            snprintf(err, err_cap, "truncated allow_intrabc");
            return false;
        }
        out->allow_intrabc = tmp;
    }

    // reduced_still_picture_header => disable_frame_end_update_cdf = 1 (no bits)

    if (!parse_tile_info(&br, seq, out, tile, err, err_cap)) {
        return false;
    }

    // Best-effort derivation of coded_lossless without disturbing the main bitreader position.
    // Needed by tile syntax decode decisions (e.g. CFL-allowed uv_mode), but some synthetic test
    // vectors intentionally truncate the header after tile_info().
    out->coded_lossless = 0;
    out->base_q_idx = 0;
    out->tx_mode = 1; // TX_MODE_LARGEST default when unknown.
    {
        BitReader br2 = br;
        QuantizationState qs;
        SegmentationState ss;
        char tmp_err[256];
        if (parse_quantization_params_skip(&br2, seq, &qs, tmp_err, sizeof(tmp_err)) &&
            parse_segmentation_params_skip(&br2, &ss, tmp_err, sizeof(tmp_err))) {
            out->coded_lossless = compute_coded_lossless(&qs, &ss);
            out->base_q_idx = qs.base_q_idx;
            if (out->coded_lossless) {
                out->tx_mode = 0; // ONLY_4X4
            }
        }
    }

    if (out_header_bytes) {
        if (!skip_uncompressed_header_after_tile_info(&br, seq, out, err, err_cap)) {
            return false;
        }
        *out_header_bytes = br.bitpos / 8u;
    }

    // Still consider this a successful parse to the extent implemented.
    (void)br;
    return true;
}

static bool parse_uncompressed_header_nonreduced_still(const uint8_t *payload,
                                                       size_t payload_len,
                                                       const SeqHdr *seq,
                                                       FrameHdr *out,
                                                       TileInfo *tile,
                                                       uint64_t *out_header_bytes,
                                                       char *err,
                                                       size_t err_cap) {
    BitReader br = {payload, payload_len, 0};

    // This function is intentionally narrow: a single intra KEY_FRAME with show_frame=1.
    // We parse only enough to reach frame_size()/render_size() and tile_info().

    if (seq->frame_id_numbers_present_flag) {
        snprintf(err, err_cap, "unsupported: frame_id_numbers_present_flag for still-picture subset");
        return false;
    }

    uint32_t show_existing_frame;
    if (!br_read_bit(&br, &show_existing_frame)) {
        snprintf(err, err_cap, "truncated show_existing_frame");
        return false;
    }
    if (show_existing_frame) {
        snprintf(err, err_cap, "unsupported: show_existing_frame=1");
        return false;
    }

    uint32_t frame_type;
    uint32_t show_frame;
    if (!br_read_bits(&br, 2, &frame_type) || !br_read_bit(&br, &show_frame)) {
        snprintf(err, err_cap, "truncated frame_type/show_frame");
        return false;
    }
    out->frame_type = frame_type;
    out->show_frame = show_frame;

    // Current subset: require keyframe shown.
    if (frame_type != 0 /* KEY_FRAME */ || show_frame != 1) {
        snprintf(err, err_cap, "unsupported: expected KEY_FRAME with show_frame=1");
        return false;
    }

    // showable_frame is derived when show_frame==1 (no bits)
    out->error_resilient_mode = 1; // derived when KEY_FRAME && show_frame

    // disable_cdf_update
    uint32_t tmp;
    uint32_t disable_cdf_update = 0;
    if (!br_read_bit(&br, &disable_cdf_update)) {
        snprintf(err, err_cap, "truncated disable_cdf_update");
        return false;
    }
    out->disable_cdf_update = disable_cdf_update;

    // allow_screen_content_tools
    uint32_t allow_screen_content_tools = 0;
    if (seq->seq_force_screen_content_tools == 2) {
        if (!br_read_bit(&br, &allow_screen_content_tools)) {
            snprintf(err, err_cap, "truncated allow_screen_content_tools");
            return false;
        }
    } else {
        allow_screen_content_tools = seq->seq_force_screen_content_tools;
    }

    out->allow_screen_content_tools = allow_screen_content_tools;

    // force_integer_mv (only relevant if allow_screen_content_tools)
    if (allow_screen_content_tools) {
        if (seq->seq_force_integer_mv == 2) {
            if (!br_read_bit(&br, &tmp)) {
                snprintf(err, err_cap, "truncated force_integer_mv");
                return false;
            }
        }
    }

    // frame_size_override_flag (present when not reduced still)
    uint32_t frame_size_override_flag;
    if (!br_read_bit(&br, &frame_size_override_flag)) {
        snprintf(err, err_cap, "truncated frame_size_override_flag");
        return false;
    }

    // order_hint
    unsigned order_hint_bits = seq->enable_order_hint ? (unsigned)(seq->order_hint_bits_minus_1 + 1) : 0;
    if (order_hint_bits > 0) {
        if (!br_read_bits(&br, order_hint_bits, &tmp)) {
            snprintf(err, err_cap, "truncated order_hint");
            return false;
        }
    }

    // primary_ref_frame is derived because FrameIsIntra||error_resilient_mode

    if (seq->decoder_model_info_present_flag) {
        // buffer_removal_time_present_flag
        uint32_t present;
        if (!br_read_bit(&br, &present)) {
            snprintf(err, err_cap, "truncated buffer_removal_time_present_flag");
            return false;
        }
        if (present) {
            snprintf(err, err_cap, "unsupported: buffer_removal_time_present_flag=1");
            return false;
        }
    }

    // refresh_frame_flags is derived (KEY_FRAME && show_frame)
    // FrameIsIntra => frame_size()/render_size() follow immediately.

    if (!parse_frame_size_render_and_superres(&br, seq, frame_size_override_flag, out, err, err_cap)) {
        return false;
    }

    // allow_intrabc
    out->allow_intrabc = 0;
    if (allow_screen_content_tools && out->upscaled_width == out->frame_width) {
        if (!br_read_bit(&br, &tmp)) {
            snprintf(err, err_cap, "truncated allow_intrabc");
            return false;
        }
        out->allow_intrabc = tmp;
    }

    // disable_frame_end_update_cdf
    if (seq->reduced_still_picture_header || disable_cdf_update) {
        // forced to 1
    } else {
        if (!br_read_bit(&br, &tmp)) {
            snprintf(err, err_cap, "truncated disable_frame_end_update_cdf");
            return false;
        }
    }

    // primary_ref_frame is none for our intra keyframe subset => tile_info() follows
    if (!parse_tile_info(&br, seq, out, tile, err, err_cap)) {
        return false;
    }

    out->coded_lossless = 0;
    out->base_q_idx = 0;
    {
        BitReader br2 = br;
        QuantizationState qs;
        SegmentationState ss;
        char tmp_err[256];
        if (parse_quantization_params_skip(&br2, seq, &qs, tmp_err, sizeof(tmp_err)) &&
            parse_segmentation_params_skip(&br2, &ss, tmp_err, sizeof(tmp_err))) {
            out->coded_lossless = compute_coded_lossless(&qs, &ss);
            out->base_q_idx = qs.base_q_idx;
        }
    }

    if (out_header_bytes) {
        if (!skip_uncompressed_header_after_tile_info(&br, seq, out, err, err_cap)) {
            return false;
        }
        *out_header_bytes = br.bitpos / 8u;
    }

    return true;
}

typedef struct {
    bool found;
    uint8_t obu_type;
    uint64_t payload_off;
    uint64_t payload_size;
    uint64_t next_off;
} ObuPayloadTyped;

static bool find_first_of_types(const uint8_t *data,
                                size_t data_len,
                                const uint8_t *wanted_types,
                                size_t wanted_types_len,
                                ObuPayloadTyped *out,
                                char *err,
                                size_t err_cap) {
    out->found = false;
    out->obu_type = 0;
    out->payload_off = 0;
    out->payload_size = 0;
    out->next_off = 0;

    size_t off = 0;
    while (off < data_len) {
        if (data[off] == 0) {
            size_t z = off;
            while (z < data_len && data[z] == 0) {
                z++;
            }
            if (z == data_len) {
                return true;
            }
        }

        if (off >= data_len) {
            break;
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
            snprintf(err, err_cap, "OBU has_size_field=0 (unsupported)");
            return false;
        }

        if (extension_flag) {
            if (off >= data_len) {
                snprintf(err, err_cap, "truncated obu_extension_header");
                return false;
            }
            off += 1;
        }

        uint64_t obu_size;
        if (!read_leb128_u64(data, data_len, &off, &obu_size)) {
            snprintf(err, err_cap, "failed to read obu_size");
            return false;
        }
        if (obu_size > (uint64_t)(data_len - off)) {
            snprintf(err, err_cap, "obu_size exceeds remaining bytes");
            return false;
        }

        for (size_t i = 0; i < wanted_types_len; i++) {
            if (obu_type == wanted_types[i]) {
                out->found = true;
                out->obu_type = obu_type;
                out->payload_off = (uint64_t)off;
                out->payload_size = obu_size;
                out->next_off = (uint64_t)off + obu_size;
                return true;
            }
        }

        off += (size_t)obu_size;
    }
    return true;
}

int main(int argc, char **argv) {
    keep_helpers_linked();
    const char *path = NULL;
    const char *dump_tiles_dir = NULL;
    bool check_tile_trailing = false;
    bool check_tile_trailing_strict = false;
    uint32_t tile_consume_bools = 0;
    bool check_tile_trailingbits = false;
    bool check_tile_trailingbits_strict = false;
    bool decode_tile_syntax = false;
    bool decode_tile_syntax_strict = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--dump-tiles")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--dump-tiles requires DIR\n");
                return 2;
            }
            dump_tiles_dir = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--check-tile-trailing")) {
            check_tile_trailing = true;
            continue;
        }
        if (!strcmp(argv[i], "--check-tile-trailing-strict")) {
            check_tile_trailing = true;
            check_tile_trailing_strict = true;
            continue;
        }
        if (!strcmp(argv[i], "--tile-consume-bools")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--tile-consume-bools requires N\n");
                return 2;
            }
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (!end || *end != 0 || v > 1000000ul) {
                fprintf(stderr, "invalid --tile-consume-bools value\n");
                return 2;
            }
            tile_consume_bools = (uint32_t)v;
            continue;
        }
        if (!strcmp(argv[i], "--check-tile-trailingbits")) {
            check_tile_trailingbits = true;
            continue;
        }
        if (!strcmp(argv[i], "--check-tile-trailingbits-strict")) {
            check_tile_trailingbits = true;
            check_tile_trailingbits_strict = true;
            continue;
        }
        if (!strcmp(argv[i], "--decode-tile-syntax")) {
            decode_tile_syntax = true;
            continue;
        }
        if (!strcmp(argv[i], "--decode-tile-syntax-strict")) {
            decode_tile_syntax = true;
            decode_tile_syntax_strict = true;
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
        usage(stderr);
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
        return 1;
    }

    uint64_t size;
    if (!get_file_size(f, &size)) {
        fprintf(stderr, "failed to get size for %s\n", path);
        fclose(f);
        return 1;
    }
    if (size > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "file too large to parse in-memory (%" PRIu64 " bytes)\n", size);
        fclose(f);
        return 1;
    }

    uint8_t *bytes = (uint8_t *)malloc((size_t)size);
    if (!bytes) {
        fprintf(stderr, "OOM allocating %" PRIu64 " bytes\n", size);
        fclose(f);
        return 1;
    }

    if (fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "failed to read %s\n", path);
        free(bytes);
        fclose(f);
        return 1;
    }
    fclose(f);

    char err[256];
    err[0] = 0;

    ObuPayload seq_obu;
    if (!find_first_obu_payload(bytes, (size_t)size, 1 /* OBU_SEQUENCE_HEADER */, &seq_obu, err, sizeof(err))) {
        fprintf(stderr, "Sequence Header scan failed: %s\n", err);
        free(bytes);
        return 1;
    }
    if (!seq_obu.found) {
        fprintf(stderr, "unsupported: no Sequence Header OBU found\n");
        free(bytes);
        return 1;
    }

    SeqHdr seq;
    if (!parse_seq_hdr_min(bytes + (size_t)seq_obu.payload_off, (size_t)seq_obu.payload_size, &seq, err, sizeof(err))) {
        fprintf(stderr, "Sequence Header parse failed: %s\n", err);
        free(bytes);
        return 1;
    }

    // Note: AVIF still images in the wild may have still_picture=0.
    // For m3b we accept still_picture=0 or 1, but keep a narrow decode subset:
    // a single KEY_FRAME with show_frame=1 and a limited set of header features.

    // Prefer a standalone frame header OBU if present; otherwise fall back to a Frame OBU.
    const uint8_t wanted_frame_types[] = {
        3, // OBU_FRAME_HEADER
        6, // OBU_FRAME
        7, // OBU_REDUNDANT_FRAME_HEADER
    };
    ObuPayloadTyped frame_obu;
    if (!find_first_of_types(bytes, (size_t)size, wanted_frame_types, sizeof(wanted_frame_types), &frame_obu, err, sizeof(err))) {
        fprintf(stderr, "Frame OBU scan failed: %s\n", err);
        free(bytes);
        return 1;
    }
    if (!frame_obu.found) {
        fprintf(stderr, "unsupported: no Frame/FrameHeader OBU found\n");
        free(bytes);
        return 1;
    }

    FrameHdr fh;
    TileInfo ti;
    bool ok = false;
    uint64_t frame_header_bytes = 0;
    if (seq.reduced_still_picture_header) {
        ok = parse_uncompressed_header_reduced_still(bytes + (size_t)frame_obu.payload_off,
                                                     (size_t)frame_obu.payload_size,
                                                     &seq,
                                                     &fh,
                                                     &ti,
                                                     frame_obu.obu_type == 6 ? &frame_header_bytes : NULL,
                                                     err,
                                                     sizeof(err));
    } else {
        ok = parse_uncompressed_header_nonreduced_still(bytes + (size_t)frame_obu.payload_off,
                                                        (size_t)frame_obu.payload_size,
                                                        &seq,
                                                        &fh,
                                                        &ti,
                                                        frame_obu.obu_type == 6 ? &frame_header_bytes : NULL,
                                                        err,
                                                        sizeof(err));
    }
    if (!ok) {
        fprintf(stderr, "Frame Header parse failed: %s\n", err);
        free(bytes);
        return 1;
    }

    printf("Sequence Header:\n");
    printf("  still_picture=%u\n", seq.still_picture);
    printf("  reduced_still_picture_header=%u\n", seq.reduced_still_picture_header);

    printf("Frame Header (partial):\n");
    printf("  frame_type=%u\n", fh.frame_type);
    printf("  show_frame=%u\n", fh.show_frame);
    printf("  error_resilient_mode=%u\n", fh.error_resilient_mode);
    printf("  frame_width=%u\n", fh.frame_width);
    printf("  frame_height=%u\n", fh.frame_height);
    printf("  coded_width=%u\n", fh.coded_width);
    printf("  coded_height=%u\n", fh.coded_height);
    printf("  upscaled_width=%u\n", fh.upscaled_width);

    printf("Tile info (from frame header):\n");
    printf("  tile_cols=%u tile_rows=%u\n", ti.tile_cols, ti.tile_rows);
    printf("  tile_cols_log2=%u tile_rows_log2=%u\n", ti.tile_cols_log2, ti.tile_rows_log2);
    printf("  tile_size_bytes=%u\n", ti.tile_size_bytes);
    printf("  context_update_tile_id=%u\n", ti.context_update_tile_id);

    TileDumpCtx dump;
    memset(&dump, 0, sizeof(dump));
    dump.dir = dump_tiles_dir;
    dump.tg_index = 0;

    if (frame_obu.obu_type == 6) {
        printf("Tile group scan (embedded in OBU_FRAME):\n");
        if (frame_header_bytes >= frame_obu.payload_size) {
            fprintf(stderr, "Embedded tile group start exceeds OBU_FRAME payload\n");
            free(bytes);
            return 1;
        }
        uint64_t tg_payload_off_abs = frame_obu.payload_off + frame_header_bytes;
        const uint8_t *tg_payload = bytes + (size_t)tg_payload_off_abs;
        size_t tg_payload_len = (size_t)(frame_obu.payload_size - frame_header_bytes);
        if (!parse_tile_group_obu_and_print(tg_payload,
                                            tg_payload_len,
                                            tg_payload_off_abs,
                                            &seq,
                                            &fh,
                                            &ti,
                                            dump_tiles_dir ? &dump : NULL,
                                            check_tile_trailing,
                                            check_tile_trailing_strict,
                                            tile_consume_bools,
                                            check_tile_trailingbits,
                                            check_tile_trailingbits_strict,
                                            decode_tile_syntax,
                                            decode_tile_syntax_strict,
                                            err,
                                            sizeof(err))) {
            fprintf(stderr, "Embedded tile group parse failed: %s\n", err);
            free(bytes);
            return 1;
        }
    } else {
        printf("Tile group scan (OBU_TILE_GROUP):\n");
        size_t off = (size_t)frame_obu.next_off;
        bool any = false;
        while (off < (size_t)size) {
            // Skip trailing zeros.
            if (bytes[off] == 0) {
                size_t z = off;
                while (z < (size_t)size && bytes[z] == 0) {
                    z++;
                }
                if (z == (size_t)size) {
                    break;
                }
            }

            uint8_t header = bytes[off++];
            uint8_t forbidden = (header >> 7) & 1u;
            uint8_t obu_type = (header >> 3) & 0x0Fu;
            uint8_t extension_flag = (header >> 2) & 1u;
            uint8_t has_size_field = (header >> 1) & 1u;

            if (forbidden != 0) {
                fprintf(stderr, "OBU scan failed after frame header: forbidden bit set\n");
                break;
            }
            if (!has_size_field) {
                fprintf(stderr, "OBU scan failed after frame header: has_size_field=0\n");
                break;
            }
            if (extension_flag) {
                if (off >= (size_t)size) {
                    fprintf(stderr, "OBU scan failed after frame header: truncated extension header\n");
                    break;
                }
                off += 1;
            }

            uint64_t obu_size;
            size_t tmp_off = off;
            if (!read_leb128_u64(bytes, (size_t)size, &tmp_off, &obu_size)) {
                fprintf(stderr, "OBU scan failed after frame header: bad leb128 size\n");
                break;
            }
            off = tmp_off;
            if (obu_size > (uint64_t)((size_t)size - off)) {
                fprintf(stderr, "OBU scan failed after frame header: obu_size exceeds file\n");
                break;
            }

            uint64_t payload_off_abs = (uint64_t)off;
            if (obu_type == 4 /* OBU_TILE_GROUP */) {
                any = true;
                dump.tg_index++;
                if (!parse_tile_group_obu_and_print(bytes + off,
                                                    (size_t)obu_size,
                                                    payload_off_abs,
                                                    &seq,
                                                    &fh,
                                                    &ti,
                                                    dump_tiles_dir ? &dump : NULL,
                                                    check_tile_trailing,
                                                    check_tile_trailing_strict,
                                                    tile_consume_bools,
                                                    check_tile_trailingbits,
                                                    check_tile_trailingbits_strict,
                                                    decode_tile_syntax,
                                                    decode_tile_syntax_strict,
                                                    err,
                                                    sizeof(err))) {
                    fprintf(stderr, "Tile group parse failed: %s\n", err);
                    free(bytes);
                    return 1;
                }
            }
            off += (size_t)obu_size;
        }
        if (!any) {
            printf("  (no OBU_TILE_GROUP found after frame header OBU)\n");
        }
    }

    if (dump_tiles_dir) {
        if (!write_text_file_frame_info(dump_tiles_dir, &seq, &fh, &ti, err, sizeof(err))) {
            fprintf(stderr, "Tile dump frame_info write failed: %s\n", err);
            free(bytes);
            return 1;
        }
        printf("Dumped %u tile payload(s) into %s\n", dump.tiles_written, dump_tiles_dir);
    }

    printf("Note: AVIF container properties (e.g. ispe/colr) remain authoritative for presentation metadata.\n");

    free(bytes);
    return 0;
}
