#ifndef AVIFDEC_AVIF_COLOR_H
#define AVIFDEC_AVIF_COLOR_H

#include "avifdec.h"

/*
 * These declarations mirror the additive 1.4 public API.  They are private
 * while this leaf module is built against the 1.3 public header; the version
 * guard makes the integration change a deletion rather than a type rewrite.
 */
#if AVIFDEC_VERSION_MAJOR == 1U && AVIFDEC_VERSION_MINOR < 4U
#define AVIFDEC_DEFAULT_MAX_ICC_BYTES 16777216U
#define AVIFDEC_DEFAULT_MAX_ICC_CURVE_ENTRIES 4096U

#define AVIFDEC_RGBF32 4U
#define AVIFDEC_RGBAF32 5U

typedef struct {
    const unsigned char *data;
    size_t size;
} AvifdecByteView;

typedef struct {
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
    uint8_t has_nclx;
    AvifdecByteView icc;
} AvifdecColorDescription;

typedef enum {
    AVIFDEC_COLOR_SOURCE_AUTO = 0,
    AVIFDEC_COLOR_SOURCE_CICP = 1,
    AVIFDEC_COLOR_SOURCE_ICC = 2
} AvifdecColorSource;

typedef enum {
    AVIFDEC_COLOR_INTENT_RELATIVE_COLORIMETRIC = 0,
    AVIFDEC_COLOR_INTENT_ABSOLUTE_COLORIMETRIC = 1
} AvifdecColorIntent;

typedef enum {
    AVIFDEC_CHROMA_UPSAMPLING_BILINEAR = 0,
    AVIFDEC_CHROMA_UPSAMPLING_NEAREST = 1
} AvifdecChromaUpsampling;

typedef enum {
    AVIFDEC_COLOR_HDR_REJECT = 0,
    AVIFDEC_COLOR_HDR_PRESERVE_RELATIVE = 1,
    AVIFDEC_COLOR_HDR_CLIP_TO_DISPLAY = 2
} AvifdecColorHdrPolicy;

typedef struct {
    uint16_t destination_color_primaries;
    uint16_t destination_transfer_characteristics;
    AvifdecColorSource source;
    AvifdecColorIntent intent;
    AvifdecChromaUpsampling chroma_upsampling;
    AvifdecColorHdrPolicy hdr_policy;
    float reference_white_nits;
    float display_peak_nits;
} AvifdecColorOptions;

typedef struct {
    size_t workspace_required;
    AvifdecColorDescription source;
    AvifdecColorDescription destination;
    uint32_t flags;
} AvifdecColorTransformInfo;

#define AVIFDEC_COLOR_TRANSFORM_WORDS 64U
typedef struct {
    uintptr_t opaque[AVIFDEC_COLOR_TRANSFORM_WORDS];
} AvifdecColorTransform;
#endif

#define AVIFDEC_COLOR_TRANSFORM_SOURCE_CICP ((uint32_t)1U << 0)
#define AVIFDEC_COLOR_TRANSFORM_SOURCE_ICC ((uint32_t)1U << 1)
#define AVIFDEC_COLOR_TRANSFORM_SOURCE_GRAY ((uint32_t)1U << 2)
#define AVIFDEC_COLOR_TRANSFORM_HDR ((uint32_t)1U << 3)
#define AVIFDEC_COLOR_TRANSFORM_ABSOLUTE ((uint32_t)1U << 4)

/*
 * For 4:2:0 the private engine treats AV1's explicitly unknown position as a
 * centred fallback.  AV1 "vertical" is horizontally co-sited and vertically
 * centred; "colocated" is co-sited on both axes.  AV1 does not signal a
 * position for 4:2:2, whose horizontal samples are treated as co-sited;
 * 4:4:4 needs no siting.  Nearest sampling resolves exact ties toward the
 * lower sample, while bilinear sampling clamps odd-image edges.
 *
 * HDR-preserving float output is display-linear with 1.0 == 203 cd/m2.
 * PQ therefore retains headroom through 10000/203.  HLG signal 0.75 maps to
 * reference_white_nits/203 using the BT.2100 display OOTF derived from
 * display_peak_nits.  CLIP_TO_DISPLAY additionally clamps final destination
 * channels to display_peak_nits/203.
 */
typedef struct {
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    uint8_t color_range;
} AvifColorCicp;

typedef struct {
    AvifColorCicp cicp;
    uint8_t bit_depth;
    uint8_t monochrome;
    uint8_t subsampling_x;
    uint8_t subsampling_y;
    uint8_t chroma_sample_position;
} AvifColorAv1Cicp;

void avifdec_color_options_default(AvifdecColorOptions *options);

AvifdecStatus avifdec_image_color_description(
    const AvifdecImageInfo *info,
    AvifdecColorDescription *description,
    AvifdecError *error);

AvifdecStatus avifdec_color_transform_query(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    AvifdecColorTransformInfo *info,
    AvifdecError *error);

AvifdecStatus avifdec_color_transform_init(
    const AvifdecColorDescription *source,
    const AvifdecColorOptions *options,
    const AvifdecLimits *limits,
    void *workspace,
    size_t workspace_size,
    AvifdecColorTransform *transform,
    AvifdecError *error);

AvifdecStatus avifdec_image_to_rgb_with_transform(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    AvifdecRgbImage *rgb,
    AvifdecError *error);

AvifdecStatus avifdec_image_to_rgb_row_with_transform(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    AvifdecRgbImage *rgb,
    uint32_t row,
    AvifdecError *error);

AvifdecStatus avif_color_validate_nclx_av1(
    const AvifColorCicp *nclx,
    const AvifColorAv1Cicp *av1,
    AvifdecError *error);

AvifdecStatus avif_color_transfer_to_linear(
    uint16_t transfer_characteristics,
    double encoded,
    double *linear);

AvifdecStatus avif_color_transfer_from_linear(
    uint16_t transfer_characteristics,
    double linear,
    double *encoded);

AvifdecStatus avif_color_primaries_conversion(
    uint16_t source_primaries,
    uint16_t destination_primaries,
    AvifdecColorIntent intent,
    double matrix[9]);

/*
 * Narrow gain-map bridge.  The linear pixel helper returns straight
 * destination-primary RGBA (1.0 == 203 cd/m2).  For an alternate working
 * colour, init_source_to_working clones the output transform's intent/HDR and
 * destination policy and retained ICC byte/curve limits (using AUTO for the
 * independent base source);
 * image_pixel_to_working then joins both transforms through
 * relative-D50 or absolute XYZ.  This also maps into ICC matrix/TRC device-
 * linear RGB rather than merely using the ICC description's CICP primaries.
 * The working output helpers apply only the immutable linear primary/ICC
 * matrix and final encoding stages; they never reapply a source TRC.
 * validate_working additionally requires explicit output primaries/transfer.
 */
AvifdecStatus avif_color_transform_validate_working(
    const AvifdecColorTransform *transform,
    const AvifdecColorDescription *working,
    uint8_t output_format,
    AvifdecError *error);

AvifdecStatus avif_color_image_pixel_to_linear(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *transform,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error);

AvifdecStatus avif_color_transform_init_source_to_working(
    const AvifdecColorDescription *source,
    const AvifdecColorTransform *working_to_output,
    AvifdecColorTransform *source_transform,
    AvifdecError *error);

AvifdecStatus avif_color_image_pixel_to_working(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *source_transform,
    const AvifdecColorTransform *working_to_output,
    uint32_t x,
    uint32_t y,
    float rgba[4],
    AvifdecError *error);

AvifdecStatus avif_color_gain_map_texel(
    const AvifdecImage *image,
    const AvifdecImageInfo *info,
    const AvifdecColorTransform *source_transform,
    uint32_t x,
    uint32_t y,
    uint8_t channel_count,
    float gain[3],
    AvifdecError *error);

AvifdecStatus avif_color_transform_linear_to_linear(
    const AvifdecColorTransform *transform,
    const float working_rgb[3],
    float output_rgb[3],
    AvifdecError *error);

AvifdecStatus avif_color_transform_linear_to_encoded16(
    const AvifdecColorTransform *transform,
    const float working_rgb[3],
    uint16_t output_rgb[3],
    AvifdecError *error);

#endif
