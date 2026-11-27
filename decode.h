#ifndef TEXTLITE_DECODE_H
#define TEXTLITE_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Decode one silence-delimited PCM transmission into allocated text. */
int decode_pcm_samples(const int16_t *samples, size_t sample_count,
                       unsigned char **text_out, size_t *text_length_out,
                       unsigned *baud_rate_out);

#endif
