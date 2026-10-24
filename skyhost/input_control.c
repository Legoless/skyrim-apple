#include "input_control.h"
#include "hid.h"
#include "skyhost.h"

#include <errno.h>
#include <unistd.h>

static void reset_input(sky_input_control_t *control) {
    if (control) *control = (sky_input_control_t){0};
    sky_hid_set_host_state(0, 0, 0, 0, 0);
}

static int invalid_command(sky_input_control_t *control) {
    reset_input(control);
    LOG("INPUT_CONTROL_ERROR reason=invalid-command input=reset");
    return SKY_INPUT_CONTROL_INVALID;
}

static int parse_mask(const char *token, uint64_t *out) {
    if (*token < '0' || *token > '9') return 0;
    char *end = NULL;
    int base = token[0] == '0' && (token[1] == 'x' || token[1] == 'X') ? 16 : 10;
    errno = 0;
    unsigned long long value = strtoull(token, &end, base);
    if (errno || end == token || *end) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int parse_stick(const char *token, int32_t *out) {
    const char *digit = *token == '-' || *token == '+' ? token + 1 : token;
    if (*digit < '0' || *digit > '9') return 0;
    char *end = NULL;
    errno = 0;
    long long value = strtoll(token, &end, 10);
    if (errno || end == token || *end || value < -32768 || value > 32767) return 0;
    *out = (int32_t)value;
    return 1;
}

int sky_input_control_apply(sky_input_control_t *control, const char *line, size_t length) {
    if (!control || !line) return invalid_command(control);
    if (length > SKY_INPUT_CONTROL_LINE_MAX + 1) return invalid_command(control);
    if (length && line[length - 1] == '\n') length--;
    if (!length || length > SKY_INPUT_CONTROL_LINE_MAX || memchr(line, 0, length) || memchr(line, '\n', length))
        return invalid_command(control);
    char buffer[SKY_INPUT_CONTROL_LINE_MAX + 1];
    memcpy(buffer, line, length);
    buffer[length] = 0;
    char *tokens[6];
    unsigned count = 0;
    char *at = buffer;
    while (*at) {
        while (*at == ' ' || *at == '\t' || *at == '\r') at++;
        if (!*at) break;
        if (count == sizeof tokens / sizeof tokens[0]) return invalid_command(control);
        tokens[count++] = at;
        while (*at && *at != ' ' && *at != '\t' && *at != '\r') at++;
        if (*at) *at++ = 0;
    }
    if (!count) return invalid_command(control);
    if (!strcmp(tokens[0], "reset") && count == 1) {
        reset_input(control);
        LOG("INPUT_CONTROL_RESET reason=command");
        return SKY_INPUT_CONTROL_APPLIED;
    }
    if (!strcmp(tokens[0], "quit") && count == 1) {
        reset_input(control);
        return SKY_INPUT_CONTROL_QUIT;
    }
    uint64_t mask;
    if (!strcmp(tokens[0], "tap") && count == 2 && parse_mask(tokens[1], &mask)) {
        sky_hid_queue_host_tap(control->buttons, control->lx, control->ly, control->rx, control->ry, mask);
        LOG("INPUT_CONTROL_TAP buttons=%#llx", (unsigned long long)mask);
        return SKY_INPUT_CONTROL_APPLIED;
    }
    sky_input_control_t next = {0};
    if (!strcmp(tokens[0], "state") && count == 6 && parse_mask(tokens[1], &next.buttons) &&
        parse_stick(tokens[2], &next.lx) && parse_stick(tokens[3], &next.ly) &&
        parse_stick(tokens[4], &next.rx) && parse_stick(tokens[5], &next.ry)) {
        *control = next;
        sky_hid_queue_host_state(next.buttons, next.lx, next.ly, next.rx, next.ry);
        LOG("INPUT_CONTROL_STATE buttons=%#llx sticks=%d,%d,%d,%d",
            (unsigned long long)next.buttons, next.lx, next.ly, next.rx, next.ry);
        return SKY_INPUT_CONTROL_APPLIED;
    }
    return invalid_command(control);
}

void sky_input_control_read(int fd, void (*on_quit)(void *), void *context) {
    sky_input_control_t control = {0};
    char line[SKY_INPUT_CONTROL_LINE_MAX];
    size_t length = 0;
    int discard = 0;
    for (;;) {
        unsigned char bytes[128];
        ssize_t size = read(fd, bytes, sizeof bytes);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) {
            reset_input(&control);
            LOG("INPUT_CONTROL_RESET reason=%s", size ? "read-error" : "eof");
            return;
        }
        for (ssize_t index = 0; index < size; index++) {
            unsigned char value = bytes[index];
            if (value == '\n') {
                if (!discard && sky_input_control_apply(&control, line, length) == SKY_INPUT_CONTROL_QUIT) {
                    LOG("INPUT_CONTROL_QUIT");
                    if (on_quit) on_quit(context);
                    return;
                }
                length = 0;
                discard = 0;
            } else if (!discard) {
                if (!value || length == sizeof line) {
                    invalid_command(&control);
                    discard = 1;
                    length = 0;
                } else line[length++] = (char)value;
            }
        }
    }
}

typedef struct {
    int fd;
    void (*on_quit)(void *);
    void *context;
} reader_arguments_t;

static void *input_reader(void *argument) {
    reader_arguments_t arguments = *(reader_arguments_t *)argument;
    free(argument);
    sky_input_control_read(arguments.fd, arguments.on_quit, arguments.context);
    close(arguments.fd);
    return NULL;
}

int sky_input_control_start(void (*on_quit)(void *), void *context) {
    const char *enabled = getenv("SKY_INPUT_STDIN");
    if (!enabled || strcmp(enabled, "1")) return 0;
    int fd = dup(STDIN_FILENO);
    if (fd < 0) return -1;
    reader_arguments_t *arguments = malloc(sizeof *arguments);
    if (!arguments) { close(fd); return -1; }
    *arguments = (reader_arguments_t){fd, on_quit, context};
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes)) { close(fd); free(arguments); return -1; }
    int result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (!result) result = pthread_create(&thread, &attributes, input_reader, arguments);
    pthread_attr_destroy(&attributes);
    if (result) { close(fd); free(arguments); return -1; }
    LOG("INPUT_CONTROL_READY source=stdin commands=tap,state,reset,quit");
    return 1;
}
