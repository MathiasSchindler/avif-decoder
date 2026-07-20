#include "encoder/cli/image_input.h"

#include "encoder/cli/image_input_jpeg.h"
#include "encoder/cli/image_input_png.h"

#include "base.h"

const char *image_input_status_string(ImageInputStatus status) {
    switch (status) {
        case IMAGE_INPUT_OK: return "ok";
        case IMAGE_INPUT_INVALID_ARGUMENT: return "invalid argument";
        case IMAGE_INPUT_TRUNCATED: return "truncated input";
        case IMAGE_INPUT_INVALID_DATA: return "invalid image data";
        case IMAGE_INPUT_OVERFLOW: return "integer overflow";
        case IMAGE_INPUT_LIMIT_EXCEEDED: return "image limit exceeded";
        case IMAGE_INPUT_WORKSPACE_TOO_SMALL: return "workspace too small";
        case IMAGE_INPUT_OUTPUT_TOO_SMALL: return "output too small";
        case IMAGE_INPUT_UNSUPPORTED: return "unsupported image feature";
    }
    return "unknown image input error";
}

ImageInputStatus image_input_query(const void *input,
                                   size_t input_size,
                                   ImageInputInfo *info) {
    static const uint8_t png_signature[IMAGE_INPUT_PNG_SIGNATURE_SIZE] = {
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U
    };
    const uint8_t *bytes = (const uint8_t *)input;

    if (info == 0 || (input == 0 && input_size != 0U)) {
        return IMAGE_INPUT_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(info, 0U, sizeof(*info));
    if (input_size >= IMAGE_INPUT_PNG_SIGNATURE_SIZE &&
        avifdec_memory_compare(bytes, png_signature,
                               IMAGE_INPUT_PNG_SIGNATURE_SIZE) == 0) {
        return image_input_png_query(bytes, input_size, info);
    }
    if (input_size >= 2U && bytes[0] == 0xffU && bytes[1] == 0xd8U) {
        return image_input_jpeg_query(bytes, input_size, info);
    }
    return input_size < 2U ? IMAGE_INPUT_TRUNCATED
                           : IMAGE_INPUT_UNSUPPORTED;
}

ImageInputStatus image_input_decode(const void *input,
                                    size_t input_size,
                                    void *workspace,
                                    size_t workspace_size,
                                    void *output,
                                    size_t output_capacity,
                                    ImageInputInfo *info) {
    ImageInputInfo queried;
    ImageInputStatus status;

    if (info == 0 || (input == 0 && input_size != 0U) ||
        (workspace == 0 && workspace_size != 0U) ||
        (output == 0 && output_capacity != 0U)) {
        return IMAGE_INPUT_INVALID_ARGUMENT;
    }
    avifdec_memory_fill(info, 0U, sizeof(*info));
    status = image_input_query(input, input_size, &queried);
    if (status != IMAGE_INPUT_OK) return status;
    if (workspace == 0) return IMAGE_INPUT_WORKSPACE_TOO_SMALL;
    if (output == 0) return IMAGE_INPUT_OUTPUT_TOO_SMALL;
    if (queried.format == IMAGE_INPUT_FORMAT_PNG) {
        return image_input_png_decode(input, input_size, workspace,
                                      workspace_size, output, output_capacity,
                                      info);
    }
    return image_input_jpeg_decode(input, input_size, workspace, workspace_size,
                                   output, output_capacity, info);
}
