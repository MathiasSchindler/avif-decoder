#include "avifdec.h"
#include "bmff.h"

#define FUZZ_MAX_DIMENSION 256U
#define FUZZ_MAX_PIXELS \
    ((size_t)FUZZ_MAX_DIMENSION * FUZZ_MAX_DIMENSION)
#define FUZZ_WORKSPACE_SIZE (32U * 1024U * 1024U)
#define FUZZ_WORKSPACE_ALIGNMENT 16U
#define FUZZ_INDEX_WORKSPACE_SIZE (4U * 1024U * 1024U)

static unsigned char fuzz_workspace[
    FUZZ_WORKSPACE_SIZE + FUZZ_WORKSPACE_ALIGNMENT];
static unsigned char fuzz_index_workspace[
    FUZZ_INDEX_WORKSPACE_SIZE + FUZZ_WORKSPACE_ALIGNMENT];
static uint16_t fuzz_y[FUZZ_MAX_PIXELS];
static uint16_t fuzz_u[FUZZ_MAX_PIXELS];
static uint16_t fuzz_v[FUZZ_MAX_PIXELS];
static uint16_t fuzz_alpha[FUZZ_MAX_PIXELS];

typedef struct {
    size_t worker_count;
} FuzzExecutor;

static AvifdecStatus fuzz_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg) {
    const FuzzExecutor *executor = (const FuzzExecutor *)user_data;
    size_t worker_count = executor->worker_count;
    size_t worker_index;
    AvifdecStatus first_status = AVIFDEC_OK;

    (void)min_chunk;
    if (count == 0U) return AVIFDEC_OK;
    if (worker_count > count) worker_count = count;
    for (worker_index = worker_count; worker_index != 0U; --worker_index) {
        size_t index = worker_index - 1U;
        size_t begin = count * index / worker_count;
        size_t end = count * worker_index / worker_count;
        AvifdecStatus status = body(begin, end, index, arg);

        if (first_status == AVIFDEC_OK && status != AVIFDEC_OK) {
            first_status = status;
        }
    }
    return first_status;
}

static void fuzz_prepare_image(
    const AvifdecImageInfo *info, AvifdecImage *image) {
    avifdec_memory_fill(fuzz_y, 0xa5U, sizeof(fuzz_y));
    avifdec_memory_fill(fuzz_u, 0xa5U, sizeof(fuzz_u));
    avifdec_memory_fill(fuzz_v, 0xa5U, sizeof(fuzz_v));
    avifdec_memory_fill(fuzz_alpha, 0xa5U, sizeof(fuzz_alpha));
    avifdec_memory_fill(image, 0U, sizeof(*image));
    image->planes[0] = fuzz_y;
    image->strides[0] = FUZZ_MAX_DIMENSION;
    if (!info->monochrome) {
        image->planes[1] = fuzz_u;
        image->planes[2] = fuzz_v;
        image->strides[1] = FUZZ_MAX_DIMENSION;
        image->strides[2] = FUZZ_MAX_DIMENSION;
    }
    if (info->has_alpha) {
        image->alpha_plane = fuzz_alpha;
        image->alpha_stride = FUZZ_MAX_DIMENSION;
    }
}

static void fuzz_require(int condition) {
    if (!condition) __builtin_trap();
}

static uint64_t fuzz_hash_bytes(
    uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t index;

    for (index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t fuzz_hash_image(const AvifdecImage *image) {
    uint64_t hash = 1469598103934665603ULL;
    unsigned int plane_count = image->monochrome ? 1U : 3U;
    unsigned int plane;

    for (plane = 0U; plane < plane_count; ++plane) {
        uint32_t row;

        hash = fuzz_hash_bytes(hash, &image->widths[plane],
                               sizeof(image->widths[plane]));
        hash = fuzz_hash_bytes(hash, &image->heights[plane],
                               sizeof(image->heights[plane]));
        for (row = 0U; row < image->heights[plane]; ++row) {
            hash = fuzz_hash_bytes(
                hash,
                image->planes[plane] + (size_t)row * image->strides[plane],
                (size_t)image->widths[plane] * sizeof(uint16_t));
        }
    }
    if (image->alpha_plane != 0) {
        uint32_t row;

        hash = fuzz_hash_bytes(hash, &image->alpha_width,
                               sizeof(image->alpha_width));
        hash = fuzz_hash_bytes(hash, &image->alpha_height,
                               sizeof(image->alpha_height));
        for (row = 0U; row < image->alpha_height; ++row) {
            hash = fuzz_hash_bytes(
                hash,
                image->alpha_plane + (size_t)row * image->alpha_stride,
                (size_t)image->alpha_width * sizeof(uint16_t));
        }
    }
    return hash;
}

static int fuzz_info_fits(const AvifdecImageInfo *info) {
    return info->width <= FUZZ_MAX_DIMENSION &&
           info->height <= FUZZ_MAX_DIMENSION &&
           info->workspace_required <= FUZZ_WORKSPACE_SIZE;
}

static void fuzz_check_still(
    const unsigned char *data,
    size_t size,
    const AvifdecLimits *limits,
    size_t workspace_offset) {
    FuzzExecutor executor_states[2] = { { 2U }, { 4U } };
    AvifdecExecutor executors[2] = {
        { &executor_states[0], 2U, fuzz_parallel_for },
        { &executor_states[1], 4U, fuzz_parallel_for }
    };
    AvifdecImageInfo serial_info;
    AvifdecEntropyTrace serial_trace;
    AvifdecImage image;
    AvifdecError error;
    AvifdecStatus serial_query;
    AvifdecStatus serial_decode;
    uint64_t serial_hash;
    size_t executor_index;

    serial_query = avifdec_query(
        data, size, limits, 0, 0U, &serial_info, &error);
    if (serial_query != AVIFDEC_OK || !fuzz_info_fits(&serial_info)) return;
    fuzz_prepare_image(&serial_info, &image);
    serial_decode = avifdec_decode(
        data, size, limits, fuzz_workspace + workspace_offset,
        serial_info.workspace_required, &image, &serial_trace, &error);
    fuzz_require(serial_decode != AVIFDEC_OUT_OF_MEMORY);
    if (serial_decode != AVIFDEC_OK) return;
    serial_hash = fuzz_hash_image(&image);
    if (serial_info.workspace_required != 0U) {
        AvifdecStatus short_status;

        fuzz_prepare_image(&serial_info, &image);
        short_status = avifdec_decode(
            data, size, limits, fuzz_workspace + workspace_offset,
            serial_info.workspace_required - 1U, &image, 0, &error);
        fuzz_require(short_status == AVIFDEC_OK ||
                     short_status == AVIFDEC_OUT_OF_MEMORY);
        if (short_status == AVIFDEC_OK) {
            fuzz_require(fuzz_hash_image(&image) == serial_hash);
        }
    }
    for (executor_index = 0U; executor_index < 2U; ++executor_index) {
        AvifdecImageInfo parallel_info;
        AvifdecEntropyTrace parallel_trace;
        AvifdecStatus parallel_query = avifdec_query_ex(
            data, size, limits, &executors[executor_index], 0, 0U,
            &parallel_info, &error);

        fuzz_require(parallel_query == AVIFDEC_OK);
        if (!fuzz_info_fits(&parallel_info)) continue;
        fuzz_prepare_image(&parallel_info, &image);
        fuzz_require(avifdec_decode_ex(
            data, size, limits, &executors[executor_index],
            fuzz_workspace + workspace_offset,
            parallel_info.workspace_required, &image,
            &parallel_trace, &error) == AVIFDEC_OK);
        fuzz_require(fuzz_hash_image(&image) == serial_hash);
        fuzz_require(avifdec_memory_compare(
            &serial_trace, &parallel_trace,
            sizeof(serial_trace)) == 0);
    }
}

static void fuzz_check_sequence(
    const unsigned char *data,
    size_t size,
    const AvifdecLimits *limits,
    size_t workspace_offset) {
    FuzzExecutor executor_state = { 4U };
    AvifdecExecutor executor = {
        &executor_state, 4U, fuzz_parallel_for
    };
    AvifdecSequenceInfo sequence;
    AvifdecFrameInfo serial_frame;
    AvifdecFrameInfo parallel_frame;
    AvifdecEntropyTrace serial_trace;
    AvifdecEntropyTrace parallel_trace;
    AvifdecImage image;
    AvifdecError error;
    AvifdecStatus serial_decode;
    uint64_t serial_hash;
    size_t frame_index;

    if (avifdec_sequence_query(
            data, size, limits, &sequence, &error) != AVIFDEC_OK ||
        sequence.frame_count == 0U) {
        return;
    }
    frame_index = size == 0U ? 0U : data[size - 1U] % sequence.frame_count;
    if (avifdec_sequence_frame_query(
            data, size, limits, frame_index,
            &serial_frame, &error) != AVIFDEC_OK ||
        !fuzz_info_fits(&serial_frame.image)) {
        return;
    }
    fuzz_prepare_image(&serial_frame.image, &image);
    serial_decode = avifdec_sequence_decode_frame(
        data, size, limits, frame_index,
        fuzz_workspace + workspace_offset,
        serial_frame.image.workspace_required, &image, &serial_trace,
        &serial_frame, &error);
    fuzz_require(serial_decode != AVIFDEC_OUT_OF_MEMORY);
    if (serial_decode != AVIFDEC_OK) return;
    serial_hash = fuzz_hash_image(&image);
    if (serial_frame.image.workspace_required != 0U) {
        AvifdecStatus short_status;

        fuzz_prepare_image(&serial_frame.image, &image);
        short_status = avifdec_sequence_decode_frame(
            data, size, limits, frame_index,
            fuzz_workspace + workspace_offset,
            serial_frame.image.workspace_required - 1U, &image, 0,
            &serial_frame, &error);
        fuzz_require(short_status == AVIFDEC_OK ||
                     short_status == AVIFDEC_OUT_OF_MEMORY);
        if (short_status == AVIFDEC_OK) {
            fuzz_require(fuzz_hash_image(&image) == serial_hash);
        }
    }
    fuzz_require(avifdec_sequence_frame_query_ex(
        data, size, limits, &executor, frame_index,
        &parallel_frame, &error) == AVIFDEC_OK);
    if (!fuzz_info_fits(&parallel_frame.image)) return;
    fuzz_prepare_image(&parallel_frame.image, &image);
    fuzz_require(avifdec_sequence_decode_frame_ex(
        data, size, limits, &executor, frame_index,
        fuzz_workspace + workspace_offset,
        parallel_frame.image.workspace_required, &image, &parallel_trace,
        &parallel_frame, &error) == AVIFDEC_OK);
    fuzz_require(fuzz_hash_image(&image) == serial_hash);
    fuzz_require(avifdec_memory_compare(
        &serial_trace, &parallel_trace, sizeof(serial_trace)) == 0);
}

static void fuzz_check_extended_apis(
    const unsigned char *data,
    size_t size,
    const AvifdecLimits *limits,
    size_t workspace_offset) {
    AvifdecMetadataResult metadata_result;
    AvifdecGainMapInfo gain_map;
    AvifdecImageInfo still_info;
    AvifdecColorDescription color;
    AvifdecColorOptions color_options;
    AvifdecColorTransformInfo color_transform;
    AvifdecSequenceIndexInfo index_info;
    AvifdecSequenceIndex index;
    AvifdecSequenceSelectOptions select_options;
    AvifdecSequenceSelection selection;
    AvifdecSequencePresentationInfo presentation;
    AvifdecEntropyTrace trace;
    AvifdecImage image;
    AvifdecError error;
    AvifdecStatus status;
    size_t track_index;

    (void)avifdec_metadata_query(
        data, size, limits, 0, 0U, 0, 0U, 0, 0U,
        &metadata_result, &error);
    (void)avifdec_gain_map_query(
        data, size, limits, &gain_map, &error);

    if (avifdec_query(
            data, size, limits, 0, 0U, &still_info, &error) ==
        AVIFDEC_OK) {
        if (avifdec_image_color_description(
                &still_info, &color, &error) == AVIFDEC_OK) {
            avifdec_color_options_default(&color_options);
            (void)avifdec_color_transform_query(
                &color, &color_options, limits,
                &color_transform, &error);
        }
    }

    status = avifdec_sequence_index_query(
        data, size, limits, &index_info, &error);
    if (status != AVIFDEC_OK ||
        index_info.workspace_required > FUZZ_INDEX_WORKSPACE_SIZE) {
        return;
    }
    if (index_info.workspace_required != 0U) {
        AvifdecSequenceIndex short_index;
        AvifdecSequenceIndexInfo short_info;

        fuzz_require(avifdec_sequence_index_init(
            data, size, limits,
            fuzz_index_workspace + workspace_offset,
            index_info.workspace_required - 1U,
            &short_index, &short_info, &error) ==
            AVIFDEC_OUT_OF_MEMORY);
        fuzz_require(short_info.workspace_required ==
                     index_info.workspace_required);
    }
    fuzz_require(avifdec_sequence_index_init(
        data, size, limits,
        fuzz_index_workspace + workspace_offset,
        index_info.workspace_required,
        &index, &index_info, &error) == AVIFDEC_OK);
    (void)avifdec_sequence_metadata_query(
        &index, 0U, 0, 0U, 0, 0U, 0, 0U,
        &metadata_result, &error);

    avifdec_memory_fill(
        &select_options, 0U, sizeof(select_options));
    for (track_index = 0U;
         track_index < index_info.track_count;
         ++track_index) {
        AvifdecSequenceTrackInfo track;

        fuzz_require(avifdec_sequence_track_query(
            &index, track_index, &track, &error) == AVIFDEC_OK);
        if (select_options.main_track_id == 0U &&
            (track.flags & AVIFDEC_SEQUENCE_TRACK_VISUAL) != 0U &&
            (track.flags & AVIFDEC_SEQUENCE_TRACK_ALPHA) == 0U) {
            select_options.main_track_id = track.track_id;
        }
    }
    if (select_options.main_track_id == 0U ||
        avifdec_sequence_select(
            &index, &select_options, &selection, &error) != AVIFDEC_OK ||
        selection.presentation_count == 0U) {
        return;
    }
    track_index = size == 0U
        ? 0U : data[size - 1U] % selection.presentation_count;
    status = avifdec_sequence_presentation_query(
        &index, &selection, track_index, &presentation, &error);
    if (status != AVIFDEC_OK ||
        !fuzz_info_fits(&presentation.image)) {
        return;
    }
    fuzz_prepare_image(&presentation.image, &image);
    status = avifdec_sequence_decode_presentation(
        &index, &selection, track_index,
        fuzz_workspace + workspace_offset,
        presentation.image.workspace_required,
        &image, &trace, &presentation, &error);
    fuzz_require(status != AVIFDEC_OUT_OF_MEMORY);
    if (status == AVIFDEC_OK) {
        (void)fuzz_hash_image(&image);
    }
}

int LLVMFuzzerTestOneInput(
    const unsigned char *data, size_t size) {
    AvifdecBmffLimits bmff_limits = { 16U, 4096U };
    AvifdecLimits limits;
    AvifdecBmffInfo bmff_info;
    AvifdecError error;
    size_t workspace_offset =
        size == 0U ? 0U : data[0] & (FUZZ_WORKSPACE_ALIGNMENT - 1U);

    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    limits.max_width = FUZZ_MAX_DIMENSION;
    limits.max_height = FUZZ_MAX_DIMENSION;
    limits.max_pixels = FUZZ_MAX_PIXELS;
    limits.max_items = 64U;
    limits.max_extents = 16U;
    limits.max_properties = 64U;
    limits.max_obus = 2048U;
    limits.max_frames = 16U;
    limits.max_metadata_items = 16U;
    limits.max_metadata_spans = 64U;
    limits.max_tracks = 8U;
    limits.max_edits = 16U;
    limits.max_fragments = 32U;
    limits.max_icc_bytes = 1024U * 1024U;
    limits.max_icc_curve_entries = 1024U;

    (void)avifdec_bmff_inspect(
        data, size, &bmff_limits, 0, 0, &bmff_info, &error);
    fuzz_check_still(data, size, &limits, workspace_offset);
    fuzz_check_sequence(data, size, &limits, workspace_offset);
    fuzz_check_extended_apis(data, size, &limits, workspace_offset);
    return 0;
}
