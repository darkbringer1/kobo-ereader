#define _GNU_SOURCE
#include "kobodash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

#define KD_MAX_INPUT_FDS 6
#define HOME_LONG_PRESS_MS  1800u
#define POWER_LONG_PRESS_MS 2400u
#define TOUCH_LONG_TAP_MS   380u
#define LONG_TAP_MAX_MOVE   28
#define SWIPE_MIN_X 75
#define SWIPE_MIN_Y 90

struct InputState {
    Config cfg;
    int fds[KD_MAX_INPUT_FDS];
    bool is_touch[KD_MAX_INPUT_FDS];
    int raw_min_x[KD_MAX_INPUT_FDS];
    int raw_max_x[KD_MAX_INPUT_FDS];
    int raw_min_y[KD_MAX_INPUT_FDS];
    int raw_max_y[KD_MAX_INPUT_FDS];
    int fd_count;

    /* Touch tracking state */
    bool touch_active;
    bool touch_long_fired;
    bool pending_down;
    bool pending_up;
    bool have_raw_x;
    bool have_raw_y;
    int raw_x;
    int raw_y;
    int start_x;
    int start_y;
    int last_x;
    int last_y;
    uint64_t touch_down_ms;

    /* Physical button state */
    bool home_down;
    bool home_long_fired;
    uint64_t home_down_ms;
    bool power_down;
    bool power_long_fired;
    uint64_t power_down_ms;
};

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void input_transform_coords(const Config *cfg, int raw_x, int raw_y,
                            int raw_max_x, int raw_max_y, int *sx, int *sy) {
    int x = raw_x;
    int y = raw_y;

    /* If kernel reported non-screen raw range (e.g. 0..4095), scale proportionally */
    int target_raw_x = cfg->touch_swap_xy ? (KD_HEIGHT - 1) : (KD_WIDTH - 1);
    int target_raw_y = cfg->touch_swap_xy ? (KD_WIDTH - 1) : (KD_HEIGHT - 1);

    if (raw_max_x > 0 && abs(raw_max_x - target_raw_x) > 32) {
        x = (int)((long long)x * target_raw_x / raw_max_x);
    }
    if (raw_max_y > 0 && abs(raw_max_y - target_raw_y) > 32) {
        y = (int)((long long)y * target_raw_y / raw_max_y);
    }

    if (cfg->touch_swap_xy) {
        int tmp = x;
        x = y;
        y = tmp;
    }
    if (cfg->touch_mirror_x) {
        x = (KD_WIDTH - 1) - x;
    }
    if (cfg->touch_mirror_y) {
        y = (KD_HEIGHT - 1) - y;
    }

    *sx = clamp_int(x, 0, KD_WIDTH - 1);
    *sy = clamp_int(y, 0, KD_HEIGHT - 1);
}

#ifdef __linux__
static bool test_bit(const unsigned long *arr, int bit) {
    return (arr[bit / (int)(8 * sizeof(unsigned long))] &
            (1UL << (bit % (int)(8 * sizeof(unsigned long))))) != 0;
}

InputState *input_open(const Config *cfg) {
    InputState *in = calloc(1, sizeof(*in));
    if (!in) return NULL;
    in->cfg = *cfg;

    for (int i = 0; i < 8 && in->fd_count < KD_MAX_INPUT_FDS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long evbits[(EV_MAX / (8 * sizeof(unsigned long))) + 2];
        memset(evbits, 0, sizeof(evbits));
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
            close(fd);
            continue;
        }

        bool has_abs = test_bit(evbits, EV_ABS);
        bool has_key = test_bit(evbits, EV_KEY);
        if (!has_abs && !has_key) {
            close(fd);
            continue;
        }

        int slot = in->fd_count++;
        in->fds[slot] = fd;
        in->is_touch[slot] = false;

        if (has_abs) {
            unsigned long absbits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2];
            memset(absbits, 0, sizeof(absbits));
            if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0) {
                if (test_bit(absbits, ABS_X) && test_bit(absbits, ABS_Y)) {
                    in->is_touch[slot] = true;
                    struct input_absinfo ai;
                    if (ioctl(fd, EVIOCGABS(ABS_X), &ai) == 0) {
                        in->raw_min_x[slot] = ai.minimum;
                        in->raw_max_x[slot] = ai.maximum;
                    }
                    if (ioctl(fd, EVIOCGABS(ABS_Y), &ai) == 0) {
                        in->raw_min_y[slot] = ai.minimum;
                        in->raw_max_y[slot] = ai.maximum;
                    }
                } else if (test_bit(absbits, ABS_MT_POSITION_X) && test_bit(absbits, ABS_MT_POSITION_Y)) {
                    in->is_touch[slot] = true;
                    struct input_absinfo ai;
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) {
                        in->raw_min_x[slot] = ai.minimum;
                        in->raw_max_x[slot] = ai.maximum;
                    }
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) {
                        in->raw_min_y[slot] = ai.minimum;
                        in->raw_max_y[slot] = ai.maximum;
                    }
                }
            }
        }
    }
    return in;
}

void input_update_config(InputState *in, const Config *cfg) {
    if (in && cfg) {
        in->cfg = *cfg;
    }
}

void input_close(InputState *in) {
    if (!in) return;
    for (int i = 0; i < in->fd_count; i++) {
        if (in->fds[i] >= 0) close(in->fds[i]);
    }
    free(in);
}

int input_get_fds(const InputState *in, int *fds, int max_fds) {
    if (!in) return 0;
    int n = in->fd_count < max_fds ? in->fd_count : max_fds;
    for (int i = 0; i < n; i++) fds[i] = in->fds[i];
    return n;
}

int input_poll_timeout_ms(const InputState *in, uint64_t now_ms) {
    if (!in) return -1;
    int best = -1;
    if (in->home_down && !in->home_long_fired) {
        uint64_t elapsed = (now_ms >= in->home_down_ms) ? (now_ms - in->home_down_ms) : 0;
        int rem = (elapsed >= HOME_LONG_PRESS_MS) ? 0 : (int)(HOME_LONG_PRESS_MS - elapsed);
        best = rem;
    }
    if (in->power_down && !in->power_long_fired) {
        uint64_t elapsed = (now_ms >= in->power_down_ms) ? (now_ms - in->power_down_ms) : 0;
        int rem = (elapsed >= POWER_LONG_PRESS_MS) ? 0 : (int)(POWER_LONG_PRESS_MS - elapsed);
        if (best < 0 || rem < best) best = rem;
    }
    if (in->touch_active && !in->touch_long_fired) {
        int dx = abs(in->last_x - in->start_x);
        int dy = abs(in->last_y - in->start_y);
        if (dx <= LONG_TAP_MAX_MOVE && dy <= LONG_TAP_MAX_MOVE) {
            uint64_t elapsed = (now_ms >= in->touch_down_ms) ? (now_ms - in->touch_down_ms) : 0;
            int rem = (elapsed >= TOUCH_LONG_TAP_MS) ? 0 : (int)(TOUCH_LONG_TAP_MS - elapsed);
            if (best < 0 || rem < best) best = rem;
        }
    }
    return best;
}

bool input_check_timers(InputState *in, uint64_t now_ms, InputEvent *out) {
    if (!in || !out) return false;
    if (in->home_down && !in->home_long_fired) {
        if (now_ms >= in->home_down_ms && (now_ms - in->home_down_ms) >= HOME_LONG_PRESS_MS) {
            in->home_long_fired = true;
            out->type = INPUT_HOME_LONG;
            out->x = 0;
            out->y = 0;
            return true;
        }
    }
    if (in->power_down && !in->power_long_fired) {
        if (now_ms >= in->power_down_ms && (now_ms - in->power_down_ms) >= POWER_LONG_PRESS_MS) {
            in->power_long_fired = true;
            out->type = INPUT_POWER_LONG;
            out->x = 0;
            out->y = 0;
            return true;
        }
    }
    if (in->touch_active && !in->touch_long_fired) {
        int dx = abs(in->last_x - in->start_x);
        int dy = abs(in->last_y - in->start_y);
        if (dx <= LONG_TAP_MAX_MOVE && dy <= LONG_TAP_MAX_MOVE &&
            now_ms >= in->touch_down_ms && (now_ms - in->touch_down_ms) >= TOUCH_LONG_TAP_MS) {
            in->touch_long_fired = true;
            out->type = INPUT_LONG_TAP;
            out->x = in->start_x;
            out->y = in->start_y;
            return true;
        }
    }
    return false;
}

static int find_slot(const InputState *in, int fd) {
    for (int i = 0; i < in->fd_count; i++) {
        if (in->fds[i] == fd) return i;
    }
    return -1;
}

bool input_read_event(InputState *in, int ready_fd, uint64_t now_ms, InputEvent *out) {
    if (!in || !out) return false;
    out->type = INPUT_NONE;
    int slot = find_slot(in, ready_fd);
    if (slot < 0) return false;

    struct input_event ev;
    while (read(ready_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_KEY) {
            if (ev.code == KEY_HOME) {
                if (ev.value == 1) {
                    in->home_down = true;
                    in->home_long_fired = false;
                    in->home_down_ms = now_ms;
                } else if (ev.value == 0 && in->home_down) {
                    in->home_down = false;
                    if (!in->home_long_fired) {
                        uint64_t held = (now_ms >= in->home_down_ms) ? (now_ms - in->home_down_ms) : 0;
                        out->type = (held >= HOME_LONG_PRESS_MS) ? INPUT_HOME_LONG : INPUT_HOME_SHORT;
                        out->x = 0;
                        out->y = 0;
                        return true;
                    }
                }
            } else if (ev.code == KEY_POWER) {
                if (ev.value == 1) {
                    in->power_down = true;
                    in->power_long_fired = false;
                    in->power_down_ms = now_ms;
                } else if (ev.value == 0 && in->power_down) {
                    in->power_down = false;
                    if (!in->power_long_fired) {
                        uint64_t held = (now_ms >= in->power_down_ms) ? (now_ms - in->power_down_ms) : 0;
                        out->type = (held >= POWER_LONG_PRESS_MS) ? INPUT_POWER_LONG : INPUT_POWER_SHORT;
                        out->x = 0;
                        out->y = 0;
                        return true;
                    }
                }
            } else if (ev.code == BTN_TOUCH) {
                if (ev.value != 0) in->pending_down = true;
                else in->pending_up = true;
            }
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                in->raw_x = ev.value - in->raw_min_x[slot];
                in->have_raw_x = true;
            } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                in->raw_y = ev.value - in->raw_min_y[slot];
                in->have_raw_y = true;
            } else if (ev.code == ABS_PRESSURE) {
                if (ev.value > 0) in->pending_down = true;
                else in->pending_up = true;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0) in->pending_down = true;
                else in->pending_up = true;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int max_x = in->raw_max_x[slot] - in->raw_min_x[slot];
            int max_y = in->raw_max_y[slot] - in->raw_min_y[slot];
            int sx = in->last_x;
            int sy = in->last_y;
            if (in->have_raw_x || in->have_raw_y) {
                input_transform_coords(&in->cfg, in->raw_x, in->raw_y, max_x, max_y, &sx, &sy);
            }

            if (in->pending_down && !in->touch_active) {
                in->touch_active = true;
                in->touch_long_fired = false;
                in->start_x = sx;
                in->start_y = sy;
                in->last_x = sx;
                in->last_y = sy;
                in->touch_down_ms = now_ms;
            } else if (in->touch_active && !in->pending_up) {
                if (in->have_raw_x || in->have_raw_y) {
                    in->last_x = sx;
                    in->last_y = sy;
                }
            }

            if (in->pending_up && in->touch_active) {
                bool was_long = in->touch_long_fired;
                in->touch_active = false;
                in->touch_long_fired = false;
                in->pending_down = false;
                in->pending_up = false;
                in->have_raw_x = false;
                in->have_raw_y = false;

                if (was_long) {
                    return false;
                }

                int dx = in->last_x - in->start_x;
                int dy = in->last_y - in->start_y;
                if (abs(dx) >= SWIPE_MIN_X && abs(dx) * 10 > abs(dy) * 13) {
                    out->type = (dx < 0) ? INPUT_SWIPE_LEFT : INPUT_SWIPE_RIGHT;
                    out->x = in->last_x;
                    out->y = in->last_y;
                } else if (abs(dy) >= SWIPE_MIN_Y && abs(dy) * 10 > abs(dx) * 13) {
                    out->type = (dy < 0) ? INPUT_SWIPE_UP : INPUT_SWIPE_DOWN;
                    out->x = in->last_x;
                    out->y = in->last_y;
                } else {
                    uint64_t held = (now_ms >= in->touch_down_ms) ? (now_ms - in->touch_down_ms) : 0;
                    out->type = (held >= TOUCH_LONG_TAP_MS &&
                                 abs(dx) <= LONG_TAP_MAX_MOVE && abs(dy) <= LONG_TAP_MAX_MOVE)
                                    ? INPUT_LONG_TAP : INPUT_TAP;
                    out->x = in->last_x;
                    out->y = in->last_y;
                }
                return true;
            }
            in->pending_down = false;
            in->pending_up = false;
            in->have_raw_x = false;
            in->have_raw_y = false;
        }
    }
    return false;
}

bool input_drain_wakeup(InputState *in) {
    if (!in) return false;
    bool saw_user_key = false;
    struct input_event ev;
    for (int i = 0; i < in->fd_count; i++) {
        if (in->fds[i] < 0) continue;
        while (read(in->fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && (ev.code == KEY_POWER || ev.code == KEY_HOME)) {
                saw_user_key = true;
            }
        }
    }
    in->touch_active = false;
    in->touch_long_fired = false;
    in->pending_down = false;
    in->pending_up = false;
    in->home_down = false;
    in->home_long_fired = false;
    in->power_down = false;
    in->power_long_fired = false;
    return saw_user_key;
}

#else /* Non-Linux host stub for unit tests */
InputState *input_open(const Config *cfg) {
    InputState *in = calloc(1, sizeof(*in));
    if (in && cfg) in->cfg = *cfg;
    return in;
}
void input_update_config(InputState *in, const Config *cfg) {
    if (in && cfg) in->cfg = *cfg;
}
void input_close(InputState *in) { free(in); }
int input_get_fds(const InputState *in, int *fds, int max_fds) { (void)in; (void)fds; (void)max_fds; return 0; }
int input_poll_timeout_ms(const InputState *in, uint64_t now_ms) { (void)in; (void)now_ms; return -1; }
bool input_read_event(InputState *in, int ready_fd, uint64_t now_ms, InputEvent *out) {
    (void)in; (void)ready_fd; (void)now_ms; (void)out; return false;
}
bool input_check_timers(InputState *in, uint64_t now_ms, InputEvent *out) {
    (void)in; (void)now_ms; (void)out; return false;
}
bool input_drain_wakeup(InputState *in) {
    (void)in; return false;
}
#endif
