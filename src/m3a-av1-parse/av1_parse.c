#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static void usage(FILE *out) {
    fprintf(out,
            "Usage: av1_parse [--list-obus] <in.av1>\n"
            "\n"
            "Parses size-delimited AV1 OBUs and prints Sequence Header summary.\n");
}

static bool read_exact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

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

// --- LEB128 and OBU framing ---

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
    uint64_t seq_hdr_offset;
    uint64_t seq_hdr_size;
    uint32_t type_counts[16];
} ObuScan;

static const char *obu_type_name(uint8_t t) {
    switch (t) {
        case 0: return "reserved";
        case 1: return "sequence_header";
        case 2: return "temporal_delimiter";
        case 3: return "frame_header";
        case 4: return "tile_group";
        case 5: return "metadata";
        case 6: return "frame";
        case 7: return "redundant_frame_header";
        case 8: return "tile_list";
        case 9: return "padding";
        default: return "reserved";
    }
}

static bool scan_obus_find_seq_hdr(const uint8_t *data,
                                  size_t data_len,
                                  bool list_obus,
                                  ObuScan *scan,
                                  char *err,
                                  size_t err_cap) {
    memset(scan, 0, sizeof(*scan));

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

        scan->obu_count++;
        scan->type_counts[obu_type]++;
        if (obu_type == 1) {
            scan->seq_hdr_count++;
            // Record the first sequence header.
            if (scan->seq_hdr_count == 1) {
                scan->seq_hdr_offset = (uint64_t)off;
                scan->seq_hdr_size = obu_size;
            }
        }

        if (list_obus) {
            printf("OBU @%zu: type=%u(%s) payload=%" PRIu64 " bytes\n",
                   off,
                   (unsigned)obu_type,
                   obu_type_name(obu_type),
                   obu_size);
        }

        off += (size_t)obu_size;
    }

    return true;
}

// --- Bitreader and minimal Sequence Header parsing ---

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t bitpos; // from start, in bits
} BitReader;

static bool br_read_bit(BitReader *br, uint32_t *out_bit) {
    if (br->bitpos >= br->size * 8) {
        return false;
    }
    size_t byte_off = br->bitpos / 8;
    unsigned bit_in_byte = 7u - (unsigned)(br->bitpos % 8);
    uint8_t byte = br->data[byte_off];
    *out_bit = (byte >> bit_in_byte) & 1u;
    br->bitpos++;
    return true;
}

static bool br_read_bits(BitReader *br, unsigned n, uint32_t *out) {
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

static bool br_read_uvlc(BitReader *br, uint32_t *out) {
    // uvlc() as used by AV1: leading zeros then a one, then that many bits.
    // Value = (1<<leadingZeros)-1 + suffix.
    uint32_t leading = 0;
    for (;;) {
        uint32_t b;
        if (!br_read_bit(br, &b)) {
            return false;
        }
        if (b == 0) {
            leading++;
            if (leading > 31) {
                return false;
            }
            continue;
        }
        break;
    }

    if (leading == 0) {
        *out = 0;
        return true;
    }

    uint32_t suffix;
    if (!br_read_bits(br, leading, &suffix)) {
        return false;
    }
    *out = ((1u << leading) - 1u) + suffix;
    return true;
}

typedef struct {
    uint32_t profile;
    uint32_t still_picture;
    uint32_t reduced_still_picture_header;

    uint32_t operating_point_idc;

    uint32_t bit_depth;
    uint32_t monochrome;
    uint32_t subsampling_x;
    uint32_t subsampling_y;

    uint32_t color_primaries;
    uint32_t transfer_characteristics;
    uint32_t matrix_coefficients;
    uint32_t full_range;
} SeqHdrSummary;

static bool parse_color_config(BitReader *br, uint32_t seq_profile, SeqHdrSummary *out) {
    // AV1 spec: color_config(). We parse a subset needed for AVIF.
    uint32_t high_bitdepth;
    if (!br_read_bit(br, &high_bitdepth)) {
        return false;
    }

    uint32_t twelve_bit = 0;
    if (seq_profile == 2 && high_bitdepth) {
        if (!br_read_bit(br, &twelve_bit)) {
            return false;
        }
    }

    uint32_t bit_depth = 8;
    if (!high_bitdepth) {
        bit_depth = 8;
    } else if (seq_profile == 2) {
        bit_depth = twelve_bit ? 12 : 10;
    } else {
        bit_depth = 10;
    }
    out->bit_depth = bit_depth;

    if (seq_profile == 1) {
        out->monochrome = 0;
    } else {
        uint32_t mono;
        if (!br_read_bit(br, &mono)) {
            return false;
        }
        out->monochrome = mono;
    }

    uint32_t color_description_present_flag;
    if (!br_read_bit(br, &color_description_present_flag)) {
        return false;
    }
    if (color_description_present_flag) {
        uint32_t cp, tc, mc;
        if (!br_read_bits(br, 8, &cp) || !br_read_bits(br, 8, &tc) || !br_read_bits(br, 8, &mc)) {
            return false;
        }
        out->color_primaries = cp;
        out->transfer_characteristics = tc;
        out->matrix_coefficients = mc;
    } else {
        // Unspecified.
        out->color_primaries = 2;
        out->transfer_characteristics = 2;
        out->matrix_coefficients = 2;
    }

    // color_range
    uint32_t color_range;
    if (!br_read_bit(br, &color_range)) {
        return false;
    }
    out->full_range = color_range;

    if (out->monochrome) {
        // No chroma planes.
        out->subsampling_x = 1;
        out->subsampling_y = 1;
        return true;
    }

    // Special case: if signaled as sRGB/identity, subsampling is forced to 4:4:4.
    // (This is commonly used for RGB signaling.)
    if (out->color_primaries == 1 && out->transfer_characteristics == 13 && out->matrix_coefficients == 0) {
        out->subsampling_x = 0;
        out->subsampling_y = 0;
        uint32_t separate_uv_delta_q;
        if (!br_read_bit(br, &separate_uv_delta_q)) {
            return false;
        }
        return true;
    }

    // Otherwise, subsampling depends on profile (and sometimes bit depth for profile 2).
    uint32_t subsampling_x = 0;
    uint32_t subsampling_y = 0;

    if (seq_profile == 0) {
        subsampling_x = 1;
        subsampling_y = 1;
    } else if (seq_profile == 1) {
        subsampling_x = 0;
        subsampling_y = 0;
    } else {
        // profile 2
        if (bit_depth == 12) {
            uint32_t sx;
            if (!br_read_bit(br, &sx)) {
                return false;
            }
            subsampling_x = sx;
            if (subsampling_x) {
                uint32_t sy;
                if (!br_read_bit(br, &sy)) {
                    return false;
                }
                subsampling_y = sy;
            } else {
                subsampling_y = 0;
            }
        } else {
            // 8/10-bit profile 2 defaults to 4:2:2.
            subsampling_x = 1;
            subsampling_y = 0;
        }
    }

    out->subsampling_x = subsampling_x;
    out->subsampling_y = subsampling_y;

    if (subsampling_x && subsampling_y) {
        // chroma_sample_position (2 bits)
        uint32_t csp;
        if (!br_read_bits(br, 2, &csp)) {
            return false;
        }
        (void)csp;
    }

    // separate_uv_delta_q
    uint32_t separate_uv_delta_q;
    if (!br_read_bit(br, &separate_uv_delta_q)) {
        return false;
    }
    (void)separate_uv_delta_q;

    return true;
}

static bool parse_sequence_header_obu_payload(const uint8_t *payload, size_t payload_len, SeqHdrSummary *out, char *err, size_t err_cap) {
    memset(out, 0, sizeof(*out));
    if (err_cap > 0) {
        snprintf(err, err_cap, "sequence header parse failed");
    }

    BitReader br;
    br.data = payload;
    br.size = payload_len;
    br.bitpos = 0;

    uint32_t seq_profile;
    uint32_t still_picture;
    uint32_t reduced_still_picture_header;

    if (!br_read_bits(&br, 3, &seq_profile) || !br_read_bit(&br, &still_picture) ||
        !br_read_bit(&br, &reduced_still_picture_header)) {
        snprintf(err, err_cap, "truncated sequence header fields");
        return false;
    }

    out->profile = seq_profile;
    out->still_picture = still_picture;
    out->reduced_still_picture_header = reduced_still_picture_header;

    uint32_t timing_info_present_flag = 0;
    uint32_t decoder_model_info_present_flag = 0;
    uint32_t initial_display_delay_present_flag = 0;
    uint32_t buffer_delay_length_minus_1 = 0;

    if (reduced_still_picture_header) {
        out->operating_point_idc = 0;
        out->bit_depth = 8;
        out->monochrome = 0;
        out->subsampling_x = 1;
        out->subsampling_y = 1;

        // Skip: seq_level_idx[0] (5 bits)
        uint32_t tmp;
        if (!br_read_bits(&br, 5, &tmp)) {
            snprintf(err, err_cap, "truncated seq_level_idx");
            return false;
        }
        // Parse color_config
        if (!parse_color_config(&br, seq_profile, out)) {
            snprintf(err, err_cap, "failed to parse color_config");
            return false;
        }

        return true;
    }

    // Non-reduced sequence header.
    if (!br_read_bit(&br, &timing_info_present_flag)) {
        snprintf(err, err_cap, "truncated timing_info_present_flag");
        return false;
    }
    if (timing_info_present_flag) {
        // timing_info(): num_units_in_display_tick(32), time_scale(32), equal_picture_interval(1), if eq then uvlc
        uint32_t tmp32;
        for (int i = 0; i < 2; i++) {
            if (!br_read_bits(&br, 32, &tmp32)) {
                snprintf(err, err_cap, "truncated timing_info");
                return false;
            }
        }
        uint32_t eq;
        if (!br_read_bit(&br, &eq)) {
            return false;
        }
        if (eq) {
            uint32_t v;
            if (!br_read_uvlc(&br, &v)) {
                return false;
            }
        }
        if (!br_read_bit(&br, &decoder_model_info_present_flag)) {
            return false;
        }
        if (decoder_model_info_present_flag) {
            // decoder_model_info(): buffer_delay_length_minus_1(5), num_units_in_decoding_tick(32),
            // buffer_removal_time_length_minus_1(5), frame_presentation_time_length_minus_1(5)
            uint32_t t;
            if (!br_read_bits(&br, 5, &t)) {
                return false;
            }
            buffer_delay_length_minus_1 = t;
            if (!br_read_bits(&br, 32, &t)) {
                return false;
            }
            if (!br_read_bits(&br, 5, &t) || !br_read_bits(&br, 5, &t)) {
                return false;
            }
        }
    } else {
        decoder_model_info_present_flag = 0;
    }

    if (!br_read_bit(&br, &initial_display_delay_present_flag)) {
        return false;
    }

    uint32_t operating_points_cnt_minus_1;
    if (!br_read_bits(&br, 5, &operating_points_cnt_minus_1)) {
        snprintf(err, err_cap, "truncated operating_points_cnt_minus_1");
        return false;
    }

    // Parse all operating points to keep the bitstream aligned. Record op[0] idc.
    for (uint32_t i = 0; i <= operating_points_cnt_minus_1; i++) {
        uint32_t operating_point_idc;
        if (!br_read_bits(&br, 12, &operating_point_idc)) {
            return false;
        }
        if (i == 0) {
            out->operating_point_idc = operating_point_idc;
        }

        uint32_t seq_level_idx;
        if (!br_read_bits(&br, 5, &seq_level_idx)) {
            return false;
        }

        if (seq_level_idx > 7) {
            uint32_t seq_tier;
            if (!br_read_bit(&br, &seq_tier)) {
                return false;
            }
        }

        if (decoder_model_info_present_flag) {
            uint32_t decoder_model_present_for_this_op;
            if (!br_read_bit(&br, &decoder_model_present_for_this_op)) {
                return false;
            }
            if (decoder_model_present_for_this_op) {
                // operating_parameters_info(): decoder_buffer_delay, encoder_buffer_delay, low_delay_mode_flag
                uint32_t dummy;
                unsigned n = buffer_delay_length_minus_1 + 1;
                if (n > 32) {
                    snprintf(err, err_cap, "unsupported: buffer_delay_length_minus_1 too large");
                    return false;
                }
                if (!br_read_bits(&br, n, &dummy) || !br_read_bits(&br, n, &dummy)) {
                    return false;
                }
                if (!br_read_bit(&br, &dummy)) {
                    return false;
                }
            }
        }

        if (initial_display_delay_present_flag) {
            uint32_t initial_display_delay_present_for_this_op;
            if (!br_read_bit(&br, &initial_display_delay_present_for_this_op)) {
                return false;
            }
            if (initial_display_delay_present_for_this_op) {
                uint32_t dummy;
                if (!br_read_bits(&br, 4, &dummy)) {
                    return false;
                }
            }
        }
    }

    // frame_width_bits_minus_1 / frame_height_bits_minus_1 / max_frame_{width,height}_minus_1
    uint32_t frame_width_bits_minus_1;
    uint32_t frame_height_bits_minus_1;
    if (!br_read_bits(&br, 4, &frame_width_bits_minus_1) || !br_read_bits(&br, 4, &frame_height_bits_minus_1)) {
        return false;
    }

    uint32_t dummy;
    if (!br_read_bits(&br, frame_width_bits_minus_1 + 1, &dummy) || !br_read_bits(&br, frame_height_bits_minus_1 + 1, &dummy)) {
        return false;
    }

    // frame_id_numbers_present_flag
    uint32_t frame_id_numbers_present_flag;
    if (!br_read_bit(&br, &frame_id_numbers_present_flag)) {
        return false;
    }
    if (frame_id_numbers_present_flag) {
        uint32_t a, b;
        if (!br_read_bits(&br, 4, &a) || !br_read_bits(&br, 3, &b)) {
            return false;
        }
    }

    // Skip a bunch of feature flags we don't need for the summary.
    // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    for (int i = 0; i < 3; i++) {
        uint32_t b;
        if (!br_read_bit(&br, &b)) {
            return false;
        }
    }

    // enable_interintra_compound, enable_masked_compound, enable_warped_motion, enable_dual_filter
    for (int i = 0; i < 4; i++) {
        uint32_t b;
        if (!br_read_bit(&br, &b)) {
            return false;
        }
    }

    // enable_order_hint
    uint32_t enable_order_hint;
    if (!br_read_bit(&br, &enable_order_hint)) {
        return false;
    }

    // enable_jnt_comp and enable_ref_frame_mvs
    if (enable_order_hint) {
        uint32_t b;
        if (!br_read_bit(&br, &b) || !br_read_bit(&br, &b)) {
            return false;
        }
    }

    // seq_choose_screen_content_tools / seq_force_screen_content_tools
    uint32_t seq_choose_screen_content_tools;
    if (!br_read_bit(&br, &seq_choose_screen_content_tools)) {
        return false;
    }

    uint32_t seq_force_screen_content_tools = 0;
    if (seq_choose_screen_content_tools) {
        seq_force_screen_content_tools = 2;
    } else {
        uint32_t v;
        if (!br_read_bit(&br, &v)) {
            return false;
        }
        seq_force_screen_content_tools = v;
    }

    // seq_choose_integer_mv / seq_force_integer_mv (present only if screen content tools are used)
    if (seq_force_screen_content_tools > 0) {
        uint32_t seq_choose_integer_mv;
        if (!br_read_bit(&br, &seq_choose_integer_mv)) {
            return false;
        }
        if (!seq_choose_integer_mv) {
            uint32_t v;
            if (!br_read_bit(&br, &v)) {
                return false;
            }
        }
    }

    // order_hint_bits_minus_1 if enable_order_hint
    if (enable_order_hint) {
        uint32_t dummy;
        if (!br_read_bits(&br, 3, &dummy)) {
            return false;
        }
    }

    // enable_superres, enable_cdef, enable_restoration
    for (int i = 0; i < 3; i++) {
        uint32_t b;
        if (!br_read_bit(&br, &b)) {
            return false;
        }
    }
    // Now parse color_config.
    if (!parse_color_config(&br, seq_profile, out)) {
        snprintf(err, err_cap, "failed to parse color_config");
        return false;
    }

    return true;
}

int main(int argc, char **argv) {
    bool list_obus = false;
    const char *path = NULL;

    if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--list-obus")) {
        list_obus = true;
        path = argv[2];
    } else if (argc == 2) {
        path = argv[1];
    } else {
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

    if (!read_exact(f, bytes, (size_t)size)) {
        fprintf(stderr, "failed to read %s\n", path);
        free(bytes);
        fclose(f);
        return 1;
    }
    fclose(f);

    char err[256];
    err[0] = '\0';
    ObuScan scan;
    if (!scan_obus_find_seq_hdr(bytes, (size_t)size, list_obus, &scan, err, sizeof(err))) {
        fprintf(stderr, "OBU scan failed: %s\n", err);
        free(bytes);
        return 1;
    }

    printf("OBUs: %u\n", scan.obu_count);
    printf("OBU types:\n");
    for (unsigned t = 0; t < 16; t++) {
        if (scan.type_counts[t] == 0) {
            continue;
        }
        printf("  %2u (%s): %u\n", t, obu_type_name((uint8_t)t), scan.type_counts[t]);
    }

    if (scan.seq_hdr_count == 0) {
        fprintf(stderr, "unsupported: no Sequence Header OBU found\n");
        free(bytes);
        return 1;
    }
    if (scan.seq_hdr_count > 1) {
        fprintf(stderr, "unsupported: multiple Sequence Header OBUs (%u)\n", scan.seq_hdr_count);
        // Continue parsing the first one for diagnostics.
    }

    if (scan.seq_hdr_offset + scan.seq_hdr_size > size) {
        fprintf(stderr, "internal error: sequence header bounds\n");
        free(bytes);
        return 1;
    }

    err[0] = '\0';
    SeqHdrSummary sh;
    if (!parse_sequence_header_obu_payload(bytes + (size_t)scan.seq_hdr_offset, (size_t)scan.seq_hdr_size, &sh, err, sizeof(err))) {
        fprintf(stderr, "Sequence Header parse failed: %s\n", err);
        free(bytes);
        return 1;
    }

    printf("Sequence Header (bitstream):\n");
    printf("  profile=%u\n", sh.profile);
    printf("  still_picture=%u\n", sh.still_picture);
    printf("  reduced_still_picture_header=%u\n", sh.reduced_still_picture_header);
    printf("  operating_point_idc=%u\n", sh.operating_point_idc);
    printf("  bit_depth=%u\n", sh.bit_depth);
    printf("  monochrome=%u\n", sh.monochrome);
    printf("  subsampling_x=%u\n", sh.subsampling_x);
    printf("  subsampling_y=%u\n", sh.subsampling_y);
    printf("  full_range=%u\n", sh.full_range);
    printf("  cicp: primaries=%u transfer=%u matrix=%u\n", sh.color_primaries, sh.transfer_characteristics, sh.matrix_coefficients);
    printf("Note: AVIF often signals CICP/range via container properties (colr/nclx); bitstream defaults may differ.\n");

    free(bytes);
    return 0;
}
