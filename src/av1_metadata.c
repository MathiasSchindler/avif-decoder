#include "av1_metadata.h"

#include "base.h"

#define AV1_METADATA_HDR_CLL 1U
#define AV1_METADATA_HDR_MDCV 2U
#define AV1_METADATA_SCALABILITY 3U
#define AV1_METADATA_ITUT_T35 4U
#define AV1_METADATA_TIMECODE 5U

void av1_metadata_reset(AvifdecImageInfo *info) {
    if (info == 0) return;
    info->metadata_present_mask = 0U;
    avifdec_memory_fill(&info->hdr_cll, 0U, sizeof(info->hdr_cll));
    avifdec_memory_fill(&info->hdr_mdcv, 0U, sizeof(info->hdr_mdcv));
    info->scalability_mode_idc = 0U;
    info->scalability_flags = 0U;
    info->spatial_layer_count = 0U;
    info->temporal_group_size = 0U;
    avifdec_memory_fill(
        info->spatial_layer_width, 0U,
        sizeof(info->spatial_layer_width));
    avifdec_memory_fill(
        info->spatial_layer_height, 0U,
        sizeof(info->spatial_layer_height));
    avifdec_memory_fill(
        info->spatial_layer_ref_id, 0U,
        sizeof(info->spatial_layer_ref_id));
    info->scalability_checksum = 0U;
    info->itu_t35_country_code = 0U;
    info->itu_t35_country_code_extension = 0U;
    info->itu_t35_payload_size = 0U;
    info->itu_t35_payload_checksum = 0U;
    avifdec_memory_fill(&info->timecode, 0U, sizeof(info->timecode));
}

static void av1_metadata_hash(uint64_t *checksum, uint64_t value) {
    unsigned int byte;

    for (byte = 0U; byte < 8U; ++byte) {
        *checksum ^= (uint8_t)(value >> (byte * 8U));
        *checksum *= (uint64_t)1099511628211ULL;
    }
}

static AvifdecStatus av1_metadata_payload_bits(
    const Av1Bits *bits,
    size_t start,
    Av1Bits *payload) {
    size_t end;

    if (bits == 0 || payload == 0 ||
        !avifdec_size_add(bits->start, bits->size, &end) ||
        start > end) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    av1_bits_init(payload, bits->stream, start, end - start);
    return AVIFDEC_OK;
}

static AvifdecStatus av1_metadata_parse_hdr_cll(
    Av1Bits *bits,
    AvifdecImageInfo *info) {
    info->hdr_cll.max_cll = (uint16_t)av1_bits_read(bits, 16U);
    info->hdr_cll.max_fall = (uint16_t)av1_bits_read(bits, 16U);
    if (bits->status != AVIFDEC_OK) return bits->status;
    info->metadata_present_mask |=
        (uint8_t)(1U << AV1_METADATA_HDR_CLL);
    return av1_trailing_bits(bits);
}

static AvifdecStatus av1_metadata_parse_hdr_mdcv(
    Av1Bits *bits,
    AvifdecImageInfo *info) {
    unsigned int primary;

    for (primary = 0U; primary < 3U; ++primary) {
        info->hdr_mdcv.primary_x[primary] =
            (uint16_t)av1_bits_read(bits, 16U);
        info->hdr_mdcv.primary_y[primary] =
            (uint16_t)av1_bits_read(bits, 16U);
    }
    info->hdr_mdcv.white_point_x =
        (uint16_t)av1_bits_read(bits, 16U);
    info->hdr_mdcv.white_point_y =
        (uint16_t)av1_bits_read(bits, 16U);
    info->hdr_mdcv.luminance_max = av1_bits_read(bits, 32U);
    info->hdr_mdcv.luminance_min = av1_bits_read(bits, 32U);
    if (bits->status != AVIFDEC_OK) return bits->status;
    info->metadata_present_mask |=
        (uint8_t)(1U << AV1_METADATA_HDR_MDCV);
    return av1_trailing_bits(bits);
}

static AvifdecStatus av1_metadata_parse_scalability(
    Av1Bits *bits,
    const Av1MetadataConfig *config,
    AvifdecImageInfo *info) {
    uint64_t checksum = (uint64_t)1469598103934665603ULL;
    uint8_t mode = (uint8_t)av1_bits_read(bits, 8U);

    info->scalability_mode_idc = mode;
    av1_metadata_hash(&checksum, mode);
    if (mode == 14U) {
        uint8_t spatial_layers_minus_1 =
            (uint8_t)av1_bits_read(bits, 2U);
        uint8_t dimensions_present =
            (uint8_t)av1_bits_read(bits, 1U);
        uint8_t descriptions_present =
            (uint8_t)av1_bits_read(bits, 1U);
        uint8_t temporal_group_present =
            (uint8_t)av1_bits_read(bits, 1U);
        uint8_t reserved = (uint8_t)av1_bits_read(bits, 3U);
        unsigned int layer;

        if (reserved != 0U) return AVIFDEC_INVALID_DATA;
        info->spatial_layer_count =
            (uint8_t)(spatial_layers_minus_1 + 1U);
        info->scalability_flags =
            dimensions_present |
            (uint8_t)(descriptions_present << 1U) |
            (uint8_t)(temporal_group_present << 2U);
        av1_metadata_hash(&checksum, info->spatial_layer_count);
        av1_metadata_hash(&checksum, info->scalability_flags);
        if (dimensions_present) {
            for (layer = 0U; layer < info->spatial_layer_count; ++layer) {
                uint16_t width = (uint16_t)av1_bits_read(bits, 16U);
                uint16_t height = (uint16_t)av1_bits_read(bits, 16U);

                if (width == 0U || height == 0U ||
                    width > config->max_width ||
                    height > config->max_height) {
                    return AVIFDEC_INVALID_DATA;
                }
                info->spatial_layer_width[layer] = width;
                info->spatial_layer_height[layer] = height;
                av1_metadata_hash(&checksum, width);
                av1_metadata_hash(&checksum, height);
            }
        }
        if (descriptions_present) {
            for (layer = 0U; layer < info->spatial_layer_count; ++layer) {
                info->spatial_layer_ref_id[layer] =
                    (uint8_t)av1_bits_read(bits, 8U);
                av1_metadata_hash(
                    &checksum, info->spatial_layer_ref_id[layer]);
            }
        }
        if (temporal_group_present) {
            uint8_t group_size = (uint8_t)av1_bits_read(bits, 8U);
            unsigned int picture;

            info->temporal_group_size = group_size;
            av1_metadata_hash(&checksum, group_size);
            for (picture = 0U; picture < group_size; ++picture) {
                uint8_t temporal_id =
                    (uint8_t)av1_bits_read(bits, 3U);
                uint8_t temporal_switching_up =
                    (uint8_t)av1_bits_read(bits, 1U);
                uint8_t spatial_switching_up =
                    (uint8_t)av1_bits_read(bits, 1U);
                uint8_t reference_count =
                    (uint8_t)av1_bits_read(bits, 3U);
                unsigned int reference;

                if (picture == 0U && temporal_id != 0U) {
                    return AVIFDEC_INVALID_DATA;
                }
                av1_metadata_hash(&checksum, temporal_id);
                av1_metadata_hash(&checksum, temporal_switching_up);
                av1_metadata_hash(&checksum, spatial_switching_up);
                av1_metadata_hash(&checksum, reference_count);
                for (reference = 0U; reference < reference_count;
                     ++reference) {
                    av1_metadata_hash(
                        &checksum, av1_bits_read(bits, 8U));
                }
            }
        }
    } else {
        info->spatial_layer_count = 0U;
        info->scalability_flags = 0U;
        info->temporal_group_size = 0U;
    }
    if (bits->status != AVIFDEC_OK) return bits->status;
    info->scalability_checksum = checksum;
    info->metadata_present_mask |=
        (uint8_t)(1U << AV1_METADATA_SCALABILITY);
    return av1_trailing_bits(bits);
}

static AvifdecStatus av1_metadata_parse_itut_t35(
    const Av1Bits *bits,
    size_t start,
    AvifdecImageInfo *info) {
    size_t end;
    size_t position = start;
    size_t trailer;
    uint8_t byte;
    uint64_t checksum = (uint64_t)1469598103934665603ULL;

    if (!avifdec_size_add(bits->start, bits->size, &end) ||
        position >= end ||
        !av1_stream_byte_at(bits->stream, position++, &byte, 0)) {
        return AVIFDEC_TRUNCATED;
    }
    info->itu_t35_country_code = byte;
    info->itu_t35_country_code_extension = 0U;
    if (byte == 0xffU) {
        if (position >= end ||
            !av1_stream_byte_at(bits->stream, position++, &byte, 0)) {
            return AVIFDEC_TRUNCATED;
        }
        info->itu_t35_country_code_extension = byte;
    }
    trailer = end;
    while (trailer > position) {
        if (!av1_stream_byte_at(
                bits->stream, trailer - 1U, &byte, 0)) {
            return AVIFDEC_TRUNCATED;
        }
        if (byte != 0U) break;
        --trailer;
    }
    if (trailer == position ||
        !av1_stream_byte_at(bits->stream, trailer - 1U, &byte, 0) ||
        byte != 0x80U) {
        return AVIFDEC_INVALID_DATA;
    }
    --trailer;
    info->itu_t35_payload_size = trailer - position;
    while (position < trailer) {
        if (!av1_stream_byte_at(bits->stream, position++, &byte, 0)) {
            return AVIFDEC_TRUNCATED;
        }
        checksum ^= byte;
        checksum *= (uint64_t)1099511628211ULL;
    }
    info->itu_t35_payload_checksum = checksum;
    info->metadata_present_mask |=
        (uint8_t)(1U << AV1_METADATA_ITUT_T35);
    return AVIFDEC_OK;
}

static uint64_t av1_metadata_timecode_position(
    const AvifdecTimecode *timecode,
    const Av1MetadataConfig *config) {
    uint64_t seconds =
        ((uint64_t)timecode->hours * 60U + timecode->minutes) *
        60U + timecode->seconds;
    uint64_t ticks_per_picture =
        config->equal_picture_interval
        ? (uint64_t)config->num_ticks_per_picture_minus_1 + 1U
        : 1U;

    return seconds * config->time_scale +
           (uint64_t)timecode->n_frames * ticks_per_picture +
           timecode->time_offset_value;
}

static AvifdecStatus av1_metadata_parse_timecode(
    Av1Bits *bits,
    const Av1MetadataConfig *config,
    AvifdecImageInfo *info) {
    AvifdecTimecode previous = info->timecode;
    AvifdecTimecode current;
    int had_previous =
        (info->metadata_present_mask &
         (uint8_t)(1U << AV1_METADATA_TIMECODE)) != 0U;

    avifdec_memory_fill(&current, 0U, sizeof(current));
    current.counting_type = (uint8_t)av1_bits_read(bits, 5U);
    current.full_timestamp = (uint8_t)av1_bits_read(bits, 1U);
    current.discontinuity = (uint8_t)av1_bits_read(bits, 1U);
    current.count_dropped = (uint8_t)av1_bits_read(bits, 1U);
    current.n_frames = (uint16_t)av1_bits_read(bits, 9U);
    if (current.counting_type > 6U) return AVIFDEC_INVALID_DATA;
    if (current.full_timestamp) {
        current.seconds_present = 1U;
        current.minutes_present = 1U;
        current.hours_present = 1U;
        current.seconds = (uint8_t)av1_bits_read(bits, 6U);
        current.minutes = (uint8_t)av1_bits_read(bits, 6U);
        current.hours = (uint8_t)av1_bits_read(bits, 5U);
    } else {
        current.seconds_present = (uint8_t)av1_bits_read(bits, 1U);
        if (current.seconds_present) {
            current.seconds = (uint8_t)av1_bits_read(bits, 6U);
            current.minutes_present = (uint8_t)av1_bits_read(bits, 1U);
            if (current.minutes_present) {
                current.minutes = (uint8_t)av1_bits_read(bits, 6U);
                current.hours_present = (uint8_t)av1_bits_read(bits, 1U);
                if (current.hours_present) {
                    current.hours = (uint8_t)av1_bits_read(bits, 5U);
                }
            }
        }
    }
    if (!current.seconds_present) {
        if (!had_previous || !previous.seconds_present) {
            return AVIFDEC_INVALID_DATA;
        }
        current.seconds = previous.seconds;
        current.seconds_present = previous.seconds_present;
    }
    if (!current.minutes_present) {
        if (!had_previous || !previous.minutes_present) {
            return AVIFDEC_INVALID_DATA;
        }
        current.minutes = previous.minutes;
        current.minutes_present = previous.minutes_present;
    }
    if (!current.hours_present) {
        if (!had_previous || !previous.hours_present) {
            return AVIFDEC_INVALID_DATA;
        }
        current.hours = previous.hours;
        current.hours_present = previous.hours_present;
    }
    current.time_offset_length = (uint8_t)av1_bits_read(bits, 5U);
    if (current.time_offset_length != 0U) {
        current.time_offset_value =
            av1_bits_read(bits, current.time_offset_length);
    }
    if (bits->status != AVIFDEC_OK) return bits->status;
    if (current.seconds > 59U || current.minutes > 59U ||
        current.hours > 23U ||
        (current.counting_type == 0U &&
         current.time_offset_length != 0U)) {
        return AVIFDEC_INVALID_DATA;
    }
    if (config->timing_info_present) {
        uint64_t denominator =
            (uint64_t)2U * config->num_units_in_display_tick;
        uint64_t frame_limit =
            ((uint64_t)config->time_scale + denominator - 1U) /
            denominator;

        if (current.n_frames >= frame_limit) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    if (had_previous) {
        if (current.counting_type != previous.counting_type ||
            current.time_offset_length != previous.time_offset_length ||
            (!current.discontinuity &&
             av1_metadata_timecode_position(&current, config) <
             av1_metadata_timecode_position(&previous, config))) {
            return AVIFDEC_INVALID_DATA;
        }
    }
    info->timecode = current;
    info->metadata_present_mask |=
        (uint8_t)(1U << AV1_METADATA_TIMECODE);
    return av1_trailing_bits(bits);
}

AvifdecStatus av1_metadata_parse(Av1Bits *bits,
                                 const Av1MetadataConfig *config,
                                 uint8_t extension_flag,
                                 AvifdecImageInfo *info) {
    Av1Stream stream;
    Av1Bits payload;
    size_t end;
    size_t metadata_type;
    AvifdecStatus status;

    if (bits == 0 || config == 0 || info == 0 ||
        extension_flag > 1U) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    stream = *bits->stream;
    stream.position = bits->start;
    if (!avifdec_size_add(bits->start, bits->size, &end)) {
        return AVIFDEC_OVERFLOW;
    }
    stream.size = end;
    status = av1_leb128(&stream, &metadata_type);
    if (status != AVIFDEC_OK) return status;
    if (metadata_type != AV1_METADATA_ITUT_T35 && extension_flag) {
        return AVIFDEC_INVALID_DATA;
    }
    status = av1_metadata_payload_bits(bits, stream.position, &payload);
    if (status != AVIFDEC_OK) return status;
    if (metadata_type == AV1_METADATA_HDR_CLL) {
        return av1_metadata_parse_hdr_cll(&payload, info);
    }
    if (metadata_type == AV1_METADATA_HDR_MDCV) {
        return av1_metadata_parse_hdr_mdcv(&payload, info);
    }
    if (metadata_type == AV1_METADATA_SCALABILITY) {
        return av1_metadata_parse_scalability(&payload, config, info);
    }
    if (metadata_type == AV1_METADATA_ITUT_T35) {
        return av1_metadata_parse_itut_t35(
            bits, stream.position, info);
    }
    if (metadata_type == AV1_METADATA_TIMECODE) {
        return av1_metadata_parse_timecode(&payload, config, info);
    }
    return AVIFDEC_OK;
}
