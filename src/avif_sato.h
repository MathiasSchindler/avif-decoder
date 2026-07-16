#ifndef AVIFDEC_AVIF_SATO_H
#define AVIFDEC_AVIF_SATO_H

#include "avifdec.h"

#define AVIF_SATO_MAX_INPUTS 32U
#define AVIF_SATO_MAX_TOKENS 255U
#define AVIF_SATO_MAX_STACK_DEPTH 128U
#define AVIF_SATO_MAX_CONSTANTS 128U

/*
 * AvifdecError.context is zero for header errors, token_index + 1 for token
 * errors, and one of the following values for whole-expression errors.
 */
#define AVIF_SATO_ERROR_TRAILING 256U
#define AVIF_SATO_ERROR_EXPRESSION 257U

typedef struct {
    int64_t constants[AVIF_SATO_MAX_CONSTANTS];
    uint8_t tokens[AVIF_SATO_MAX_TOKENS];
    uint8_t bit_depth_code;
    uint8_t intermediate_bits;
    uint8_t token_count;
    uint8_t constant_count;
    uint8_t required_input_count;
    uint8_t max_stack_depth;
} AvifSatoProgram;

AvifdecStatus avif_sato_parse(AvifSatoProgram *program,
                              const AvifdecSpan *spans,
                              size_t span_count,
                              size_t input_count,
                              AvifdecError *error);

AvifdecStatus avif_sato_evaluate(const AvifSatoProgram *program,
                                 const int64_t *input_samples,
                                 size_t input_count,
                                 int64_t output_minimum,
                                 int64_t output_maximum,
                                 int64_t *result);

AvifdecStatus avif_sato_apply_rows(
    const AvifSatoProgram *program,
    const AvifdecImage *inputs,
    size_t input_count,
    unsigned int plane,
    uint32_t width,
    uint32_t row_begin,
    uint32_t row_end,
    int64_t output_minimum,
    int64_t output_maximum,
    AvifdecImage *output);

#endif
