#ifndef AVIFDEC_SEQUENCE_DECODE_PRIVATE_H
#define AVIFDEC_SEQUENCE_DECODE_PRIVATE_H

#include "avif_sequence_index.h"

typedef struct {
    const AvifSequenceIndex *index;
    uint32_t track_id;
    size_t first_sample_index;
    size_t last_sample_index;
    size_t span_count;
    uint8_t prepend_config;
    uint8_t alpha;
} AvifSequenceReplay;

typedef struct {
    AvifSequenceReplay main;
    AvifSequenceReplay alpha;
    size_t workspace_required;
    AvifSequencePresentationInfo presentation;
    AvifdecImageInfo alpha_image;
    uint8_t has_alpha;
} AvifSequenceDecodePlan;

typedef AvifdecStatus (*AvifSequenceReplayQuery)(
    void *user_data,
    const AvifSequenceReplay *replay,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    AvifdecImageInfo *info,
    AvifdecError *error);

typedef AvifdecStatus (*AvifSequenceReplayDecode)(
    void *user_data,
    const AvifSequenceReplay *replay,
    const AvifdecLimits *limits,
    const AvifdecExecutor *executor,
    const AvifdecImageInfo *info,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifdecError *error);

typedef struct {
    void *user_data;
    AvifSequenceReplayQuery query;
    AvifSequenceReplayDecode decode;
} AvifSequenceDecodeCallbacks;

AvifdecStatus avif_sequence_replay_span_query(
    const AvifSequenceReplay *replay,
    size_t span_index,
    AvifdecSpan *span,
    size_t *sample_index,
    AvifdecError *error);

AvifdecStatus avif_sequence_decode_plan_query(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    const AvifSequenceDecodeCallbacks *callbacks,
    AvifSequenceDecodePlan *plan,
    AvifdecError *error);

/*
 * Image, trace, and presentation structs commit only after both callbacks
 * succeed. Callback writes to caller-owned pixel planes are not rolled back.
 */
AvifdecStatus avif_sequence_decode_presentation(
    const AvifSequenceIndex *index,
    const AvifSequenceSelection *selection,
    const AvifdecExecutor *executor,
    size_t presentation_index,
    const AvifSequenceDecodeCallbacks *callbacks,
    void *workspace,
    size_t workspace_size,
    AvifdecImage *image,
    AvifdecEntropyTrace *trace,
    AvifSequencePresentationInfo *presentation,
    AvifdecError *error);

#endif
