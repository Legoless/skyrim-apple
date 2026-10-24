#include "input_control.h"
#include "hid.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static sky_hid_npad_state_t read_state(void) {
    uint32_t id = 0;
    sky_hid_npad_state_t state;
    sky_hid_get_full_key_state(&state, &id);
    assert(state.attributes == 1);
    return state;
}

static int state_matches(const sky_hid_npad_state_t *state, uint64_t buttons,
                         int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return state->buttons == buttons && state->lx == lx && state->ly == ly &&
        state->rx == rx && state->ry == ry;
}

static void expect_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    sky_hid_npad_state_t state = read_state();
    assert(state_matches(&state, buttons, lx, ly, rx, ry));
}

static void expect_neutral(void) {
    /* A second read also catches an edge or stick sample left pending. */
    expect_state(0, 0, 0, 0, 0);
    expect_state(0, 0, 0, 0, 0);
}

static void expect_neutral_context(const sky_input_control_t *control) {
    assert(!control->buttons && !control->lx && !control->ly && !control->rx && !control->ry);
}

static int apply(sky_input_control_t *control, const char *line) {
    return sky_input_control_apply(control, line, strlen(line));
}

static void prime_pending(sky_input_control_t *control) {
    assert(apply(control, "reset") == SKY_INPUT_CONTROL_APPLIED);
    assert(apply(control, "state 128 123 -456 789 -1000") == SKY_INPUT_CONTROL_APPLIED);
    assert(apply(control, "tap 32") == SKY_INPUT_CONTROL_APPLIED);
}

static void expect_invalid(sky_input_control_t *control, const char *line, size_t length) {
    prime_pending(control);
    assert(sky_input_control_apply(control, line, length) == SKY_INPUT_CONTROL_INVALID);
    expect_neutral_context(control);
    expect_neutral();
}

static void test_commands(void) {
    sky_input_control_t control = {0};
    assert(apply(&control, "state 18446744073709551615 -32768 32767 0 -1") == SKY_INPUT_CONTROL_APPLIED);
    assert(control.buttons == UINT64_MAX && control.lx == -32768 && control.ly == 32767);
    assert(control.rx == 0 && control.ry == -1);
    expect_state(UINT64_MAX, -32768, 32767, 0, -1);
    expect_state(UINT64_MAX, -32768, 32767, 0, -1);
    assert(apply(&control, "reset") == SKY_INPUT_CONTROL_APPLIED);
    expect_neutral_context(&control);
    expect_neutral();

    assert(apply(&control, "tap 0xffffffffffffffff") == SKY_INPUT_CONTROL_APPLIED);
    expect_neutral_context(&control);
    expect_state(UINT64_MAX, 0, 0, 0, 0);
    expect_neutral();
    assert(apply(&control, "tap 010") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(10, 0, 0, 0, 0); /* Leading zero still means decimal. */
    expect_neutral();

    assert(apply(&control, "state 0x80 12 -34 56 -78") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(0x80, 12, -34, 56, -78);
    assert(apply(&control, "tap 1") == SKY_INPUT_CONTROL_APPLIED);
    assert(control.buttons == 0x80 && control.lx == 12 && control.ly == -34);
    assert(control.rx == 56 && control.ry == -78);
    expect_state(0x81, 12, -34, 56, -78);
    expect_state(0x80, 12, -34, 56, -78);
    assert(apply(&control, "tap 0") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(0x80, 12, -34, 56, -78);
    assert(apply(&control, "tap 128") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(0x80, 12, -34, 56, -78);
    expect_state(0x80, 12, -34, 56, -78);
    assert(apply(&control, "tap 1") == SKY_INPUT_CONTROL_APPLIED);
    assert(apply(&control, "tap 2") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(0x83, 12, -34, 56, -78);
    expect_state(0x80, 12, -34, 56, -78);

    /* A tap uses its reader's held snapshot, not another context's state. */
    sky_input_control_t other = {0};
    assert(apply(&other, "state 4 -1 2 -3 4") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(4, -1, 2, -3, 4);
    assert(apply(&control, "tap 8") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(0x88, 12, -34, 56, -78);
    expect_state(0x80, 12, -34, 56, -78);
    assert(other.buttons == 4 && other.lx == -1 && other.ly == 2 && other.rx == -3 && other.ry == 4);

    prime_pending(&control);
    assert(apply(&control, "reset") == SKY_INPUT_CONTROL_APPLIED);
    expect_neutral_context(&control);
    expect_neutral();
    prime_pending(&control);
    assert(apply(&control, "quit") == SKY_INPUT_CONTROL_QUIT);
    expect_neutral_context(&control);
    expect_neutral();

    assert(apply(&control, "tap 1\n") == SKY_INPUT_CONTROL_APPLIED);
    expect_state(1, 0, 0, 0, 0);
    expect_neutral();
    puts("T_INPUT_CONTROL_COMMANDS OK masks axes held-tap independent-context reset quit");
}

static void test_invalid_commands(void) {
    sky_input_control_t control = {0};
    const char *invalid[] = {
        "", " ", "unknown", "Tap 1", "tap", "tap 1 2", "tap -1", "tap +1",
        "tap 18446744073709551616", "tap 0x10000000000000000", "tap 0x", "tap 0xg",
        "tap 1garbage", "tap 1.0", "state", "state 1 0 0 0", "state 1 0 0 0 0 0",
        "state -1 0 0 0 0", "state 18446744073709551616 0 0 0 0",
        "state 0 32768 0 0 0", "state 0 0 -32769 0 0", "state 0 0 0 32768 0",
        "state 0 0 0 0 -32769", "state 0 9999999999999999999999 0 0 0",
        "state 0 -9999999999999999999999 0 0 0", "state 0 0x10 0 0 0",
        "state 0 1.5 0 0 0", "state 0 - 0 0 0", "state 0 1x 0 0 0",
        "reset 1", "quit 1", "tap 1\nreset", "tap 1\n\n", "tap \v1", "tap \f1",
        "state 0 \v1 0 0 0", "state 0 0 \f1 0 0"
    };
    for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index++)
        expect_invalid(&control, invalid[index], strlen(invalid[index]));
    const char embedded_nul[] = {'t', 'a', 'p', ' ', '1', '\0', 'x'};
    expect_invalid(&control, embedded_nul, sizeof embedded_nul);
    expect_invalid(&control, "tap 1", SIZE_MAX);
    expect_invalid(&control, NULL, 5);
    prime_pending(&control);
    assert(sky_input_control_apply(NULL, "tap 1", 5) == SKY_INPUT_CONTROL_INVALID);
    expect_neutral();
    assert(apply(&control, "reset") == SKY_INPUT_CONTROL_APPLIED);

    char boundary[SKY_INPUT_CONTROL_LINE_MAX + 2];
    memset(boundary, ' ', sizeof boundary);
    memcpy(boundary, "tap 1", 5);
    assert(sky_input_control_apply(&control, boundary, SKY_INPUT_CONTROL_LINE_MAX) == SKY_INPUT_CONTROL_APPLIED);
    expect_state(1, 0, 0, 0, 0);
    expect_neutral();
    boundary[SKY_INPUT_CONTROL_LINE_MAX] = '\n';
    assert(sky_input_control_apply(&control, boundary, SKY_INPUT_CONTROL_LINE_MAX + 1) == SKY_INPUT_CONTROL_APPLIED);
    expect_state(1, 0, 0, 0, 0);
    expect_neutral();
    boundary[SKY_INPUT_CONTROL_LINE_MAX] = ' ';
    expect_invalid(&control, boundary, SKY_INPUT_CONTROL_LINE_MAX + 1);
    puts("T_INPUT_CONTROL_INVALID OK bounds overflow extra-token embedded-NUL newline pending-reset");
}

typedef struct {
    int fd;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int done, quits;
} reader_t;

static void on_quit(void *context) {
    reader_t *reader = context;
    assert(!pthread_mutex_lock(&reader->lock));
    reader->quits++;
    assert(!pthread_cond_broadcast(&reader->changed));
    assert(!pthread_mutex_unlock(&reader->lock));
}

static void *reader_main(void *context) {
    reader_t *reader = context;
    sky_input_control_read(reader->fd, on_quit, reader);
    assert(!pthread_mutex_lock(&reader->lock));
    reader->done = 1;
    assert(!pthread_cond_broadcast(&reader->changed));
    assert(!pthread_mutex_unlock(&reader->lock));
    return NULL;
}

static void init_reader(reader_t *reader, int fd) {
    memset(reader, 0, sizeof *reader);
    reader->fd = fd;
    assert(!pthread_mutex_init(&reader->lock, NULL));
    assert(!pthread_cond_init(&reader->changed, NULL));
}

static void start_reader(reader_t *reader, int fd) {
    init_reader(reader, fd);
    assert(!pthread_create(&reader->thread, NULL, reader_main, reader));
}

static void finish_reader(reader_t *reader, int expected_quits) {
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 5;
    assert(!pthread_mutex_lock(&reader->lock));
    while (!reader->done)
        assert(!pthread_cond_timedwait(&reader->changed, &reader->lock, &deadline));
    assert(reader->quits == expected_quits);
    assert(!pthread_mutex_unlock(&reader->lock));
    assert(!pthread_join(reader->thread, NULL));
    assert(fcntl(reader->fd, F_GETFD) >= 0); /* The caller still owns the descriptor. */
    assert(!pthread_cond_destroy(&reader->changed));
    assert(!pthread_mutex_destroy(&reader->lock));
}

static void write_bytes(int fd, const void *data, size_t length) {
    const char *bytes = data;
    while (length) {
        ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        bytes += count;
        length -= (size_t)count;
    }
}

static void write_line(int fd, const char *line) {
    write_bytes(fd, line, strlen(line));
}

static void await_state(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    struct timespec start, current;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    for (;;) {
        sky_hid_npad_state_t state = read_state();
        if (state_matches(&state, buttons, lx, ly, rx, ry)) return;
        assert(!clock_gettime(CLOCK_MONOTONIC, &current));
        assert(current.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

static void test_reader(void) {
    int fds[2];
    reader_t reader;
    sky_hid_set_host_state(0, 0, 0, 0, 0);
    assert(!pipe(fds));
    start_reader(&reader, fds[0]);
    write_line(fds[1], "sta");
    write_line(fds[1], "te 4 1 -2 3 -4\n");
    await_state(4, 1, -2, 3, -4);
    write_line(fds[1], "tap 8\n");
    await_state(12, 1, -2, 3, -4);
    await_state(4, 1, -2, 3, -4);
    /* EOF must release held input and discard an unterminated command. */
    write_line(fds[1], "quit");
    assert(!close(fds[1]));
    finish_reader(&reader, 0);
    expect_neutral();
    assert(!close(fds[0]));

    assert(!pipe(fds));
    start_reader(&reader, fds[0]);
    write_line(fds[1], "state 32 -1 -2 -3 -4\n");
    await_state(32, -1, -2, -3, -4);
    write_line(fds[1], "tap 64\nquit\nstate 128 9 8 7 6\nquit\n");
    finish_reader(&reader, 1);
    expect_neutral();
    assert(!close(fds[1]));
    assert(!close(fds[0]));

    /* The over-limit byte resets immediately, before a newline arrives. */
    assert(!pipe(fds));
    start_reader(&reader, fds[0]);
    write_line(fds[1], "state 2 11 12 13 14\n");
    await_state(2, 11, 12, 13, 14);
    char oversized[SKY_INPUT_CONTROL_LINE_MAX + 1];
    memset(oversized, 'x', sizeof oversized);
    write_bytes(fds[1], oversized, sizeof oversized);
    await_state(0, 0, 0, 0, 0);
    /* A valid-looking tail belongs to the discarded line. */
    write_line(fds[1], "quit\nstate 8 21 22 23 24\n");
    await_state(8, 21, 22, 23, 24);
    assert(!close(fds[1]));
    finish_reader(&reader, 0);
    expect_neutral();
    assert(!close(fds[0]));

    assert(!pipe(fds));
    start_reader(&reader, fds[0]);
    write_line(fds[1], "state 16 -11 -12 -13 -14\n");
    await_state(16, -11, -12, -13, -14);
    const char nul = 0;
    write_bytes(fds[1], &nul, 1);
    await_state(0, 0, 0, 0, 0);
    write_line(fds[1], "quit\nstate 64 -21 -22 -23 -24\n");
    await_state(64, -21, -22, -23, -24);
    assert(!close(fds[1]));
    finish_reader(&reader, 0);
    expect_neutral();
    assert(!close(fds[0]));

    /* A read error also clears both held and pending input without quitting. */
    init_reader(&reader, -1);
    sky_input_control_t control = {0};
    prime_pending(&control);
    sky_input_control_read(-1, on_quit, &reader);
    assert(reader.quits == 0);
    expect_neutral();
    assert(!pthread_cond_destroy(&reader.changed));
    assert(!pthread_mutex_destroy(&reader.lock));
    puts("T_INPUT_CONTROL_READER OK fragmented EOF-reset quit-once bounded-drain NUL-drain read-error descriptor-ownership");
}

static void test_disabled_start(void) {
    const char *value = getenv("SKY_INPUT_STDIN");
    char *saved = value ? strdup(value) : NULL;
    assert(!value || saved);
    assert(!unsetenv("SKY_INPUT_STDIN"));
    assert(sky_input_control_start(NULL, NULL) == 0);
    const char *disabled[] = {"", "0", "true", "01", "1 ", " 1"};
    for (size_t index = 0; index < sizeof disabled / sizeof disabled[0]; index++) {
        assert(!setenv("SKY_INPUT_STDIN", disabled[index], 1));
        assert(sky_input_control_start(NULL, NULL) == 0);
    }
    if (saved) {
        assert(!setenv("SKY_INPUT_STDIN", saved, 1));
        free(saved);
    } else {
        assert(!unsetenv("SKY_INPUT_STDIN"));
    }
    puts("T_INPUT_CONTROL_START OK exact-environment-opt-in");
}

static void read_ack(int fd, char expected) {
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    int result;
    do result = poll(&descriptor, 1, 5000); while (result < 0 && errno == EINTR);
    assert(result == 1 && (descriptor.revents & POLLIN));
    char value;
    ssize_t count;
    do count = read(fd, &value, 1); while (count < 0 && errno == EINTR);
    assert(count == 1 && value == expected);
}

static void await_closed(int fd) {
    struct timespec start, current;
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    for (;;) {
        if (fcntl(fd, F_GETFD) < 0) {
            assert(errno == EBADF);
            return;
        }
        assert(!clock_gettime(CLOCK_MONOTONIC, &current));
        assert(current.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

static void enabled_start_child(int input_fd, int ack_fd, int quit) {
    assert(dup2(input_fd, STDIN_FILENO) == STDIN_FILENO);
    if (input_fd != STDIN_FILENO) assert(!close(input_fd));
    assert(!setenv("SKY_INPUT_STDIN", "1", 1));
    reader_t callback;
    init_reader(&callback, STDIN_FILENO);
    /* dup() reserves the lowest free descriptor. With no other fd operations,
     * start() must use this slot for its owned copy of stdin. */
    int reader_fd = dup(STDIN_FILENO);
    assert(reader_fd >= 0);
    assert(!close(reader_fd));
    assert(sky_input_control_start(on_quit, &callback) == 1);
    assert(fcntl(reader_fd, F_GETFD) >= 0);
    write_line(ack_fd, "R");
    await_state(4, 1, -2, 3, -4);
    write_line(ack_fd, "S");
    if (quit) {
        await_state(0, 0, 0, 0, 0);
        write_line(ack_fd, "Z");
        await_state(8, -5, 6, -7, 8);
        write_line(ack_fd, "T");
    }
    /* Closing its duplicate proves the detached reader has returned, so the
     * callback count and retained stdin descriptor can be checked without a race. */
    await_closed(reader_fd);
    assert(!pthread_mutex_lock(&callback.lock));
    assert(callback.quits == quit);
    assert(!pthread_mutex_unlock(&callback.lock));
    expect_neutral();
    assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
    assert(!pthread_cond_destroy(&callback.changed));
    assert(!pthread_mutex_destroy(&callback.lock));
    write_line(ack_fd, "D");
    _exit(0);
}

static void test_enabled_start(void) {
    for (int quit = 0; quit <= 1; quit++) {
        int input[2], ack[2];
        assert(!pipe(input));
        assert(!pipe(ack));
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            assert(!close(input[1]));
            assert(!close(ack[0]));
            enabled_start_child(input[0], ack[1], quit);
        }
        assert(!close(input[0]));
        assert(!close(ack[1]));
        read_ack(ack[0], 'R');
        write_line(input[1], "state 4 1 -2 3 -4\n");
        read_ack(ack[0], 'S');
        if (quit) {
            write_line(input[1], "reset\n");
            read_ack(ack[0], 'Z');
            write_line(input[1], "state 8 -5 6 -7 8\n");
            read_ack(ack[0], 'T');
            write_line(input[1], "quit\nstate 128 9 8 7 6\nquit\n");
        } else {
            assert(!close(input[1]));
        }
        read_ack(ack[0], 'D');
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        if (quit) assert(!close(input[1]));
        assert(!close(ack[0]));
    }
    puts("T_INPUT_CONTROL_ENABLED OK detached-reader stdin-duplicate state reset quit-once EOF-no-quit");
}

int main(void) {
    assert(!sky_hid_initialize_npad());
    sky_hid_set_supported_npad_style_set(1);
    const uint32_t id = 0;
    sky_hid_set_supported_npad_id_type(&id, 1);
    test_commands();
    test_invalid_commands();
    test_reader();
    test_disabled_start();
    test_enabled_start();
    puts("T_INPUT_CONTROL OK");
    return 0;
}
