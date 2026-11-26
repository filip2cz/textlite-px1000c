/* Encode 7-bit text as a TEXT LITE PX1000 C FSK audio transmission. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    SAMPLE_RATE = 44100,
    MAX_TEXT_BYTES = 7400,
    MAX_TTS_WAV_BYTES = 128 * 1024 * 1024,
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

typedef struct {
    const unsigned char *samples;
    size_t data_size;
    uint32_t sample_rate;
    uint16_t channels;
} SpeechWav;

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

static uint16_t get_u16le(const unsigned char *input)
{
    return (uint16_t)input[0] | ((uint16_t)input[1] << 8);
}

static uint32_t get_u32le(const unsigned char *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8)
         | ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static int synthesize_speech(const unsigned char *text, size_t text_length,
                             const char *language,
                             unsigned char **wav_out, size_t *wav_length_out)
{
    /* eSpeak NG writes a PCM WAV to stdout; capture it without invoking a shell. */
    size_t spoken_length = text_length;
    while (spoken_length > 0 && text[spoken_length - 1] == '\r')
        --spoken_length;
    char *spoken_text = malloc(spoken_length + 1);
    if (spoken_text == NULL) {
        perror("malloc");
        return -1;
    }
    for (size_t i = 0; i < spoken_length; ++i)
        spoken_text[i] = (text[i] == '\r' || text[i] == '\t') ? ' ' : (char)text[i];
    spoken_text[spoken_length] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        free(spoken_text);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        free(spoken_text);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[1]);
        if (language != NULL)
            execlp("espeak-ng", "espeak-ng", "--stdout", "-z", "-v",
                   language, "--", spoken_text, (char *)NULL);
        else
            execlp("espeak-ng", "espeak-ng", "--stdout", "-z", "--",
                   spoken_text, (char *)NULL);
        perror("espeak-ng");
        _exit(127);
    }
    close(pipefd[1]);
    free(spoken_text);

    size_t capacity = 1U << 20;
    size_t used = 0;
    unsigned char *buffer = malloc(capacity);
    if (buffer == NULL) {
        perror("malloc");
        close(pipefd[0]);
        waitpid(child, NULL, 0);
        return -1;
    }
    for (;;) {
        if (used == capacity) {
            if (capacity >= MAX_TTS_WAV_BYTES) {
                fprintf(stderr, "TTS audio output is too large.\n");
                free(buffer);
                close(pipefd[0]);
                waitpid(child, NULL, 0);
                return -1;
            }
            size_t next = capacity * 2;
            if (next > MAX_TTS_WAV_BYTES)
                next = MAX_TTS_WAV_BYTES;
            unsigned char *grown = realloc(buffer, next);
            if (grown == NULL) {
                perror("realloc");
                free(buffer);
                close(pipefd[0]);
                waitpid(child, NULL, 0);
                return -1;
            }
            buffer = grown;
            capacity = next;
        }
        ssize_t got = read(pipefd[0], buffer + used, capacity - used);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            perror("read TTS output");
            free(buffer);
            close(pipefd[0]);
            waitpid(child, NULL, 0);
            return -1;
        }
        used += (size_t)got;
    }
    close(pipefd[0]);

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("waitpid");
        free(buffer);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || used < 44) {
        fprintf(stderr, "eSpeak NG failed to synthesize the text.\n");
        free(buffer);
        return -1;
    }
    *wav_out = buffer;
    *wav_length_out = used;
    return 0;
}

static int parse_speech_wav(const unsigned char *wav, size_t wav_length,
                            SpeechWav *speech)
{
    if (wav_length < 12 || memcmp(wav, "RIFF", 4) != 0 ||
        memcmp(wav + 8, "WAVE", 4) != 0)
        return -1;

    uint16_t format = 0;
    uint16_t bits_per_sample = 0;
    size_t offset = 12;
    speech->samples = NULL;
    speech->data_size = 0;
    speech->sample_rate = 0;
    speech->channels = 0;
    /* RIFF permits intervening chunks, and pads odd-sized payloads to the
       next even-byte boundary. */
    while (offset + 8 <= wav_length) {
        const unsigned char *chunk = wav + offset;
        uint32_t chunk_size = get_u32le(chunk + 4);
        size_t chunk_data = offset + 8;
        size_t available = wav_length - chunk_data;
        if (chunk_size > available) {
            /* eSpeak NG uses a large placeholder size for streamed WAV data. */
            if (memcmp(chunk, "data", 4) != 0 || format != 1)
                return -1;
            speech->samples = wav + chunk_data;
            speech->data_size = available;
            break;
        }
        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            format = get_u16le(wav + chunk_data);
            speech->channels = get_u16le(wav + chunk_data + 2);
            speech->sample_rate = get_u32le(wav + chunk_data + 4);
            bits_per_sample = get_u16le(wav + chunk_data + 14);
        } else if (memcmp(chunk, "data", 4) == 0) {
            speech->samples = wav + chunk_data;
            speech->data_size = chunk_size;
        }
        offset = chunk_data + chunk_size + (chunk_size & 1U);
    }
    if (format != 1 || bits_per_sample != 16 || speech->channels == 0 ||
        speech->channels > 2 || speech->sample_rate < 8000 ||
        speech->sample_rate > 192000 || speech->samples == NULL ||
        speech->data_size == 0 || speech->data_size % (speech->channels * 2U) != 0)
        return -1;
    return 0;
}

static int16_t read_speech_frame(const SpeechWav *speech, size_t frame)
{
    /* Average channels before interpolation to keep the output mono. */
    int32_t sum = 0;
    for (uint16_t channel = 0; channel < speech->channels; ++channel) {
        size_t index = (frame * speech->channels + channel) * 2;
        uint16_t value = get_u16le(speech->samples + index);
        sum += (int16_t)value;
    }
    return (int16_t)(sum / speech->channels);
}

static int append_speech(AudioWriter *writer, const SpeechWav *speech)
{
    size_t bytes_per_frame = (size_t)speech->channels * 2;
    size_t source_frames = speech->data_size / bytes_per_frame;
    if (source_frames == 0)
        return -1;
    uint64_t output_frames = (uint64_t)llround(
        (double)source_frames * SAMPLE_RATE / speech->sample_rate);
    /* Map each output frame back to the source timeline and blend its two
       neighboring frames to resample speech to 44.1 kHz. */
    for (uint64_t i = 0; i < output_frames; ++i) {
        double source_position = (double)i * speech->sample_rate / SAMPLE_RATE;
        size_t first = (size_t)source_position;
        if (first >= source_frames)
            first = source_frames - 1;
        size_t second = first + 1 < source_frames ? first + 1 : first;
        double fraction = source_position - first;
        double a = read_speech_frame(speech, first);
        double b = read_speech_frame(speech, second);
        int16_t sample = (int16_t)lrint(a + (b - a) * fraction);
        if (write_sample(writer, sample) != 0)
            return -1;
    }
    return 0;
}

static int write_silence(AudioWriter *writer, double seconds)
{
    uint64_t sample_count = (uint64_t)llround(seconds * SAMPLE_RATE);
    for (uint64_t i = 0; i < sample_count; ++i) {
        if (write_sample(writer, 0) != 0)
            return -1;
    }
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

static int parse_gap(const char *text, double *gap_out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < 0.0 || value > 600.0)
        return -1;
    *gap_out = value;
    return 0;
}

static int encode(const char *output_path, const unsigned char *text,
                  size_t text_length, unsigned margin, unsigned baud_rate,
                  int speak_enabled, const char *speech_language,
                  double gap_seconds)
{
    unsigned char *speech_wav = NULL;
    size_t speech_wav_length = 0;
    SpeechWav speech = {0};
    if (speak_enabled) {
        if (synthesize_speech(text, text_length, speech_language,
                              &speech_wav, &speech_wav_length) != 0)
            return -1;
        if (parse_speech_wav(speech_wav, speech_wav_length, &speech) != 0) {
            fprintf(stderr, "eSpeak NG produced an unsupported WAV format.\n");
            free(speech_wav);
            return -1;
        }
    }

    FILE *file = fopen(output_path, "wb+");
    if (file == NULL) {
        perror(output_path);
        free(speech_wav);
        return -1;
    }
    /* The final RIFF/data sizes are not known until PCM streaming finishes,
       so write a placeholder header and replace it after the byte count is known. */
    unsigned char empty_header[44] = {0};
    if (fwrite(empty_header, 1, sizeof(empty_header), file) != sizeof(empty_header)) {
        perror("write");
        fclose(file);
        free(speech_wav);
        return -1;
    }

    AudioWriter writer = {0};
    writer.file = file;
    writer.baud_rate = baud_rate;

    int failed = 0;
    if (speak_enabled) {
        failed = append_speech(&writer, &speech) != 0;
        if (!failed)
            failed = write_silence(&writer, gap_seconds) != 0;
    }
    free(speech_wav);

    /* PX header: 0.5 s MARK, sixteen NUL frames, 0.8 s MARK, four NUL frames. */
    if (!failed)
        failed = write_mark_duration(&writer, 500) != 0;
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
    fprintf(stderr, "Usage: %s [-o output.wav] [-m margin] [-b baud] [-s] [-l language] [-g seconds] [text]\n", program);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o output.wav  Output WAV path (default: encoded.wav).\n");
    fprintf(stderr, "  -m margin      Margin byte, from 0 to 127 (default: 40).\n");
    fprintf(stderr, "  -b baud        Baud rate: 300, 600, or 1200 (default: 300).\n");
    fprintf(stderr, "  -s, --speak    Read the text before the encoded transmission (requires espeak-ng).\n");
    fprintf(stderr, "  -l, --lang id  eSpeak NG language/voice code (default: eSpeak NG default).\n");
    fprintf(stderr, "  -g, --gap sec  Silence after speech before transmission (0 to 600, default: 3).\n");
    fprintf(stderr, "  --             Treat following arguments as text.\n");
    fprintf(stderr, "  text           Text to encode; multiple arguments are joined with spaces.\n");
    fprintf(stderr, "If text is omitted, it is read from standard input.\n");
}

int main(int argc, char **argv)
{
    const char *output_path = "encoded.wav";
    unsigned margin = 40;
    unsigned baud_rate = 300;
    int speak_enabled = 0;
    const char *speech_language = NULL;
    double gap_seconds = 3.0;
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
        } else if (strcmp(argv[index], "-s") == 0 ||
                   strcmp(argv[index], "--speak") == 0) {
            speak_enabled = 1;
            ++index;
        } else if ((strcmp(argv[index], "-l") == 0 ||
                    strcmp(argv[index], "--lang") == 0) && index + 1 < argc) {
            if (argv[index + 1][0] == '\0' || argv[index + 1][0] == '-') {
                fprintf(stderr, "Language must be an eSpeak NG voice code, such as cs or en-us.\n");
                return 2;
            }
            speech_language = argv[index + 1];
            index += 2;
        } else if ((strcmp(argv[index], "-g") == 0 ||
                    strcmp(argv[index], "--gap") == 0) && index + 1 < argc) {
            if (parse_gap(argv[index + 1], &gap_seconds) != 0) {
                fprintf(stderr, "Gap must be a number from 0 to 600 seconds.\n");
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
    } else {
        if (isatty(STDIN_FILENO)) {
            print_usage(argv[0]);
            return 2;
        }
        if (read_text_stdin(&raw_text, &raw_length) != 0) {
            fprintf(stderr, "Could not read input text.\n");
            return 1;
        }
    }
    int has_text_content = 0;
    for (size_t i = 0; i < raw_length; ++i) {
        if (raw_text[i] != '\r' && raw_text[i] != '\n') {
            has_text_content = 1;
            break;
        }
    }
    if (!has_text_content) {
        free(raw_text);
        print_usage(argv[0]);
        return 2;
    }

    unsigned char *text = NULL;
    size_t text_length = 0;
    int normalized = normalize_text(raw_text, raw_length, &text, &text_length);
    free(raw_text);
    if (normalized != 0)
        return 1;

    int result = encode(output_path, text, text_length, margin, baud_rate,
                        speak_enabled, speech_language, gap_seconds);
    free(text);
    if (result != 0)
        return 1;
    return 0;
}
