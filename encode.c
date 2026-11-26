#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SAMPLE_RATE = 44100,
    MAX_TEXT_BYTES = 7400,
    PCM_AMPLITUDE = 14000,
    PCM_BUFFER_SIZE = 8192
};

typedef struct {
    FILE *file;
    unsigned char buffer[PCM_BUFFER_SIZE];
    size_t used;
    uint64_t data_bytes;
    double phase;
    unsigned baud_rate;
    unsigned sample_remainder;
} AudioWriter;

static int flush_pcm(AudioWriter *writer)
{
    if (writer->used == 0)
        return 0;
    if (fwrite(writer->buffer, 1, writer->used, writer->file) != writer->used)
        return -1;
    writer->data_bytes += writer->used;
    writer->used = 0;
    return 0;
}

static int write_sample(AudioWriter *writer, int16_t sample)
{
    uint16_t value = (uint16_t)sample;
    writer->buffer[writer->used++] = (unsigned char)(value & 0xffU);
    writer->buffer[writer->used++] = (unsigned char)(value >> 8);
    if (writer->used == sizeof(writer->buffer))
        return flush_pcm(writer);
    return 0;
}

static int write_tone_bit(AudioWriter *writer, unsigned bit)
{
    /* PX audio uses 1300 Hz for MARK/1 and 1700 Hz for SPACE/0. */
    const double tau = 6.2831853071795864769;
    double frequency = bit ? 1300.0 : 1700.0;
    double increment = tau * frequency / SAMPLE_RATE;
    /* Distribute fractional samples across bits to keep the average baud rate
       exact when the sample rate is not divisible by the selected speed. */
    writer->sample_remainder += SAMPLE_RATE;
    unsigned sample_count = writer->sample_remainder / writer->baud_rate;
    writer->sample_remainder %= writer->baud_rate;
    for (unsigned i = 0; i < sample_count; ++i) {
        int16_t sample = (int16_t)lrint(sin(writer->phase) * PCM_AMPLITUDE);
        if (write_sample(writer, sample) != 0)
            return -1;
        writer->phase += increment;
        if (writer->phase >= tau)
            writer->phase -= tau;
    }
    return 0;
}

static int write_mark_duration(AudioWriter *writer, unsigned milliseconds)
{
    uint64_t sample_count = (uint64_t)SAMPLE_RATE * milliseconds / 1000U;
    const double tau = 6.2831853071795864769;
    double increment = tau * 1300.0 / SAMPLE_RATE;
    for (uint64_t i = 0; i < sample_count; ++i) {
        int16_t sample = (int16_t)lrint(sin(writer->phase) * PCM_AMPLITUDE);
        if (write_sample(writer, sample) != 0)
            return -1;
        writer->phase += increment;
        if (writer->phase >= tau)
            writer->phase -= tau;
    }
    return 0;
}

static int write_serial_byte(AudioWriter *writer, unsigned char value)
{
    if (value > 0x7f) {
        fprintf(stderr, "Text Lite audio supports 7-bit characters only.\n");
        return -1;
    }

    /* Send one 7E2 serial frame: start, seven LSB-first data bits, even
       parity, and two MARK stop bits. */
    if (write_tone_bit(writer, 0) != 0)
        return -1;
    for (unsigned i = 0; i < 7; ++i) {
        if (write_tone_bit(writer, (value >> i) & 1U) != 0)
            return -1;
    }
    unsigned parity = (unsigned)__builtin_popcount((unsigned)value) & 1U;
    if (write_tone_bit(writer, parity) != 0 ||
        write_tone_bit(writer, 1) != 0 ||
        write_tone_bit(writer, 1) != 0)
        return -1;
    return 0;
}

static int write_serial_bytes(AudioWriter *writer, const unsigned char *bytes,
                              size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (write_serial_byte(writer, bytes[i]) != 0)
            return -1;
    }
    return 0;
}

static void put_u16le(unsigned char *out, uint16_t value)
{
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)(value >> 8);
}

static void put_u32le(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value & 0xffU);
    out[1] = (unsigned char)((value >> 8) & 0xffU);
    out[2] = (unsigned char)((value >> 16) & 0xffU);
    out[3] = (unsigned char)(value >> 24);
}

static int write_wav_header(FILE *file, uint32_t data_bytes)
{
    unsigned char header[44] = {0};
    memcpy(header, "RIFF", 4);
    put_u32le(header + 4, data_bytes + 36U);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    put_u32le(header + 16, 16);
    put_u16le(header + 20, 1); /* PCM format */
    put_u16le(header + 22, 1); /* mono */
    put_u32le(header + 24, SAMPLE_RATE);
    put_u32le(header + 28, SAMPLE_RATE * 2U);
    put_u16le(header + 32, 2);
    put_u16le(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_u32le(header + 40, data_bytes);
    return fwrite(header, 1, sizeof(header), file) == sizeof(header) ? 0 : -1;
}

static int read_text_stdin(unsigned char **text_out, size_t *length_out)
{
    size_t capacity = 256;
    size_t length = 0;
    unsigned char *text = malloc(capacity);
    if (text == NULL)
        return -1;

    for (;;) {
        int c = getchar();
        if (c == EOF)
            break;
        if (length >= MAX_TEXT_BYTES) {
            fprintf(stderr, "Input text exceeds the PX1000 C memory limit.\n");
            free(text);
            return -1;
        }
        if (length == capacity) {
            size_t next = capacity * 2;
            if (next > MAX_TEXT_BYTES)
                next = MAX_TEXT_BYTES;
            unsigned char *grown = realloc(text, next);
            if (grown == NULL) {
                free(text);
                return -1;
            }
            text = grown;
            capacity = next;
        }
        text[length++] = (unsigned char)c;
    }
    if (ferror(stdin)) {
        perror("stdin");
        free(text);
        return -1;
    }
    *text_out = text;
    *length_out = length;
    return 0;
}

static int normalize_text(const unsigned char *input, size_t input_length,
                          unsigned char **output_out, size_t *output_length_out)
{
    unsigned char *output = malloc(input_length + 2);
    if (output == NULL)
        return -1;

    size_t length = 0;
    for (size_t i = 0; i < input_length; ++i) {
        unsigned char c = input[i];
        if (c == '\r') {
            if (i + 1 < input_length && input[i + 1] == '\n')
                ++i;
            c = '\r';
        } else if (c == '\n') {
            c = '\r';
        } else if (c == '\t' || (c >= 32 && c <= 126)) {
            /* Preserve printable ASCII and tabs. */
        } else {
            fprintf(stderr, "Input must contain printable 7-bit ASCII text.\n");
            free(output);
            return -1;
        }
        output[length++] = c;
    }

    while (length > 0 && output[length - 1] == '\r')
        --length;
    if (length == 0) {
        fprintf(stderr, "Input text must not be empty.\n");
        free(output);
        return -1;
    }
    output[length++] = '\r';
    output[length++] = '\r';
    *output_out = output;
    *output_length_out = length;
    return 0;
}

static int parse_margin(const char *text, unsigned *margin_out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 127)
        return -1;
    *margin_out = (unsigned)value;
    return 0;
}

static int parse_baud_rate(const char *text, unsigned *baud_rate_out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        (value != 300 && value != 600 && value != 1200))
        return -1;
    *baud_rate_out = (unsigned)value;
    return 0;
}

static int encode(const char *output_path, const unsigned char *text,
                  size_t text_length, unsigned margin, unsigned baud_rate)
{
    FILE *file = fopen(output_path, "wb+");
    if (file == NULL) {
        perror(output_path);
        return -1;
    }
    unsigned char empty_header[44] = {0};
    if (fwrite(empty_header, 1, sizeof(empty_header), file) != sizeof(empty_header)) {
        perror("write");
        fclose(file);
        return -1;
    }

    AudioWriter writer = {0};
    writer.file = file;
    writer.baud_rate = baud_rate;

    /* PX header: 0.5 s MARK, sixteen NUL frames, 0.8 s MARK, four NUL frames. */
    int failed = write_mark_duration(&writer, 500) != 0;
    unsigned char nulls[16] = {0};
    if (!failed)
        failed = write_serial_bytes(&writer, nulls, sizeof(nulls)) != 0;
    if (!failed)
        failed = write_mark_duration(&writer, 800) != 0;
    if (!failed) {
        unsigned char sync[4] = {0};
        failed = write_serial_bytes(&writer, sync, sizeof(sync)) != 0;
    }

    /* The first data byte is the margin, followed by text and CR line ends. */
    if (!failed)
        failed = write_serial_byte(&writer, (unsigned char)margin) != 0;
    if (!failed)
        failed = write_serial_bytes(&writer, text, text_length) != 0;

    /* Four eight-byte EOT groups terminate a normal, unencrypted message. */
    unsigned char eot[32];
    memset(eot, 0x04, sizeof(eot));
    if (!failed)
        failed = write_serial_bytes(&writer, eot, sizeof(eot)) != 0;
    if (!failed)
        failed = write_mark_duration(&writer, 1000) != 0;
    if (!failed)
        failed = flush_pcm(&writer) != 0;

    if (!failed && (writer.data_bytes > UINT32_MAX - 36U ||
                    fseek(file, 0, SEEK_SET) != 0 ||
                    write_wav_header(file, (uint32_t)writer.data_bytes) != 0))
        failed = 1;
    if (fclose(file) != 0)
        failed = 1;
    if (failed) {
        fprintf(stderr, "Could not write WAV file '%s'.\n", output_path);
        return -1;
    }
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Usage: %s [-o output.wav] [-m margin] [-b baud] [text]\n", program);
    fprintf(stderr, "Baud rate must be 300, 600, or 1200 (default: 300).\n");
    fprintf(stderr, "If text is omitted, it is read from standard input.\n");
}

int main(int argc, char **argv)
{
    const char *output_path = "encoded.wav";
    unsigned margin = 40;
    unsigned baud_rate = 300;
    int index = 1;
    while (index < argc && argv[index][0] == '-') {
        if (strcmp(argv[index], "--") == 0) {
            ++index;
            break;
        } else if (strcmp(argv[index], "-o") == 0 && index + 1 < argc) {
            output_path = argv[index + 1];
            index += 2;
        } else if (strcmp(argv[index], "-m") == 0 && index + 1 < argc) {
            if (parse_margin(argv[index + 1], &margin) != 0) {
                fprintf(stderr, "Margin must be an integer from 0 to 127.\n");
                return 2;
            }
            index += 2;
        } else if (strcmp(argv[index], "-b") == 0 && index + 1 < argc) {
            if (parse_baud_rate(argv[index + 1], &baud_rate) != 0) {
                fprintf(stderr, "Baud rate must be 300, 600, or 1200.\n");
                return 2;
            }
            index += 2;
        } else {
            print_usage(argv[0]);
            return 2;
        }
    }

    unsigned char *raw_text = NULL;
    size_t raw_length = 0;
    if (index < argc) {
        size_t capacity = 1;
        for (int i = index; i < argc; ++i)
            capacity += strlen(argv[i]) + (i + 1 < argc ? 1U : 0U);
        if (capacity - 1 > MAX_TEXT_BYTES) {
            fprintf(stderr, "Input text exceeds the PX1000 C memory limit.\n");
            return 1;
        }
        raw_text = malloc(capacity);
        if (raw_text == NULL) {
            perror("malloc");
            return 1;
        }
        for (int i = index; i < argc; ++i) {
            size_t n = strlen(argv[i]);
            memcpy(raw_text + raw_length, argv[i], n);
            raw_length += n;
            if (i + 1 < argc)
                raw_text[raw_length++] = ' ';
        }
    } else if (read_text_stdin(&raw_text, &raw_length) != 0) {
        fprintf(stderr, "Could not read input text.\n");
        return 1;
    }

    unsigned char *text = NULL;
    size_t text_length = 0;
    int normalized = normalize_text(raw_text, raw_length, &text, &text_length);
    free(raw_text);
    if (normalized != 0)
        return 1;

    int result = encode(output_path, text, text_length, margin, baud_rate);
    free(text);
    if (result != 0)
        return 1;
    return 0;
}
