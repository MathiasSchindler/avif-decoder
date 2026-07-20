#include "encoder/avifenc.h"
#include "encoder/image_input.h"
#include "base.h"
#include "platform.h"
#include "task_pool.h"
#include <stddef.h>
#include <stdint.h>

#define AVIFENC_CLI_MAX_INPUT_SIZE (1024U * 1024U * 1024U)

static size_t text_length(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0') ++length;
    return length;
}

static int text_equal(const char *left, const char *right) {
    size_t index = 0U;

    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        ++index;
    }
    return left[index] == right[index];
}

static int text_to_u32(const char *text, uint32_t *value) {
    uint32_t result = 0U;
    size_t index = 0U;

    if (text == 0 || value == 0 || text[0] == '\0') return 0;
    while (text[index] != '\0') {
        uint32_t digit;

        if (text[index] < '0' || text[index] > '9') return 0;
        digit = (uint32_t)(text[index] - '0');
        if (result > (UINT32_MAX - digit) / 10U) return 0;
        result = result * 10U + digit;
        ++index;
    }
    *value = result;
    return 1;
}

static int text_to_i8(const char *text, int8_t *value) {
    uint32_t magnitude;
    int negative = text != 0 && text[0] == '-';

    if (text == 0 || value == 0 ||
        !text_to_u32(text + negative, &magnitude) ||
        magnitude > (negative ? 64U : 63U)) {
        return 0;
    }
    *value = negative ? (int8_t)-(int)magnitude : (int8_t)magnitude;
    return 1;
}

static int write_bytes(int fd, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;

    while (written < size) {
        long result = platform_write(fd, bytes + written, size - written);

        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int read_bytes(int fd, void *data, size_t size) {
    unsigned char *bytes = (unsigned char *)data;
    size_t received = 0U;

    while (received < size) {
        long result = platform_read(fd, bytes + received, size - received);

        if (result <= 0) return -1;
        received += (size_t)result;
    }
    return 0;
}

static int load_file(const char *path,
                     unsigned char **data_out,
                     size_t *size_out) {
    long long end;
    unsigned char *data;
    int fd = platform_open_read(path);

    if (fd < 0) return -1;
    end = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end <= 0 ||
        (unsigned long long)end > (unsigned long long)SIZE_MAX ||
        (unsigned long long)end > AVIFENC_CLI_MAX_INPUT_SIZE ||
        platform_seek(fd, 0, PLATFORM_SEEK_SET) != 0) {
        (void)platform_close(fd);
        return -1;
    }
    data = (unsigned char *)platform_allocate_pages((size_t)end);
    if (data == 0) {
        (void)platform_close(fd);
        return -1;
    }
    if (read_bytes(fd, data, (size_t)end) != 0) {
        (void)platform_close(fd);
        (void)platform_free_pages(data, (size_t)end);
        return -1;
    }
    if (platform_close(fd) != 0) {
        (void)platform_free_pages(data, (size_t)end);
        return -1;
    }
    *data_out = data;
    *size_out = (size_t)end;
    return 0;
}

static uint8_t color_clamp(int32_t value) {
    if (value < 0) return 0U;
    if (value > 255) return 255U;
    return (uint8_t)value;
}

static int32_t color_divide_256(int32_t value) {
    return value >= 0
        ? (value + 128) / 256
        : -((-value + 128) / 256);
}

static uint8_t color_luma(uint8_t red, uint8_t green, uint8_t blue) {
    return color_clamp(
        16 + (47 * (int32_t)red + 157 * (int32_t)green +
              16 * (int32_t)blue + 128) / 256);
}

static uint8_t color_blue_difference(uint8_t red,
                                     uint8_t green,
                                     uint8_t blue) {
    return color_clamp(
        128 + color_divide_256(
            -26 * (int32_t)red - 87 * (int32_t)green +
            112 * (int32_t)blue));
}

static uint8_t color_red_difference(uint8_t red,
                                    uint8_t green,
                                    uint8_t blue) {
    return color_clamp(
        128 + color_divide_256(
            112 * (int32_t)red - 102 * (int32_t)green -
            10 * (int32_t)blue));
}

static int image_dimensions_supported(uint32_t width, uint32_t height) {
    return width != 0U && height != 0U &&
        (width & 1U) == 0U && (height & 1U) == 0U &&
        width <= AVIFENC_MAX_DIMENSION &&
        height <= AVIFENC_MAX_DIMENSION;
}

static void image_fit_dimensions(uint32_t source_width,
                                 uint32_t source_height,
                                 uint32_t *width_out,
                                 uint32_t *height_out) {
    uint32_t width = (source_width + 1U) & ~1U;
    uint32_t height = (source_height + 1U) & ~1U;

    if (!image_dimensions_supported(width, height)) width = height = 0U;
    *width_out = width;
    *height_out = height;
}

static uint32_t image_source_coordinate(uint32_t output_coordinate,
                                        uint32_t output_size,
                                        uint32_t source_size) {
    if (output_size == source_size || output_size == source_size + 1U) {
        return output_coordinate < source_size
            ? output_coordinate : source_size - 1U;
    }
    return (uint32_t)(
        ((uint64_t)(output_coordinate * 2U + 1U) * source_size) /
        ((uint64_t)output_size * 2U));
}

static void rgb_to_yuv420(const uint8_t *rgb,
                          const ImageInputInfo *source,
                          AvifencImage *image,
                          unsigned char *yuv,
                          size_t luma_size,
                          size_t chroma_size) {
    unsigned char *luma = yuv;
    unsigned char *blue_difference = yuv + luma_size;
    unsigned char *red_difference = blue_difference + chroma_size;
    uint32_t output_y;

    for (output_y = 0U; output_y < image->height; ++output_y) {
        uint32_t source_y = image_source_coordinate(
            output_y, image->height, source->height);
        uint32_t output_x;

        for (output_x = 0U; output_x < image->width; ++output_x) {
            uint32_t source_x = image_source_coordinate(
                output_x, image->width, source->width);
            const uint8_t *pixel = rgb + (size_t)source_y *
                source->rgb_stride + (size_t)source_x * 3U;

            luma[(size_t)output_y * image->width + output_x] =
                color_luma(pixel[0], pixel[1], pixel[2]);
        }
    }
    for (output_y = 0U; output_y < image->height / 2U; ++output_y) {
        uint32_t output_x;

        for (output_x = 0U; output_x < image->width / 2U; ++output_x) {
            uint32_t red = 0U;
            uint32_t green = 0U;
            uint32_t blue = 0U;
            unsigned int offset_y;

            for (offset_y = 0U; offset_y < 2U; ++offset_y) {
                uint32_t source_y = image_source_coordinate(
                    output_y * 2U + offset_y,
                    image->height, source->height);
                unsigned int offset_x;

                for (offset_x = 0U; offset_x < 2U; ++offset_x) {
                    uint32_t source_x = image_source_coordinate(
                        output_x * 2U + offset_x,
                        image->width, source->width);
                    const uint8_t *pixel;

                    pixel = rgb + (size_t)source_y * source->rgb_stride +
                        (size_t)source_x * 3U;
                    red += pixel[0];
                    green += pixel[1];
                    blue += pixel[2];
                }
            }
            red = (red + 2U) / 4U;
            green = (green + 2U) / 4U;
            blue = (blue + 2U) / 4U;
            blue_difference[(size_t)output_y * (image->width / 2U) +
                            output_x] = color_blue_difference(
                                (uint8_t)red, (uint8_t)green, (uint8_t)blue);
            red_difference[(size_t)output_y * (image->width / 2U) +
                           output_x] = color_red_difference(
                               (uint8_t)red, (uint8_t)green, (uint8_t)blue);
        }
    }
}

static int write_text(int fd, const char *text) {
    return write_bytes(fd, text, text_length(text));
}

typedef struct {
    AvifencParallelBody body;
    void *arg;
} EncoderCliParallelCall;

static int encoder_cli_parallel_body(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg) {
    EncoderCliParallelCall *call = (EncoderCliParallelCall *)arg;

    return (int)call->body(begin, end, worker_index, call->arg);
}

static AvifencStatus encoder_cli_parallel_for(
    void *user_data,
    size_t count,
    size_t min_chunk,
    AvifencParallelBody body,
    void *arg) {
    EncoderCliParallelCall call;
    int status;

    if (user_data == 0 || body == 0) return AVIFENC_INVALID_ARGUMENT;
    call.body = body;
    call.arg = arg;
    status = rt_parallel_for(
        (RtTaskPool *)user_data, count, min_chunk,
        encoder_cli_parallel_body, &call);
    if (status < (int)AVIFENC_OK || status > (int)AVIFENC_UNSUPPORTED) {
        return AVIFENC_INVALID_ARGUMENT;
    }
    return (AvifencStatus)status;
}

static void write_usage(int fd) {
    (void)write_text(
        fd,
        "usage: avifenc [--quantizer 0..255] [--speed 0..2] [--workers 1..32] "
        "[--target-quality 0..10000|--target-size BYTES] "
        "[--qmatrix off|auto|LEVEL] [--aq off|activity] "
        "[--aq-strength 0..63] [--y-dc-delta N] [--u-dc-delta N] "
        "[--u-ac-delta N] [--v-dc-delta N] [--v-ac-delta N] "
        "WIDTH HEIGHT INPUT.yuv OUTPUT.avif\n"
        "       avifenc [--quantizer 0..255] [--speed 0..2] [--workers 1..32] "
        "[--target-quality 0..10000|--target-size BYTES] "
        "[--qmatrix off|auto|LEVEL] [--aq off|activity] "
        "[--aq-strength 0..63] [--y-dc-delta N] [--u-dc-delta N] "
        "[--u-ac-delta N] [--v-dc-delta N] [--v-ac-delta N] "
        "INPUT.png|jpg|jpeg OUTPUT.avif\n"
        "       avifenc --help\n"
        "       avifenc --version\n");
}

static int write_error(AvifencStatus status, const AvifencError *error) {
    if (write_text(2, "avifenc: ") != 0 ||
        write_text(2, avifenc_status_string(status)) != 0 ||
        write_text(2, ": ") != 0 ||
        write_text(2, avifenc_error_context_string(error->context)) != 0 ||
        write_text(2, "\n") != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    static const uint8_t placeholder = 0U;
    AvifencImage image = { 0 };
    AvifencOptions options;
    AvifencRequirements requirements;
    AvifencError error;
    AvifencStatus status;
    unsigned char *input = 0;
    unsigned char *source_file = 0;
    unsigned char *rgb = 0;
    void *image_workspace = 0;
    void *workspace = 0;
    unsigned char *output = 0;
    size_t luma_size;
    size_t chroma_size;
    size_t input_size = 0U;
    size_t source_file_size = 0U;
    size_t rgb_size = 0U;
    size_t image_workspace_size = 0U;
    size_t output_written = 0U;
    uint32_t quantizer = AVIFENC_DEFAULT_QUANTIZER;
    uint32_t speed = AVIFENC_DEFAULT_SPEED;
    uint32_t requested_workers = 1U;
    uint32_t target_quality = 0U;
    uint32_t target_size = 0U;
    uint32_t matrix_level = 0U;
    uint32_t aq_strength = 8U;
    int8_t quant_deltas[5] = { 0 };
    uint8_t rate_mode = 0U;
    uint8_t matrix_mode = 0U;
    uint8_t aq_mode = 0U;
    RtTaskPool task_pool;
    AvifencExecutor executor;
    const AvifencExecutor *encode_executor = 0;
    int task_pool_initialized = 0;
    int input_fd = -1;
    int output_fd = -1;
    const char *output_path;
    int argument = 1;
    int image_file_input = 0;
    int result = 3;

    if (argc == 2 && text_equal(argv[1], "--help")) {
        write_usage(1);
        return 0;
    }
    if (argc == 2 && text_equal(argv[1], "--version")) {
        (void)write_text(1, "avifenc ");
        (void)write_text(1, avifenc_version_string());
        (void)write_text(1, "\n");
        return 0;
    }
    while (argument + 1 < argc) {
        if (text_equal(argv[argument], "--quantizer")) {
            if (!text_to_u32(argv[argument + 1], &quantizer) ||
                quantizer > UINT16_MAX) {
                (void)write_text(2, "avifenc: invalid quantizer\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--speed")) {
            if (!text_to_u32(argv[argument + 1], &speed) ||
                speed > UINT8_MAX) {
                (void)write_text(2, "avifenc: invalid speed\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--workers")) {
            if (!text_to_u32(argv[argument + 1], &requested_workers) ||
                requested_workers == 0U ||
                requested_workers > AVIFENC_EXECUTOR_MAX_WORKERS) {
                (void)write_text(2, "avifenc: invalid worker count\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--target-quality")) {
            if (rate_mode == 2U ||
                !text_to_u32(argv[argument + 1], &target_quality) ||
                target_quality > AVIFENC_TARGET_QUALITY_MAX) {
                (void)write_text(2, "avifenc: invalid target quality\n");
                return 2;
            }
            rate_mode = 1U;
        } else if (text_equal(argv[argument], "--target-size")) {
            if (rate_mode == 1U ||
                !text_to_u32(argv[argument + 1], &target_size) ||
                target_size == 0U) {
                (void)write_text(2, "avifenc: invalid target size\n");
                return 2;
            }
            rate_mode = 2U;
        } else if (text_equal(argv[argument], "--qmatrix")) {
            if (text_equal(argv[argument + 1], "off")) {
                matrix_mode = 0U;
            } else if (text_equal(argv[argument + 1], "auto")) {
                matrix_mode = 2U;
            } else if (text_to_u32(argv[argument + 1], &matrix_level) &&
                       matrix_level <= 14U) {
                matrix_mode = 1U;
            } else {
                (void)write_text(2, "avifenc: invalid qmatrix\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--aq")) {
            if (text_equal(argv[argument + 1], "off")) aq_mode = 0U;
            else if (text_equal(argv[argument + 1], "activity")) aq_mode = 1U;
            else {
                (void)write_text(2, "avifenc: invalid aq mode\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--aq-strength")) {
            if (!text_to_u32(argv[argument + 1], &aq_strength) ||
                aq_strength > 63U) {
                (void)write_text(2, "avifenc: invalid aq strength\n");
                return 2;
            }
        } else if (text_equal(argv[argument], "--y-dc-delta")) {
            if (!text_to_i8(argv[argument + 1], &quant_deltas[0])) return 2;
        } else if (text_equal(argv[argument], "--u-dc-delta")) {
            if (!text_to_i8(argv[argument + 1], &quant_deltas[1])) return 2;
        } else if (text_equal(argv[argument], "--u-ac-delta")) {
            if (!text_to_i8(argv[argument + 1], &quant_deltas[2])) return 2;
        } else if (text_equal(argv[argument], "--v-dc-delta")) {
            if (!text_to_i8(argv[argument + 1], &quant_deltas[3])) return 2;
        } else if (text_equal(argv[argument], "--v-ac-delta")) {
            if (!text_to_i8(argv[argument + 1], &quant_deltas[4])) return 2;
        } else {
            break;
        }
        argument += 2;
    }
    if (argc - argument == 4 &&
        text_to_u32(argv[argument], &image.width) &&
        text_to_u32(argv[argument + 1], &image.height)) {
        output_path = argv[argument + 3];
    } else if (argc - argument == 2) {
        ImageInputInfo source_info;
        ImageInputStatus image_status;

        image_file_input = 1;
        output_path = argv[argument + 1];
        if (load_file(argv[argument], &source_file, &source_file_size) != 0) {
            (void)write_text(2, "avifenc: failed to read image input\n");
            result = 4;
            goto cleanup;
        }
        image_status = image_input_query(
            source_file, source_file_size, &source_info);
        if (image_status != IMAGE_INPUT_OK) {
            (void)write_text(2, "avifenc: image input: ");
            (void)write_text(2, image_input_status_string(image_status));
            (void)write_text(2, "\n");
            result = 4;
            goto cleanup;
        }
        if (source_info.width > AVIFENC_MAX_DIMENSION ||
            source_info.height > AVIFENC_MAX_DIMENSION ||
            (source_info.width == AVIFENC_MAX_DIMENSION &&
             (source_info.width & 1U) != 0U) ||
            (source_info.height == AVIFENC_MAX_DIMENSION &&
             (source_info.height & 1U) != 0U)) {
            (void)write_text(2, "avifenc: image dimensions exceed limit\n");
            result = 2;
            goto cleanup;
        }
        image_fit_dimensions(
            source_info.width, source_info.height,
            &image.width, &image.height);
        if (image.width == 0U || image.height == 0U) {
            (void)write_text(2, "avifenc: image dimensions exceed limit\n");
            result = 2;
            goto cleanup;
        }
        rgb_size = source_info.output_size;
        image_workspace_size = source_info.workspace_size;
        rgb = (unsigned char *)platform_allocate_pages(rgb_size);
        image_workspace = platform_allocate_pages(image_workspace_size);
        if (rgb == 0 || image_workspace == 0) {
            (void)write_text(2, "avifenc: out of memory: image decode\n");
            goto cleanup;
        }
        image_status = image_input_decode(
            source_file, source_file_size,
            image_workspace, image_workspace_size,
            rgb, rgb_size, &source_info);
        if (image_status != IMAGE_INPUT_OK) {
            (void)write_text(2, "avifenc: image input: ");
            (void)write_text(2, image_input_status_string(image_status));
            (void)write_text(2, "\n");
            result = 4;
            goto cleanup;
        }
    } else {
        write_usage(2);
        return 2;
    }

    image.planes[0] = &placeholder;
    image.planes[1] = &placeholder;
    image.planes[2] = &placeholder;
    image.strides[0] = image.width;
    image.strides[1] = image.width / 2U;
    image.strides[2] = image.width / 2U;
    image.color.color_primaries = 1U;
    image.color.transfer_characteristics = 1U;
    image.color.matrix_coefficients = 1U;
    avifenc_options_default(&options);
    options.quantizer = (uint16_t)quantizer;
    options.speed = (uint8_t)speed;
    options.quantization.delta_q_y_dc = quant_deltas[0];
    options.quantization.delta_q_u_dc = quant_deltas[1];
    options.quantization.delta_q_u_ac = quant_deltas[2];
    options.quantization.delta_q_v_dc = quant_deltas[3];
    options.quantization.delta_q_v_ac = quant_deltas[4];
    options.quantization.matrix_mode = matrix_mode;
    options.quantization.matrix_levels[0] = (uint8_t)matrix_level;
    options.quantization.matrix_levels[1] = (uint8_t)matrix_level;
    options.quantization.matrix_levels[2] = (uint8_t)matrix_level;
    options.quantization.adaptive_quantization = aq_mode;
    options.quantization.aq_strength = (uint8_t)aq_strength;
    options.rate_control.mode = rate_mode;
    options.rate_control.target_quality = (uint16_t)target_quality;
    options.rate_control.target_size = target_size;
    if (requested_workers > 1U) {
        if (rt_task_pool_init(&task_pool, requested_workers) != 0) {
            (void)write_text(2, "avifenc: failed to initialize workers\n");
            goto cleanup;
        }
        task_pool_initialized = 1;
        executor.user_data = &task_pool;
        executor.worker_count = rt_task_pool_width(&task_pool);
        executor.parallel_for = encoder_cli_parallel_for;
        encode_executor = &executor;
    }
    status = avifenc_query_with_executor(
        &image, &options, encode_executor, &requirements, &error);
    if (status != AVIFENC_OK) {
        (void)write_error(status, &error);
        result = 2;
        goto cleanup;
    }
    if (!avifdec_size_multiply(image.width, image.height, &luma_size) ||
        !avifdec_size_multiply(
            image.width / 2U, image.height / 2U, &chroma_size) ||
        chroma_size > (SIZE_MAX - luma_size) / 2U) {
        (void)write_text(2, "avifenc: input dimensions overflow\n");
        return 2;
    }
    input_size = luma_size + 2U * chroma_size;
    if (input_size == 0U) {
        (void)write_text(2, "avifenc: invalid dimensions\n");
        return 2;
    }
    input = (unsigned char *)platform_allocate_pages(input_size);
    if (input == 0) {
        (void)write_text(2, "avifenc: out of memory: input\n");
        return 3;
    }
    if (image_file_input) {
        ImageInputInfo source_info;
        ImageInputStatus image_status = image_input_query(
            source_file, source_file_size, &source_info);

        if (image_status != IMAGE_INPUT_OK) {
            result = 4;
            goto cleanup;
        }
        rgb_to_yuv420(
            rgb, &source_info, &image, input, luma_size, chroma_size);
    } else {
        input_fd = platform_open_read(argv[argument + 2]);
        if (input_fd < 0 || read_bytes(input_fd, input, input_size) != 0) {
            (void)write_text(2, "avifenc: failed to read complete YUV input\n");
            result = 4;
            goto cleanup;
        }
        {
            unsigned char trailing;
            long extra = platform_read(input_fd, &trailing, 1U);

            if (extra != 0) {
                (void)write_text(2, "avifenc: YUV input has trailing data\n");
                result = 4;
                goto cleanup;
            }
        }
    }
    if (!image_file_input && platform_close(input_fd) != 0) {
        input_fd = -1;
        (void)write_text(2, "avifenc: failed to close YUV input\n");
        result = 4;
        goto cleanup;
    }
    input_fd = -1;
    image.planes[0] = input;
    image.planes[1] = input + luma_size;
    image.planes[2] = input + luma_size + chroma_size;
    workspace = platform_allocate_pages(requirements.workspace_required);
    output = (unsigned char *)platform_allocate_pages(
        requirements.output_capacity_required);
    if (workspace == 0 || output == 0) {
        (void)write_text(2, "avifenc: out of memory: encode buffers\n");
        goto cleanup;
    }
    status = avifenc_encode_with_executor(
        &image, &options, encode_executor,
        workspace, requirements.workspace_required,
        output, requirements.output_capacity_required,
        &output_written, 0, &error);
    if (status != AVIFENC_OK) {
        (void)write_error(status, &error);
        goto cleanup;
    }
    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0 || write_bytes(output_fd, output, output_written) != 0) {
        (void)write_text(2, "avifenc: failed to write AVIF output\n");
        result = 4;
        goto cleanup;
    }
    if (platform_close(output_fd) != 0) {
        output_fd = -1;
        (void)write_text(2, "avifenc: failed to close AVIF output\n");
        result = 4;
        goto cleanup;
    }
    output_fd = -1;
    result = 0;

cleanup:
    if (task_pool_initialized) rt_task_pool_destroy(&task_pool);
    if (input_fd >= 0) (void)platform_close(input_fd);
    if (output_fd >= 0) (void)platform_close(output_fd);
    if (output != 0) {
        (void)platform_free_pages(
            output, requirements.output_capacity_required);
    }
    if (workspace != 0) {
        (void)platform_free_pages(
            workspace, requirements.workspace_required);
    }
    if (image_workspace != 0) {
        (void)platform_free_pages(image_workspace, image_workspace_size);
    }
    if (rgb != 0) (void)platform_free_pages(rgb, rgb_size);
    if (source_file != 0) {
        (void)platform_free_pages(source_file, source_file_size);
    }
    if (input != 0) (void)platform_free_pages(input, input_size);
    return result;
}