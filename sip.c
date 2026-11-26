#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pjmedia/wav_port.h>
#include <pjsua-lib/pjsua.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_COMMAND_OUTPUT = 7400,
    DEFAULT_SIP_PORT = 0,
    MEDIA_WAIT_SECONDS = 30,
    PLAYBACK_TIMEOUT_MARGIN_SECONDS = 30,
    MAX_ENCODER_ARGS = 64
};

typedef struct {
    const char *registrar;
    const char *identity;
    const char *username;
    const char *password;
    const char *command;
    const char *encoder;
    unsigned baud_rate;
    int margin_set;
    unsigned margin;
    int speak;
    const char *speech_language;
    const char *gap_seconds;
    const char *extra_encoder_args[MAX_ENCODER_ARGS];
    size_t extra_encoder_arg_count;
    unsigned local_port;
} AppConfig;

typedef struct {
    const AppConfig *config;
    pjsua_call_id call_id;
} WorkerArgs;

extern char **environ;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
static const AppConfig *app_config;
static pjsua_call_id active_call = -1;
static pjsua_conf_port_id active_audio_slot = -1;
static pjsua_player_id active_player_id = -1;
static int call_is_active;
static int media_is_ready;
static int playback_is_finished;
static int worker_is_joinable;
static int worker_is_finished = 1;
static pthread_t worker_thread;
static pid_t command_process_group = -1;
static volatile sig_atomic_t stop_requested;

static void print_pj_error(const char *operation, pj_status_t status)
{
    char message[PJ_ERR_MSG_SIZE];
    pj_strerror(status, message, sizeof(message));
    fprintf(stderr, "%s failed: %s\n", operation, message);
}

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int call_is_current_and_active(pjsua_call_id call_id)
{
    int result;
    pthread_mutex_lock(&state_mutex);
    result = !stop_requested && call_is_active && active_call == call_id;
    pthread_mutex_unlock(&state_mutex);
    return result;
}

static void mark_call_inactive(pjsua_call_id call_id)
{
    pthread_mutex_lock(&state_mutex);
    if (active_call == call_id) {
        call_is_active = 0;
        media_is_ready = 0;
        active_call = -1;
        active_audio_slot = -1;
        pthread_cond_broadcast(&state_changed);
    }
    pthread_mutex_unlock(&state_mutex);
}

static void hangup_if_active(pjsua_call_id call_id)
{
    if (call_is_current_and_active(call_id)) {
        pj_status_t status = pjsua_call_hangup(call_id, 0, NULL, NULL);
        if (status != PJ_SUCCESS)
            print_pj_error("Could not hang up the call", status);
        mark_call_inactive(call_id);
    }
}

static int wait_for_child(pid_t child, int *status_out)
{
    pid_t result;
    do {
        result = waitpid(child, status_out, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror("waitpid");
        return -1;
    }
    return 0;
}

static void stop_command_group(pid_t child)
{
    if (kill(-child, SIGTERM) != 0)
        kill(child, SIGTERM);
}

static void remember_command_group(pid_t child)
{
    pthread_mutex_lock(&state_mutex);
    command_process_group = child;
    pthread_mutex_unlock(&state_mutex);
}

static void forget_command_group(pid_t child)
{
    pthread_mutex_lock(&state_mutex);
    if (command_process_group == child)
        command_process_group = -1;
    pthread_mutex_unlock(&state_mutex);
}

static int spawn_shell_command(const char *command, unsigned char **text_out,
                               size_t *length_out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int actions_ready = 0;
    int attributes_ready = 0;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0) {
        actions_ready = 1;
        error = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    }
    if (error == 0)
        error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                                  "/dev/null", O_RDONLY, 0);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (error == 0) {
        error = posix_spawnattr_init(&attributes);
        if (error == 0)
            attributes_ready = 1;
    }
    if (error == 0)
        error = posix_spawnattr_setpgroup(&attributes, 0);
    if (error == 0)
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);

    pid_t child = -1;
    if (error == 0) {
        char *arguments[] = {"sh", "-c", (char *)command, NULL};
        error = posix_spawn(&child, "/bin/sh", &actions, &attributes,
                            arguments, environ);
    }
    if (attributes_ready)
        posix_spawnattr_destroy(&attributes);
    if (actions_ready)
        posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        fprintf(stderr, "Could not start the text command: %s\n", strerror(error));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);
    remember_command_group(child);
    unsigned char *buffer = malloc(MAX_COMMAND_OUTPUT + 1U);
    if (buffer == NULL) {
        perror("malloc");
        close(pipefd[0]);
        stop_command_group(child);
        wait_for_child(child, &(int){0});
        forget_command_group(child);
        return -1;
    }

    size_t used = 0;
    int failed = 0;
    int too_large = 0;
    while (used < MAX_COMMAND_OUTPUT + 1U) {
        ssize_t count = read(pipefd[0], buffer + used,
                             MAX_COMMAND_OUTPUT + 1U - used);
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            perror("read command output");
            failed = 1;
            break;
        }
        used += (size_t)count;
        if (used > MAX_COMMAND_OUTPUT) {
            fprintf(stderr, "Text command output exceeds %u bytes.\n",
                    (unsigned)MAX_COMMAND_OUTPUT);
            too_large = 1;
            break;
        }
    }
    close(pipefd[0]);
    if (failed || too_large || stop_requested)
        stop_command_group(child);

    int child_status = 0;
    if (wait_for_child(child, &child_status) != 0)
        failed = 1;
    forget_command_group(child);
    if (failed || too_large || stop_requested || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        if (!failed && !too_large && !stop_requested)
            fprintf(stderr, "Text command exited unsuccessfully.\n");
        free(buffer);
        return -1;
    }
    if (used == 0) {
        fprintf(stderr, "Text command produced no output.\n");
        free(buffer);
        return -1;
    }
    *text_out = buffer;
    *length_out = used;
    return 0;
}

static int write_all(int descriptor, const unsigned char *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(descriptor, data + written, length - written);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        written += (size_t)count;
    }
    return 0;
}

static int run_encoder(const AppConfig *config, const char *wav_path,
                       const unsigned char *text, size_t text_length)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    int actions_ready = error == 0;
    if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    if (error != 0) {
        fprintf(stderr, "Could not prepare the encoder process: %s\n", strerror(error));
        if (actions_ready)
            posix_spawn_file_actions_destroy(&actions);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    char baud_text[16];
    char margin_text[16];
    snprintf(baud_text, sizeof(baud_text), "%u", config->baud_rate);

    char *arguments[16 + MAX_ENCODER_ARGS];
    size_t argc = 0;
    arguments[argc++] = (char *)config->encoder;
    arguments[argc++] = "-o";
    arguments[argc++] = (char *)wav_path;
    arguments[argc++] = "-b";
    arguments[argc++] = baud_text;
    if (config->margin_set) {
        snprintf(margin_text, sizeof(margin_text), "%u", config->margin);
        arguments[argc++] = "-m";
        arguments[argc++] = margin_text;
    }
    if (config->speak)
        arguments[argc++] = "-s";
    if (config->speech_language != NULL) {
        arguments[argc++] = "-l";
        arguments[argc++] = (char *)config->speech_language;
    }
    if (config->gap_seconds != NULL) {
        arguments[argc++] = "-g";
        arguments[argc++] = (char *)config->gap_seconds;
    }
    for (size_t i = 0; i < config->extra_encoder_arg_count; ++i)
        arguments[argc++] = (char *)config->extra_encoder_args[i];
    arguments[argc] = NULL;

    pid_t child = -1;
    error = posix_spawnp(&child, config->encoder, &actions, NULL,
                         arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        fprintf(stderr, "Could not start encoder '%s': %s\n",
                config->encoder, strerror(error));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[0]);
    int write_failed = write_all(pipefd[1], text, text_length) != 0;
    if (write_failed)
        perror("write encoder input");
    close(pipefd[1]);

    int child_status = 0;
    if (wait_for_child(child, &child_status) != 0)
        return -1;
    if (write_failed || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "Encoder failed to create the call audio.\n");
        return -1;
    }
    return 0;
}

static int make_call_audio(const AppConfig *config, char *wav_path,
                           uint64_t *sample_count_out)
{
    unsigned char *text = NULL;
    size_t text_length = 0;
    if (spawn_shell_command(config->command, &text, &text_length) != 0)
        return -1;

    strcpy(wav_path, "/tmp/textlite-sip-XXXXXX.wav");
    int descriptor = mkstemps(wav_path, 4);
    if (descriptor < 0) {
        perror("mkstemps");
        free(text);
        return -1;
    }
    if (close(descriptor) != 0) {
        perror("close temporary WAV");
        unlink(wav_path);
        free(text);
        return -1;
    }

    int result = run_encoder(config, wav_path, text, text_length);
    free(text);
    if (result != 0) {
        unlink(wav_path);
        return -1;
    }

    struct stat file_info;
    if (stat(wav_path, &file_info) != 0) {
        perror("stat encoded WAV");
        unlink(wav_path);
        return -1;
    }
    if (file_info.st_size <= 44 || (file_info.st_size - 44) % 2 != 0) {
        fprintf(stderr, "Encoder produced an invalid or empty WAV file.\n");
        unlink(wav_path);
        return -1;
    }
    *sample_count_out = (uint64_t)(file_info.st_size - 44) / 2U;
    return 0;
}

static void on_player_eof(pjmedia_port *port, void *user_data)
{
    (void)port;
    pjsua_player_id player_id = (pjsua_player_id)(intptr_t)user_data;
    pthread_mutex_lock(&state_mutex);
    if (active_player_id == player_id && !playback_is_finished) {
        playback_is_finished = 1;
        pthread_cond_broadcast(&state_changed);
    }
    pthread_mutex_unlock(&state_mutex);
}

static int wait_for_audio_media(pjsua_call_id call_id,
                                pjsua_conf_port_id *audio_slot_out)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("clock_gettime");
        return -1;
    }
    deadline.tv_sec += MEDIA_WAIT_SECONDS;

    pthread_mutex_lock(&state_mutex);
    int wait_result = 0;
    while (!stop_requested && call_is_active && active_call == call_id &&
           !media_is_ready && wait_result == 0) {
        wait_result = pthread_cond_timedwait(&state_changed, &state_mutex, &deadline);
    }
    int ready = !stop_requested && call_is_active && active_call == call_id &&
                media_is_ready;
    if (ready)
        *audio_slot_out = active_audio_slot;
    pthread_mutex_unlock(&state_mutex);

    if (!ready) {
        if (wait_result == ETIMEDOUT)
            fprintf(stderr, "Timed out waiting for active SIP audio.\n");
        return -1;
    }
    return 0;
}

static int wait_for_playback_finish(pjsua_call_id call_id,
                                    unsigned timeout_seconds)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("clock_gettime");
        return -1;
    }
    deadline.tv_sec += (time_t)timeout_seconds;

    pthread_mutex_lock(&state_mutex);
    int wait_result = 0;
    while (!stop_requested && call_is_active && active_call == call_id &&
           !playback_is_finished && wait_result == 0) {
        wait_result = pthread_cond_timedwait(&state_changed, &state_mutex, &deadline);
    }
    int finished = !stop_requested && call_is_active && active_call == call_id &&
                   playback_is_finished;
    pthread_mutex_unlock(&state_mutex);

    if (!finished && wait_result == ETIMEDOUT)
        fprintf(stderr, "Timed out waiting for SIP audio playback to finish.\n");
    return finished ? 0 : -1;
}

static void *call_worker(void *argument)
{
    WorkerArgs *args = argument;
    const AppConfig *config = args->config;
    pjsua_call_id call_id = args->call_id;
    free(args);

    pj_thread_desc thread_description;
    pj_thread_t *pj_thread = NULL;
    memset(thread_description, 0, sizeof(thread_description));
    pj_status_t status = pj_thread_register("sip-call-worker",
                                            thread_description, &pj_thread);
    int thread_registered = status == PJ_SUCCESS;
    if (!thread_registered)
        print_pj_error("Could not register the call worker thread", status);

    char wav_path[sizeof("/tmp/textlite-sip-XXXXXX.wav")];
    wav_path[0] = '\0';
    uint64_t sample_count = 0;
    pjsua_player_id player_id = -1;
    pjsua_conf_port_id player_slot = -1;
    pjsua_conf_port_id call_slot = -1;
    int should_hangup = 1;

    if (!call_is_current_and_active(call_id))
        goto cleanup;
    if (make_call_audio(config, wav_path, &sample_count) != 0)
        goto cleanup;
    if (!call_is_current_and_active(call_id)) {
        should_hangup = 0;
        goto cleanup;
    }

    if (wait_for_audio_media(call_id, &call_slot) != 0) {
        if (!call_is_current_and_active(call_id))
            should_hangup = 0;
        goto cleanup;
    }

    pj_str_t filename = pj_str(wav_path);
    status = pjsua_player_create(&filename, PJMEDIA_FILE_NO_LOOP, &player_id);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not open the encoded WAV for playback", status);
        goto cleanup;
    }

    pjmedia_port *player_port = NULL;
    status = pjsua_player_get_port(player_id, &player_port);
    if (status != PJ_SUCCESS || player_port == NULL) {
        print_pj_error("Could not inspect the WAV player port", status);
        goto cleanup;
    }

    unsigned sample_rate = PJMEDIA_PIA_SRATE(&player_port->info);
    if (sample_rate == 0)
        sample_rate = 44100;
    unsigned timeout_seconds = (unsigned)(sample_count / sample_rate) +
                               PLAYBACK_TIMEOUT_MARGIN_SECONDS;

    pthread_mutex_lock(&state_mutex);
    active_player_id = player_id;
    playback_is_finished = 0;
    pthread_mutex_unlock(&state_mutex);

    status = pjmedia_wav_player_set_eof_cb2(player_port,
                                            (void *)(intptr_t)player_id,
                                            on_player_eof);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not register WAV playback completion callback", status);
        goto cleanup;
    }

    player_slot = pjsua_player_get_conf_port(player_id);
    status = pjsua_conf_connect(player_slot, call_slot);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not connect WAV playback to the SIP call", status);
        player_slot = -1;
        goto cleanup;
    }
    fprintf(stderr, "Playing encoded message to call %d.\n", call_id);

    int playback_complete = wait_for_playback_finish(call_id, timeout_seconds) == 0;
    if (playback_complete) {
        pj_thread_sleep(250);
        fprintf(stderr, "Message playback finished.\n");
    }
    if (!call_is_current_and_active(call_id))
        should_hangup = 0;

cleanup:
    pthread_mutex_lock(&state_mutex);
    if (active_player_id == player_id)
        active_player_id = -1;
    pthread_mutex_unlock(&state_mutex);

    if (player_slot >= 0 && call_slot >= 0 && call_is_current_and_active(call_id))
        pjsua_conf_disconnect(player_slot, call_slot);
    if (player_id >= 0) {
        status = pjsua_player_destroy(player_id);
        if (status != PJ_SUCCESS)
            print_pj_error("Could not close the WAV player", status);
    }
    if (wav_path[0] != '\0')
        unlink(wav_path);
    if (should_hangup)
        hangup_if_active(call_id);

    pthread_mutex_lock(&state_mutex);
    worker_is_finished = 1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    if (thread_registered)
        pj_thread_unregister();
    return NULL;
}

static void on_incoming_call(pjsua_acc_id account_id, pjsua_call_id call_id,
                             pjsip_rx_data *request)
{
    (void)account_id;
    (void)request;

    pthread_mutex_lock(&state_mutex);
    int busy = stop_requested || call_is_active || worker_is_joinable;
    if (!busy) {
        call_is_active = 1;
        active_call = call_id;
        active_audio_slot = -1;
        media_is_ready = 0;
        worker_is_finished = 0;
    }
    pthread_mutex_unlock(&state_mutex);

    if (busy) {
        fprintf(stderr, "Rejecting incoming call: already processing a call.\n");
        pjsua_call_answer(call_id, PJSIP_SC_BUSY_HERE, NULL, NULL);
        return;
    }

    fprintf(stderr, "Accepting incoming call %d.\n", call_id);
    pj_status_t status = pjsua_call_answer(call_id, PJSIP_SC_OK, NULL, NULL);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not answer incoming call", status);
        mark_call_inactive(call_id);
        return;
    }

    WorkerArgs *args = malloc(sizeof(*args));
    if (args == NULL) {
        perror("malloc worker arguments");
        pjsua_call_hangup(call_id, 0, NULL, NULL);
        mark_call_inactive(call_id);
        return;
    }
    args->config = app_config;
    args->call_id = call_id;

    pthread_t thread;
    int error = pthread_create(&thread, NULL, call_worker, args);
    if (error != 0) {
        fprintf(stderr, "Could not start call worker: %s\n", strerror(error));
        free(args);
        pjsua_call_hangup(call_id, 0, NULL, NULL);
        mark_call_inactive(call_id);
        pthread_mutex_lock(&state_mutex);
        worker_is_finished = 1;
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    pthread_mutex_lock(&state_mutex);
    worker_thread = thread;
    worker_is_joinable = 1;
    pthread_mutex_unlock(&state_mutex);
}

static void on_call_media_state(pjsua_call_id call_id)
{
    pjsua_call_info info;
    pj_status_t status = pjsua_call_get_info(call_id, &info);
    if (status != PJ_SUCCESS)
        return;

    pjsua_conf_port_id audio_slot = -1;
    for (unsigned i = 0; i < info.media_cnt; ++i) {
        if (info.media[i].type == PJMEDIA_TYPE_AUDIO &&
            info.media[i].status == PJSUA_CALL_MEDIA_ACTIVE &&
            (info.media[i].dir & PJMEDIA_DIR_ENCODING)) {
            audio_slot = info.media[i].stream.aud.conf_slot;
            break;
        }
    }
    if (audio_slot < 0)
        return;

    pthread_mutex_lock(&state_mutex);
    if (call_is_active && active_call == call_id) {
        active_audio_slot = audio_slot;
        media_is_ready = 1;
        pthread_cond_broadcast(&state_changed);
    }
    pthread_mutex_unlock(&state_mutex);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *event)
{
    (void)event;
    pjsua_call_info info;
    if (pjsua_call_get_info(call_id, &info) != PJ_SUCCESS)
        return;
    if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
        fprintf(stderr, "Call %d disconnected.\n", call_id);
        mark_call_inactive(call_id);
    }
}

static void on_registration_state(pjsua_acc_id account_id)
{
    pjsua_acc_info info;
    if (pjsua_acc_get_info(account_id, &info) == PJ_SUCCESS)
        fprintf(stderr, "SIP registration status: %d\n", (int)info.status);
}

static int parse_unsigned(const char *text, unsigned minimum, unsigned maximum,
                          unsigned *value_out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < minimum || value > maximum)
        return -1;
    *value_out = (unsigned)value;
    return 0;
}

static int parse_gap_text(const char *text)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < 0.0 || value > 600.0)
        return -1;
    return 0;
}

static int add_extra_encoder_arg(AppConfig *config, const char *argument)
{
    if (config->extra_encoder_arg_count >= MAX_ENCODER_ARGS) {
        fprintf(stderr, "Too many extra encoder arguments (max %u).\n",
                (unsigned)MAX_ENCODER_ARGS);
        return -1;
    }
    config->extra_encoder_args[config->extra_encoder_arg_count++] = argument;
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --registrar sip:host --identity sip:user@host --username name --command shell-command [options] [-- encoder-args...]\n",
            program);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --registrar URI       SIP registrar URI (required; UDP).\n");
    fprintf(stderr, "  --identity URI        SIP account identity (required).\n");
    fprintf(stderr, "  --username name       SIP digest-auth username (required).\n");
    fprintf(stderr, "  --password password   SIP password (or set SIP_PASSWORD).\n");
    fprintf(stderr, "  --command command     Shell command whose stdout becomes the message.\n");
    fprintf(stderr, "  --encoder path        Encoder program (default: ./encode).\n");
    fprintf(stderr, "  --baud rate           300, 600, or 1200 (default: 300).\n");
    fprintf(stderr, "  --margin value        Encoder margin byte, 0 to 127 (default: 40).\n");
    fprintf(stderr, "  --speak               Enable speech before transmission (encode -s).\n");
    fprintf(stderr, "  --lang code           eSpeak NG language/voice code (encode -l).\n");
    fprintf(stderr, "  --gap seconds         Silence after speech, 0 to 600 (encode -g).\n");
    fprintf(stderr, "  --encoder-arg arg     Extra argument forwarded to encoder (repeatable).\n");
    fprintf(stderr, "  --                    Forward all remaining arguments to encoder.\n");
    fprintf(stderr, "  --port number         Local UDP port, 0 to 65535 (default: 0, auto).\n");
    fprintf(stderr, "  -h, --help            Show this help.\n");
    fprintf(stderr, "The client answers one call at a time and hangs up after playback.\n");
}

static int parse_arguments(int argc, char **argv, AppConfig *config)
{
    *config = (AppConfig){
        .encoder = "./encode",
        .baud_rate = 300,
        .local_port = DEFAULT_SIP_PORT
    };

    for (int index = 1; index < argc;) {
        const char *option = argv[index++];
        if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(option, "--speak") == 0) {
            config->speak = 1;
            continue;
        }
        if (strcmp(option, "--") == 0) {
            while (index < argc) {
                if (add_extra_encoder_arg(config, argv[index++]) != 0)
                    return -1;
            }
            break;
        }
        if (index >= argc) {
            print_usage(argv[0]);
            return -1;
        }
        const char *value = argv[index++];
        if (strcmp(option, "--registrar") == 0)
            config->registrar = value;
        else if (strcmp(option, "--identity") == 0)
            config->identity = value;
        else if (strcmp(option, "--username") == 0)
            config->username = value;
        else if (strcmp(option, "--password") == 0)
            config->password = value;
        else if (strcmp(option, "--command") == 0)
            config->command = value;
        else if (strcmp(option, "--encoder") == 0)
            config->encoder = value;
        else if (strcmp(option, "--baud") == 0) {
            if (parse_unsigned(value, 300, 1200, &config->baud_rate) != 0 ||
                (config->baud_rate != 300 && config->baud_rate != 600 &&
                 config->baud_rate != 1200)) {
                fprintf(stderr, "Baud rate must be 300, 600, or 1200.\n");
                return -1;
            }
        } else if (strcmp(option, "--margin") == 0) {
            if (parse_unsigned(value, 0, 127, &config->margin) != 0) {
                fprintf(stderr, "Margin must be an integer from 0 to 127.\n");
                return -1;
            }
            config->margin_set = 1;
        } else if (strcmp(option, "--lang") == 0) {
            if (value[0] == '\0' || value[0] == '-') {
                fprintf(stderr, "Language must be an eSpeak NG voice code, such as cs or en-us.\n");
                return -1;
            }
            config->speech_language = value;
        } else if (strcmp(option, "--gap") == 0) {
            if (parse_gap_text(value) != 0) {
                fprintf(stderr, "Gap must be a number from 0 to 600 seconds.\n");
                return -1;
            }
            config->gap_seconds = value;
        } else if (strcmp(option, "--encoder-arg") == 0) {
            if (add_extra_encoder_arg(config, value) != 0)
                return -1;
        } else if (strcmp(option, "--port") == 0) {
            if (parse_unsigned(value, 0, 65535, &config->local_port) != 0) {
                fprintf(stderr, "Local SIP port must be from 0 to 65535.\n");
                return -1;
            }
        } else {
            print_usage(argv[0]);
            return -1;
        }
    }

    if (config->password == NULL)
        config->password = getenv("SIP_PASSWORD");
    if (config->password != NULL && config->password[0] == '\0')
        config->password = NULL;

    if (config->registrar == NULL || config->identity == NULL ||
        config->username == NULL || config->username[0] == '\0' ||
        config->command == NULL || config->command[0] == '\0' ||
        strncmp(config->registrar, "sip:", 4) != 0 ||
        strncmp(config->identity, "sip:", 4) != 0) {
        fprintf(stderr, "Registrar, SIP identity, username, and command are required.\n");
        print_usage(argv[0]);
        return -1;
    }
    return 0;
}

static void reap_finished_worker(void)
{
    pthread_t thread;
    int should_join = 0;
    pthread_mutex_lock(&state_mutex);
    if (worker_is_joinable && worker_is_finished) {
        thread = worker_thread;
        worker_is_joinable = 0;
        should_join = 1;
    }
    pthread_mutex_unlock(&state_mutex);
    if (should_join) {
        int error = pthread_join(thread, NULL);
        if (error != 0)
            fprintf(stderr, "Could not join call worker: %s\n", strerror(error));
    }
}

static void configure_modem_codecs(void)
{
    /* Prefer waveform codecs (G.711/G.722) so FSK tones are not mangled by vocoders. */
    pjsua_codec_info codecs[64];
    unsigned count = sizeof(codecs) / sizeof(codecs[0]);
    if (pjsua_enum_codecs(codecs, &count) != PJ_SUCCESS)
        return;

    for (unsigned i = 0; i < count; ++i) {
        const pj_str_t *id = &codecs[i].codec_id;
        pj_uint8_t priority = codecs[i].priority;
        if (pj_strnicmp2(id, "PCMA/8000/", 10) == 0)
            priority = PJMEDIA_CODEC_PRIO_HIGHEST;
        else if (pj_strnicmp2(id, "PCMU/8000/", 10) == 0)
            priority = PJMEDIA_CODEC_PRIO_HIGHEST - 1;
        else if (pj_strnicmp2(id, "G722/16000/", 11) == 0)
            priority = PJMEDIA_CODEC_PRIO_NORMAL + 50;
        else if (pj_strnicmp2(id, "speex/", 6) == 0 ||
                 pj_strnicmp2(id, "iLBC/", 5) == 0 ||
                 pj_strnicmp2(id, "GSM/", 4) == 0)
            priority = PJMEDIA_CODEC_PRIO_LOWEST;
        else
            continue;

        pjsua_codec_set_priority(id, priority);
    }
}

int main(int argc, char **argv)
{
    AppConfig config;
    int parsed = parse_arguments(argc, argv, &config);
    if (parsed != 0)
        return parsed > 0 ? 0 : 2;
    app_config = &config;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    pj_status_t status = pjsua_create();
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not create PJSIP endpoint", status);
        return 1;
    }

    pjsua_config ua_config;
    pjsua_logging_config logging_config;
    pjsua_media_config media_config;
    pjsua_config_default(&ua_config);
    ua_config.max_calls = 4;
    ua_config.cb.on_incoming_call = on_incoming_call;
    ua_config.cb.on_call_media_state = on_call_media_state;
    ua_config.cb.on_call_state = on_call_state;
    ua_config.cb.on_reg_state = on_registration_state;
    pjsua_logging_config_default(&logging_config);
    logging_config.console_level = 3;
    pjsua_media_config_default(&media_config);
    media_config.no_vad = PJ_TRUE;
    media_config.ec_tail_len = 0;

    status = pjsua_init(&ua_config, &logging_config, &media_config);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not initialize PJSIP", status);
        goto shutdown;
    }
    configure_modem_codecs();

    pjsua_transport_config transport_config;
    pjsua_transport_config_default(&transport_config);
    transport_config.port = config.local_port;
    status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &transport_config, NULL);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not create UDP SIP transport", status);
        goto shutdown;
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not start PJSIP", status);
        goto shutdown;
    }
    status = pjsua_set_null_snd_dev();
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not initialize the null audio device", status);
        goto shutdown;
    }

    pjsua_acc_config account_config;
    pjsua_acc_config_default(&account_config);
    account_config.id = pj_str((char *)config.identity);
    account_config.reg_uri = pj_str((char *)config.registrar);
    if (config.password != NULL) {
        account_config.cred_count = 1;
        account_config.cred_info[0].realm = pj_str("*");
        account_config.cred_info[0].scheme = pj_str("digest");
        account_config.cred_info[0].username = pj_str((char *)config.username);
        account_config.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
        account_config.cred_info[0].data = pj_str((char *)config.password);
    }
    pjsua_acc_id account_id;
    status = pjsua_acc_add(&account_config, PJ_TRUE, &account_id);
    if (status != PJ_SUCCESS) {
        print_pj_error("Could not add SIP account", status);
        goto shutdown;
    }

    fprintf(stderr, "SIP client started; waiting for incoming calls.\n");
    while (!stop_requested) {
        reap_finished_worker();
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
            deadline.tv_sec += 1;
            pthread_mutex_lock(&state_mutex);
            if (!stop_requested && !(worker_is_joinable && worker_is_finished))
                pthread_cond_timedwait(&state_changed, &state_mutex, &deadline);
            pthread_mutex_unlock(&state_mutex);
        } else {
            sleep(1);
        }
    }

shutdown:
    pthread_mutex_lock(&state_mutex);
    pid_t command_group = command_process_group;
    pjsua_call_id call_id = call_is_active ? active_call : -1;
    pthread_cond_broadcast(&state_changed);
    pthread_mutex_unlock(&state_mutex);
    if (command_group > 0)
        stop_command_group(command_group);
    if (call_id >= 0)
        pjsua_call_hangup(call_id, 0, NULL, NULL);

    pthread_t thread;
    int join_worker = 0;
    pthread_mutex_lock(&state_mutex);
    if (worker_is_joinable) {
        thread = worker_thread;
        worker_is_joinable = 0;
        join_worker = 1;
    }
    pthread_mutex_unlock(&state_mutex);
    if (join_worker)
        pthread_join(thread, NULL);

    pjsua_destroy();
    return status == PJ_SUCCESS ? 0 : 1;
}
