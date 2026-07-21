#include "avif_properties_internal.h"
#include "base.h"

static AvifdecStatus avif_properties_fail(
    const AvifPropertyParseContext *context,
    AvifdecStatus status,
    size_t offset,
    uint32_t box_type) {
    if (context->error != 0 &&
        context->error->status == AVIFDEC_OK) {
        context->error->status = status;
        context->error->offset = offset;
        context->error->context = box_type;
    }
    *context->failed = 1;
    return status;
}

static int avif_properties_find_item(
    const AvifPropertyParseContext *context,
    uint32_t item_id) {
    size_t index;

    for (index = 0U; index < context->item_count; ++index) {
        if (context->items[index].id == item_id) return (int)index;
    }
    return -1;
}

static int avif_text_equal(const unsigned char *bytes,
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

AvifdecStatus avif_properties_parse(
    const AvifPropertyParseContext *context,
    uint32_t item_id,
    AvifdecImageInfo *info) {
    int item_index = avif_properties_find_item(context, item_id);
    uint32_t item_type;
    size_t association_index;
    int seen_av1c = 0;
    int seen_ispe = 0;
    int seen_pixi = 0;
    int seen_nclx = 0;
    int seen_icc = 0;
    int seen_auxc = 0;
    unsigned int transform_stage = 0U;
    uint8_t pixel_bit_depth = 0U;

    if (item_index < 0) {
        return avif_properties_fail(
            context, AVIFDEC_INVALID_DATA, context->iinf->offset,
            AVIFDEC_FOURCC('i', 'n', 'f', 'e'));
    }
    item_type = context->items[item_index].type;
    info->primary_item_id = item_id;
    info->primary_item_type = item_type;
    info->selected_layer = 0xffU;

    for (association_index = 0U;
         association_index < context->association_count;
         ++association_index) {
        const AvifAssociation *association =
            &context->associations[association_index];
        const AvifProperty *property;
        const unsigned char *payload;
        size_t payload_size;

        if (association->item_id != item_id) continue;
        property =
            &context->properties[association->property_index - 1U];
        payload = context->data + property->box.payload_offset;
        payload_size = property->box.payload_size;
        if (property->type == AVIFDEC_FOURCC('a', 'v', '1', 'C')) {
            if (seen_av1c || payload_size < 4U ||
                payload[0] != 0x81U || (payload[3] & 0xe0U) != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->profile = payload[1] >> 5;
            info->level = payload[1] & 31U;
            info->tier = payload[2] >> 7;
            info->bit_depth = (payload[2] & 0x40U) == 0U ? 8U
                              : (payload[2] & 0x20U) == 0U ? 10U : 12U;
            info->monochrome = (payload[2] >> 4) & 1U;
            info->subsampling_x = (payload[2] >> 3) & 1U;
            info->subsampling_y = (payload[2] >> 2) & 1U;
            info->chroma_sample_position = payload[2] & 3U;
            seen_av1c = 1;
        } else if (property->type ==
                   AVIFDEC_FOURCC('i', 's', 'p', 'e')) {
            if (seen_ispe || payload_size != 12U ||
                payload[0] != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->width = avifdec_load_u32be(payload + 4U);
            info->height = avifdec_load_u32be(payload + 8U);
            if (info->width == 0U || info->height == 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            seen_ispe = 1;
        } else if (property->type ==
                   AVIFDEC_FOURCC('p', 'i', 'x', 'i')) {
            size_t channel;

            if (seen_pixi || payload_size < 5U ||
                payload[0] != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->channel_count = payload[4U];
            if (info->channel_count == 0U ||
                payload_size != 5U + info->channel_count) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            for (channel = 0U; channel < info->channel_count;
                 ++channel) {
                if (channel == 0U) pixel_bit_depth = payload[5U];
                if (payload[5U + channel] != pixel_bit_depth) {
                    return avif_properties_fail(
                        context, AVIFDEC_INVALID_DATA,
                        property->box.offset, property->type);
                }
            }
            seen_pixi = 1;
        } else if (property->type ==
                   AVIFDEC_FOURCC('c', 'o', 'l', 'r')) {
            uint32_t color_type;

            if (payload_size < 4U) {
                return avif_properties_fail(
                    context, AVIFDEC_TRUNCATED,
                    property->box.offset, property->type);
            }
            color_type = avifdec_load_u32be(payload);
            if (color_type == AVIFDEC_FOURCC('n', 'c', 'l', 'x')) {
                if (seen_nclx || payload_size != 11U ||
                    (payload[10U] & 0x7fU) != 0U) {
                    return avif_properties_fail(
                        context, AVIFDEC_INVALID_DATA,
                        property->box.offset, property->type);
                }
                info->color_primaries =
                    avifdec_load_u16be(payload + 4U);
                info->transfer_characteristics =
                    avifdec_load_u16be(payload + 6U);
                info->matrix_coefficients =
                    avifdec_load_u16be(payload + 8U);
                info->color_range = payload[10U] >> 7;
                info->has_nclx = 1U;
                seen_nclx = 1;
            } else if (
                color_type == AVIFDEC_FOURCC('r', 'I', 'C', 'C') ||
                color_type == AVIFDEC_FOURCC('p', 'r', 'o', 'f')) {
                if (seen_icc || payload_size == 4U) {
                    return avif_properties_fail(
                        context, AVIFDEC_INVALID_DATA,
                        property->box.offset, property->type);
                }
                info->icc_data = payload + 4U;
                info->icc_size = payload_size - 4U;
                seen_icc = 1;
            } else if (association->essential) {
                return avif_properties_fail(
                    context, AVIFDEC_UNSUPPORTED,
                    property->box.offset, property->type);
            }
        } else if (property->type ==
                   AVIFDEC_FOURCC('p', 'a', 's', 'p')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_PASP) != 0U ||
                payload_size != 8U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->pixel_aspect_h_spacing = avifdec_load_u32be(payload);
            info->pixel_aspect_v_spacing =
                avifdec_load_u32be(payload + 4U);
            if (info->pixel_aspect_h_spacing == 0U ||
                info->pixel_aspect_v_spacing == 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->transform_flags |= AVIFDEC_TRANSFORM_PASP;
        } else if (property->type ==
                   AVIFDEC_FOURCC('c', 'l', 'a', 'p')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U ||
                payload_size != 32U || !association->essential ||
                transform_stage != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->clean_aperture.width_n =
                avifdec_load_u32be(payload);
            info->clean_aperture.width_d =
                avifdec_load_u32be(payload + 4U);
            info->clean_aperture.height_n =
                avifdec_load_u32be(payload + 8U);
            info->clean_aperture.height_d =
                avifdec_load_u32be(payload + 12U);
            info->clean_aperture.horizontal_offset_n =
                (int32_t)avifdec_load_u32be(payload + 16U);
            info->clean_aperture.horizontal_offset_d =
                avifdec_load_u32be(payload + 20U);
            info->clean_aperture.vertical_offset_n =
                (int32_t)avifdec_load_u32be(payload + 24U);
            info->clean_aperture.vertical_offset_d =
                avifdec_load_u32be(payload + 28U);
            info->transform_flags |= AVIFDEC_TRANSFORM_CLAP;
            transform_stage = 1U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('i', 'r', 'o', 't')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U ||
                payload_size != 1U || (payload[0] & 0xfcU) != 0U ||
                !association->essential || transform_stage > 1U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->irot_angle = payload[0] & 3U;
            info->transform_flags |= AVIFDEC_TRANSFORM_IROT;
            transform_stage = 2U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('i', 'm', 'i', 'r')) {
            if ((info->transform_flags & AVIFDEC_TRANSFORM_IMIR) != 0U ||
                payload_size != 1U || (payload[0] & 0xfeU) != 0U ||
                !association->essential || transform_stage > 2U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->imir_axis = payload[0] & 1U;
            info->transform_flags |= AVIFDEC_TRANSFORM_IMIR;
            transform_stage = 3U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('a', 'u', 'x', 'C')) {
            size_t string_size;

            if (seen_auxc || payload_size < 5U ||
                payload[0] != 0U || payload[1] != 0U ||
                payload[2] != 0U || payload[3] != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            seen_auxc = 1;
            for (string_size = 0U;
                 4U + string_size < payload_size &&
                 payload[4U + string_size] != 0U;
                 ++string_size) {
            }
            if (4U + string_size >= payload_size) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            if (avif_text_equal(
                    payload + 4U, string_size,
                    "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha") ||
                avif_text_equal(
                    payload + 4U, string_size,
                    "urn:mpeg:hevc:2015:auxid:1")) {
                info->auxiliary_type = AVIFDEC_AUXILIARY_ALPHA;
            } else if (avif_text_equal(
                           payload + 4U, string_size,
                           "urn:mpeg:mpegB:cicp:systems:auxiliary:depth")) {
                info->auxiliary_type = AVIFDEC_AUXILIARY_DEPTH;
            }
        } else if (property->type ==
                   AVIFDEC_FOURCC('c', 'l', 'l', 'i')) {
            if (info->item_hdr_cll_present || payload_size != 4U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->item_hdr_cll.max_cll = avifdec_load_u16be(payload);
            info->item_hdr_cll.max_fall =
                avifdec_load_u16be(payload + 2U);
            info->item_hdr_cll_present = 1U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('m', 'd', 'c', 'v')) {
            size_t primary;

            if (info->item_hdr_mdcv_present || payload_size != 24U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            for (primary = 0U; primary < 3U; ++primary) {
                info->item_hdr_mdcv.primary_x[primary] =
                    avifdec_load_u16be(payload + primary * 4U);
                info->item_hdr_mdcv.primary_y[primary] =
                    avifdec_load_u16be(payload + primary * 4U + 2U);
            }
            info->item_hdr_mdcv.white_point_x =
                avifdec_load_u16be(payload + 12U);
            info->item_hdr_mdcv.white_point_y =
                avifdec_load_u16be(payload + 14U);
            info->item_hdr_mdcv.luminance_max =
                avifdec_load_u32be(payload + 16U);
            info->item_hdr_mdcv.luminance_min =
                avifdec_load_u32be(payload + 20U);
            info->item_hdr_mdcv_present = 1U;
        } else if (property->type ==
                   AVIFDEC_FOURCC('a', '1', 'o', 'p')) {
            if (info->has_a1op || payload_size != 1U ||
                !association->essential) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->has_a1op = 1U;
            info->a1op_index = payload[0];
        } else if (property->type ==
                   AVIFDEC_FOURCC('l', 's', 'e', 'l')) {
            uint16_t layer_id;

            if (info->has_lsel || payload_size != 2U ||
                !association->essential) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            layer_id = avifdec_load_u16be(payload);
            if (layer_id > 3U && layer_id != 0xffffU) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->has_lsel = 1U;
            info->lsel_layer_id = layer_id;
            info->selected_layer =
                layer_id == 0xffffU ? 0xffU : (uint8_t)layer_id;
        } else if (property->type ==
                   AVIFDEC_FOURCC('a', '1', 'l', 'x')) {
            size_t layer;
            size_t field_size;
            int zero_seen = 0;

            if (info->is_layered || association->essential ||
                (payload_size != 7U && payload_size != 13U) ||
                (payload[0] & 0xfeU) != 0U) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            field_size = (payload[0] & 1U) != 0U ? 4U : 2U;
            if (payload_size != 1U + 3U * field_size) {
                return avif_properties_fail(
                    context, AVIFDEC_INVALID_DATA,
                    property->box.offset, property->type);
            }
            info->is_layered = 1U;
            info->layer_count = 1U;
            for (layer = 0U; layer < 3U; ++layer) {
                size_t layer_size = field_size == 2U
                    ? avifdec_load_u16be(
                          payload + 1U + layer * field_size)
                    : avifdec_load_u32be(
                          payload + 1U + layer * field_size);

                if (layer_size == 0U) {
                    zero_seen = 1;
                } else {
                    if (zero_seen) {
                        return avif_properties_fail(
                            context, AVIFDEC_INVALID_DATA,
                            property->box.offset, property->type);
                    }
                    ++info->layer_count;
                }
                info->layer_sizes[layer] = layer_size;
            }
        } else if (association->essential) {
            return avif_properties_fail(
                context, AVIFDEC_UNSUPPORTED,
                property->box.offset, property->type);
        }
    }
    if (!seen_ispe || !seen_pixi ||
        (item_type == AVIFDEC_FOURCC('a', 'v', '0', '1') &&
         !seen_av1c)) {
        return avif_properties_fail(
            context, AVIFDEC_INVALID_DATA, context->ipco->offset,
            context->ipco->type);
    }
    if (seen_av1c &&
        (pixel_bit_depth != info->bit_depth ||
         info->channel_count != (info->monochrome ? 1U : 3U))) {
        return avif_properties_fail(
            context, AVIFDEC_INVALID_DATA, context->ipco->offset,
            AVIFDEC_FOURCC('p', 'i', 'x', 'i'));
    }
    if (!seen_av1c) {
        info->bit_depth = pixel_bit_depth;
        info->monochrome = info->channel_count == 1U;
    }
    info->crop.x = 0U;
    info->crop.y = 0U;
    info->crop.width = info->width;
    info->crop.height = info->height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_CLAP) != 0U &&
        !avifdec_clap_to_crop_rect(
            &info->clean_aperture, info->width, info->height,
            &info->crop)) {
        return avif_properties_fail(
            context, AVIFDEC_INVALID_DATA, context->ipco->offset,
            AVIFDEC_FOURCC('c', 'l', 'a', 'p'));
    }
    info->presentation_width = info->crop.width;
    info->presentation_height = info->crop.height;
    if ((info->transform_flags & AVIFDEC_TRANSFORM_IROT) != 0U &&
        (info->irot_angle & 1U) != 0U) {
        uint32_t swap = info->presentation_width;

        info->presentation_width = info->presentation_height;
        info->presentation_height = swap;
    }
    return AVIFDEC_OK;
}
