#include "av1.h"

int32_t av1_relative_distance(uint8_t enable_order_hint,
                              uint8_t order_hint_bits,
                              uint32_t first,
                              uint32_t second) {
    uint32_t sign_bit;
    uint32_t difference;

    if (!enable_order_hint) return 0;
    if (order_hint_bits == 0U || order_hint_bits > 8U) return 0;
    sign_bit = 1U << (order_hint_bits - 1U);
    difference = first - second;
    return (int32_t)((difference & (sign_bit - 1U)) -
                     (difference & sign_bit));
}

AvifdecStatus av1_mark_reference_frames(Av1ReferenceSlot slots[8],
                                        uint8_t id_length,
                                        uint8_t delta_frame_id_length,
                                        uint32_t current_frame_id) {
    uint32_t id_range;
    uint32_t delta_range;
    unsigned int index;

    if (slots == 0 || id_length == 0U || id_length > 16U ||
        delta_frame_id_length == 0U ||
        delta_frame_id_length >= id_length) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    id_range = 1U << id_length;
    delta_range = 1U << delta_frame_id_length;
    if (current_frame_id >= id_range) return AVIFDEC_INVALID_ARGUMENT;
    for (index = 0U; index < AV1_NUM_REF_FRAMES; ++index) {
        if (current_frame_id > delta_range) {
            if (slots[index].frame_id > current_frame_id ||
                slots[index].frame_id < current_frame_id - delta_range) {
                slots[index].valid = 0U;
            }
        } else if (slots[index].frame_id > current_frame_id &&
                   slots[index].frame_id <
                       id_range + current_frame_id - delta_range) {
            slots[index].valid = 0U;
        }
    }
    return AVIFDEC_OK;
}

static int av1_find_reference(const uint8_t used[8],
                              const int32_t shifted[8],
                              int32_t current_hint,
                              int backward,
                              int latest) {
    int selected = -1;
    int32_t selected_hint = 0;
    unsigned int index;

    for (index = 0U; index < AV1_NUM_REF_FRAMES; ++index) {
        int eligible = backward ? shifted[index] >= current_hint
                                : shifted[index] < current_hint;
        int better;

        if (used[index] || !eligible) continue;
        better = selected < 0 ||
                 (latest ? shifted[index] >= selected_hint
                         : shifted[index] < selected_hint);
        if (better) {
            selected = (int)index;
            selected_hint = shifted[index];
        }
    }
    return selected;
}

AvifdecStatus av1_set_frame_refs(const Av1ReferenceSlot slots[8],
                                 uint8_t order_hint_bits,
                                 uint32_t order_hint,
                                 uint8_t last_frame_idx,
                                 uint8_t gold_frame_idx,
                                 uint8_t ref_frame_idx[7]) {
    static const uint8_t remaining_refs[5] = { 1U, 2U, 4U, 5U, 6U };
    uint8_t used[8] = { 0U };
    int32_t shifted[8];
    int32_t current_hint;
    int selected;
    unsigned int index;

    if (slots == 0 || ref_frame_idx == 0 || order_hint_bits == 0U ||
        order_hint_bits > 8U || last_frame_idx >= AV1_NUM_REF_FRAMES ||
        gold_frame_idx >= AV1_NUM_REF_FRAMES) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    current_hint = (int32_t)(1U << (order_hint_bits - 1U));
    for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
        ref_frame_idx[index] = 0xffU;
    }
    ref_frame_idx[0] = last_frame_idx;
    ref_frame_idx[3] = gold_frame_idx;
    used[last_frame_idx] = 1U;
    used[gold_frame_idx] = 1U;
    for (index = 0U; index < AV1_NUM_REF_FRAMES; ++index) {
        shifted[index] = current_hint + av1_relative_distance(
            1U, order_hint_bits, slots[index].order_hint, order_hint);
    }
    if (shifted[last_frame_idx] >= current_hint ||
        shifted[gold_frame_idx] >= current_hint) {
        return AVIFDEC_INVALID_DATA;
    }

    selected = av1_find_reference(used, shifted, current_hint, 1, 1);
    if (selected >= 0) {
        ref_frame_idx[6] = (uint8_t)selected;
        used[selected] = 1U;
    }
    selected = av1_find_reference(used, shifted, current_hint, 1, 0);
    if (selected >= 0) {
        ref_frame_idx[4] = (uint8_t)selected;
        used[selected] = 1U;
    }
    selected = av1_find_reference(used, shifted, current_hint, 1, 0);
    if (selected >= 0) {
        ref_frame_idx[5] = (uint8_t)selected;
        used[selected] = 1U;
    }
    for (index = 0U; index < 5U; ++index) {
        uint8_t ref = remaining_refs[index];

        if (ref_frame_idx[ref] != 0xffU) continue;
        selected = av1_find_reference(used, shifted, current_hint, 0, 1);
        if (selected >= 0) {
            ref_frame_idx[ref] = (uint8_t)selected;
            used[selected] = 1U;
        }
    }
    selected = -1;
    for (index = 0U; index < AV1_NUM_REF_FRAMES; ++index) {
        if (selected < 0 || shifted[index] < shifted[selected]) {
            selected = (int)index;
        }
    }
    for (index = 0U; index < AV1_REFS_PER_FRAME; ++index) {
        if (ref_frame_idx[index] == 0xffU) {
            ref_frame_idx[index] = (uint8_t)selected;
        }
    }
    return AVIFDEC_OK;
}

AvifdecStatus av1_reference_show_existing(
    const Av1ReferenceSlot slots[8],
    uint8_t map_index,
    int frame_id_present,
    uint32_t display_frame_id,
    Av1ReferenceSlot *shown) {
    const Av1ReferenceSlot *slot;

    if (slots == 0 || shown == 0 || map_index >= AV1_NUM_REF_FRAMES ||
        (frame_id_present != 0 && frame_id_present != 1)) {
        return AVIFDEC_INVALID_ARGUMENT;
    }
    slot = &slots[map_index];
    if (!slot->valid || !slot->showable_frame ||
        (frame_id_present && display_frame_id != slot->frame_id)) {
        return AVIFDEC_INVALID_DATA;
    }
    *shown = *slot;
    return AVIFDEC_OK;
}
