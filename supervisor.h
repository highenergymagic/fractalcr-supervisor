/* SPDX-License-Identifier: MIT */
#ifndef FRACTAL_SUPERVISOR_H
#define FRACTAL_SUPERVISOR_H
#include <stddef.h>
#include <sys/types.h>
enum service_state { SV_STOPPED, SV_STARTING, SV_RUNNING, SV_STOPPING, SV_BACKOFF, SV_FAILED, SV_COMPLETE };
struct service {
    const char *name, *log_path;
    char *const *argv;
    int (*probe)(void); /* positive: healthy, zero: failed, negative: pending */
    void (*event)(struct service *, const char *, const char *);
    enum service_state state;
    pid_t pid, group;
    int log_fd, wanted;
    unsigned starts, failures, health_misses;
    long long deadline, next_probe, stable_since;
    unsigned start_ms, stop_ms, backoff_ms, healthy_ms, max_failures;
    int notify, notify_fd, notified, oneshot, console, hold;
};
long long monotonic_ms(void);
const char *service_state_name(enum service_state state);
void service_init(struct service *s);
void service_start(struct service *s);
void service_stop(struct service *s);
void service_restart(struct service *s);
void service_tick(struct service *s);
/* Call only when no synchronous helper is outstanding. Reaps PID 1 adoptees. */
void service_reap_orphans(struct service *s);
int service_reap_children(struct service *s, pid_t watched, int *status_out);
void services_reap(struct service *services, size_t count);
void service_notify(void);
void log_append(const char *path, const void *data, size_t length);
#endif
