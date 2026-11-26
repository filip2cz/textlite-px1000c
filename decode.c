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
    SAMPLE_RATE = 22050,
    BAUD_RATE = 300,
    WINDOW_SIZE = 60,
    FRAME_BITS = 11,
    MAX_INPUT_BYTES = 512 * 1024 * 1024
};

typedef struct {
    unsigned char bit;
    float confidence;
    float rms;
} Symbol;

typedef struct {
    unsigned char *bytes;
    size_t count;
    double score;
} Candidate;

static int read_ffmpeg_pcm(const char *path, int16_t **samples_out,
                           size_t *sample_count_out)
{
    /* Let FFmpeg handle the input container and codec, then read normalized
       mono, signed 16-bit PCM from its stdout pipe. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (child == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(127);
        }
        close(pipefd[1]);
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-v", "error", "-i", path,
               "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "1", "-ar",
               "22050", "pipe:1", (char *)NULL);
        perror("ffmpeg");
        _exit(127);
    }

    close(pipefd[1]);
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
            if (capacity >= MAX_INPUT_BYTES) {
                fprintf(stderr, "Audio input is too large.\n");
                free(buffer);
                close(pipefd[0]);
                waitpid(child, NULL, 0);
                return -1;
            }
            size_t next = capacity * 2;
            if (next > MAX_INPUT_BYTES)
                next = MAX_INPUT_BYTES;
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
            perror("read");
            free(buffer);
            close(pipefd[0]);
            waitpid(child, NULL, 0);
            return -1;
        }
        used += (size_t)got;
    }
    close(pipefd[0]);

    int status;
    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        free(buffer);
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FFmpeg could not read audio file '%s'.\n", path);
        free(buffer);
        return -1;
    }
    if (used < 2 || (used & 1U)) {
        fprintf(stderr, "FFmpeg returned empty or incomplete PCM audio.\n");
        free(buffer);
        return -1;
    }

    size_t count = used / 2;
    int16_t *samples = malloc(count * sizeof(*samples));
    if (samples == NULL) {
        perror("malloc");
        free(buffer);
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        uint16_t value = (uint16_t)buffer[2 * i]
                       | ((uint16_t)buffer[2 * i + 1] << 8);
        samples[i] = (int16_t)value;
    }
    free(buffer);
    *samples_out = samples;
    *sample_count_out = count;
    return 0;
}

static Symbol *demodulate(const int16_t *samples, size_t count,
                          size_t *symbol_count_out, double *max_rms_out)
{
    /* Compare the signal energy at the two PX modem tones in short sliding
       windows. The stronger tone represents one serial bit. */
    if (count <= WINDOW_SIZE)
        return NULL;

    const double tau = 6.2831853071795864769;
    double cosine[2][WINDOW_SIZE];
    double sine[2][WINDOW_SIZE];
    const double frequencies[2] = {1300.0, 1700.0};
    for (int f = 0; f < 2; ++f) {
        for (int i = 0; i < WINDOW_SIZE; ++i) {
            double phase = tau * frequencies[f] * (double)i / SAMPLE_RATE;
            cosine[f][i] = cos(phase);
            sine[f][i] = sin(phase);
        }
    }

    size_t symbol_count = count - WINDOW_SIZE + 1;
    Symbol *symbols = calloc(symbol_count, sizeof(*symbols));
    if (symbols == NULL) {
        perror("calloc");
        return NULL;
    }

    double max_rms = 0.0;
    for (size_t start = 0; start < symbol_count; ++start) {
        double real[2] = {0.0, 0.0};
        double imag[2] = {0.0, 0.0};
        double power = 0.0;
        for (int i = 0; i < WINDOW_SIZE; ++i) {
            double v = samples[start + (size_t)i];
            power += v * v;
            for (int f = 0; f < 2; ++f) {
                real[f] += v * cosine[f][i];
                imag[f] += v * sine[f][i];
            }
        }
        double energy0 = real[0] * real[0] + imag[0] * imag[0];
        double energy1 = real[1] * real[1] + imag[1] * imag[1];
        double sum = energy0 + energy1;
        double rms = sqrt(power / WINDOW_SIZE);
        symbols[start].bit = energy0 > energy1 ? 1 : 0;
        symbols[start].confidence = sum > 0.0
            ? (float)(fabs(energy0 - energy1) / sum) : 0.0f;
        symbols[start].rms = (float)rms;
        if (rms > max_rms)
            max_rms = rms;
    }
    *symbol_count_out = symbol_count;
    *max_rms_out = max_rms;
    return symbols;
}

static int printable_byte(unsigned char c)
{
    return (c >= 32 && c <= 126) || c == '\r' || c == '\n' || c == '\t';
}

static void consider_run(Candidate *best, unsigned char *run, size_t count)
{
    /* Prefer long, parity-valid runs that contain readable text. */
    if (count < 12)
        return;

    size_t printable = 0;
    for (size_t i = 0; i < count; ++i)
        printable += (size_t)printable_byte(run[i]);
    double score = (double)count + 0.35 * (double)printable;
    if (score > best->score) {
        unsigned char *copy = malloc(count);
        if (copy == NULL)
            return;
        memcpy(copy, run, count);
        free(best->bytes);
        best->bytes = copy;
        best->count = count;
        best->score = score;
    }
}

static Candidate find_message(const Symbol *symbols, size_t symbol_count,
                              double max_rms)
{
    /* Search possible bit-clock phases and keep the longest sequence of
       valid 7E2 serial frames. */
    Candidate best = {0};
    double rms_gate = fmax(200.0, max_rms * 0.06);
    const double confidence_gate = 0.22;
    unsigned char *run = malloc(symbol_count / FRAME_BITS + 2);
    if (run == NULL)
        return best;

    /* Search nominal 300 baud plus a small clock-error range.  Half-sample
       phase steps also tolerate MP3 encoder delay. */
    for (double period = 73.0; period <= 74.0001; period += 0.05) {
        for (double phase = 0.0; phase < period; phase += 0.5) {
            size_t bit_count = (size_t)((symbol_count - WINDOW_SIZE) / period);
            for (size_t bit_offset = 0; bit_offset < FRAME_BITS; ++bit_offset) {
                size_t run_count = 0;
                for (size_t first = bit_offset;
                     first + FRAME_BITS <= bit_count;
                     first += FRAME_BITS) {
                    unsigned char bits[FRAME_BITS] = {0};
                    int active = 1;
                    for (size_t j = 0; j < FRAME_BITS; ++j) {
                        size_t center = (size_t)llround(phase + (first + j) * period);
                        size_t window_start = center >= WINDOW_SIZE / 2
                            ? center - WINDOW_SIZE / 2 : 0;
                        if (window_start >= symbol_count) {
                            active = 0;
                            break;
                        }
                        Symbol s = symbols[window_start];
                        bits[j] = s.bit;
                        if (s.rms < rms_gate || s.confidence < confidence_gate)
                            active = 0;
                    }

                    unsigned char value = 0;
                    for (size_t j = 0; j < 7; ++j)
                        value |= (unsigned char)(bits[j + 1] << j);
                    unsigned ones = (unsigned)__builtin_popcount((unsigned)value)
                                  + bits[8];
                    int valid = active && bits[0] == 0 && (ones & 1U) == 0
                             && bits[9] == 1 && bits[10] == 1;
                    if (valid) {
                        run[run_count++] = value;
                    } else {
                        consider_run(&best, run, run_count);
                        run_count = 0;
                    }
                }
                consider_run(&best, run, run_count);
            }
        }
    }
    free(run);
    return best;
}

static int print_message(const Candidate *candidate)
{
    /* Skip synchronization NULs, translate line endings, and omit padding
       control bytes after the message. */
    size_t begin = 0;
    while (begin < candidate->count && candidate->bytes[begin] == 0)
        ++begin;
    size_t end = candidate->count;
    while (end > begin && (candidate->bytes[end - 1] < 32
                           || candidate->bytes[end - 1] == ' '))
        --end;

    int wrote = 0;
    int previous_cr = 0;
    for (size_t i = begin; i < end; ++i) {
        unsigned char c = candidate->bytes[i];
        if (c == '\r') {
            putchar('\n');
            previous_cr = 1;
            wrote = 1;
        } else if (c == '\n') {
            if (!previous_cr)
                putchar('\n');
            previous_cr = 0;
            wrote = 1;
        } else if (c == '\t' || (c >= 32 && c <= 126)) {
            putchar(c);
            previous_cr = 0;
            wrote = 1;
        }
    }
    if (wrote && (end == begin || candidate->bytes[end - 1] != '\r'))
        putchar('\n');
    return wrote;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s audio_file\n", argv[0]);
        fprintf(stderr, "Input may be any audio format supported by FFmpeg.\n");
        return 2;
    }

    int16_t *samples = NULL;
    size_t sample_count = 0;
    if (read_ffmpeg_pcm(argv[1], &samples, &sample_count) != 0)
        return 1;

    size_t symbol_count = 0;
    double max_rms = 0.0;
    Symbol *symbols = demodulate(samples, sample_count, &symbol_count, &max_rms);
    free(samples);
    if (symbols == NULL) {
        fprintf(stderr, "Audio file is too short or could not be processed.\n");
        return 1;
    }

    Candidate message = find_message(symbols, symbol_count, max_rms);
    free(symbols);
    if (message.bytes == NULL) {
        fprintf(stderr, "No valid TEXT LITE PX1000 C transmission was found.\n");
        return 1;
    }

    int wrote = print_message(&message);
    free(message.bytes);
    if (!wrote) {
        fprintf(stderr, "The transmission contains valid data, but no printable text was found.\n");
        return 1;
    }
    return 0;
}
