#include "avifdec.h"
#include "bmff.h"

#define FUZZ_MAX_DIMENSION 256U
#define FUZZ_MAX_PIXELS \
    ((size_t)FUZZ_MAX_DIMENSION * FUZZ_MAX_DIMENSION)
#define FUZZ_WORKSPACE_SIZE (32U * 1024U * 1024U)

static unsigned char fuzz_workspace[FUZZ_WORKSPACE_SIZE];
static uint16_t fuzz_y[FUZZ_MAX_PIXELS];
static uint16_t fuzz_u[FUZZ_MAX_PIXELS];
static uint16_t fuzz_v[FUZZ_MAX_PIXELS];
static uint16_t fuzz_alpha[FUZZ_MAX_PIXELS];

static AvifdecStatus fuzz_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifdecParallelBody body,
    void *arg) {
    size_t split;
    AvifdecStatus tail_status;
    AvifdecStatus head_status;

    (void)user_data;
    (void)min_chunk;
    if (count <= 1U) return body(0U, count, 0U, arg);
    split = count / 2U;
    tail_status = body(split, count, 3U, arg);
    head_status = body(0U, split, 0U, arg);
    return tail_status != AVIFDEC_OK ? tail_status : head_status;
}

static void fuzz_prepare_image(
    const AvifdecImageInfo *info, AvifdecImage *image) {
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

int LLVMFuzzerTestOneInput(
    const unsigned char *data, size_t size) {
    AvifdecBmffLimits bmff_limits = { 16U, 4096U };
    AvifdecLimits limits;
    AvifdecBmffInfo bmff_info;
    AvifdecImageInfo info;
    AvifdecSequenceInfo sequence;
    AvifdecFrameInfo frame;
    AvifdecEntropyTrace trace;
    AvifdecImage image;
    AvifdecError error;
    const AvifdecExecutor executor = {
        0, 4U, fuzz_parallel_for
    };

    avifdec_memory_fill(&limits, 0U, sizeof(limits));
    limits.max_width = FUZZ_MAX_DIMENSION;
    limits.max_height = FUZZ_MAX_DIMENSION;
    limits.max_pixels = FUZZ_MAX_PIXELS;
    limits.max_items = 64U;
    limits.max_extents = 16U;
    limits.max_properties = 64U;
    limits.max_obus = 2048U;
    limits.max_frames = 16U;

    (void)avifdec_bmff_inspect(
        data, size, &bmff_limits, 0, 0, &bmff_info, &error);
    if (avifdec_query_ex(
            data, size, &limits, &executor, 0, 0U,
            &info, &error) ==
            AVIFDEC_OK &&
        info.width <= FUZZ_MAX_DIMENSION &&
        info.height <= FUZZ_MAX_DIMENSION &&
        info.workspace_required <= FUZZ_WORKSPACE_SIZE) {
        fuzz_prepare_image(&info, &image);
        (void)avifdec_decode_ex(
            data, size, &limits, &executor, fuzz_workspace,
            info.workspace_required, &image, &trace, &error);
    }
    if (avifdec_sequence_query(
            data, size, &limits, &sequence, &error) ==
            AVIFDEC_OK &&
        sequence.frame_count != 0U) {
        size_t frame_index = data[size - 1U] % sequence.frame_count;

        if (avifdec_sequence_frame_query(
                data, size, &limits, frame_index, &frame, &error) ==
                AVIFDEC_OK &&
            frame.image.width <= FUZZ_MAX_DIMENSION &&
            frame.image.height <= FUZZ_MAX_DIMENSION &&
            frame.image.workspace_required <= FUZZ_WORKSPACE_SIZE) {
            fuzz_prepare_image(&frame.image, &image);
            (void)avifdec_sequence_decode_frame(
                data, size, &limits, frame_index, fuzz_workspace,
                frame.image.workspace_required, &image, &trace,
                &frame, &error);
        }
    }
    return 0;
}
