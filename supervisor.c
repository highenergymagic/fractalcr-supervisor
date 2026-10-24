/* SPDX-License-Identifier: MIT -- small, single-threaded appliance supervisor. */
#define _GNU_SOURCE
#include "supervisor.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void log_append(const char *path, const void *data, size_t length)
{
    struct stat st;
    if (!stat(path, &st) && (size_t)st.st_size + length > 262144) {
        char previous[512];
        snprintf(previous, sizeof(previous), "%s.1", path);
        if (rename(path, previous)) return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    const char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n; length -= (size_t)n;
    }
    close(fd);
}

const char *service_state_name(enum service_state state)
{
    static const char *names[] = {"stopped", "starting", "running", "stopping", "backoff", "failed", "complete"};
    return names[state];
}

static void event(struct service *s, const char *code, const char *text)
{
    if (s->event) s->event(s, code, text);
}

void service_init(struct service *s)
{
    s->state = SV_STOPPED;
    s->log_fd = -1;
    s->notify_fd = -1;
    if (!s->start_ms) s->start_ms = 60000;
    if (!s->stop_ms) s->stop_ms = 10000;
    if (!s->backoff_ms) s->backoff_ms = 1000;
    if (!s->healthy_ms) s->healthy_ms = 60000;
    if (!s->max_failures) s->max_failures = 4;
}

static void drain(struct service *s)
{
    char buffer[4096];
    for (int i = 0; s->log_fd >= 0 && i < 32; i++) {
        ssize_t n = read(s->log_fd, buffer, sizeof(buffer));
        if (n > 0) log_append(s->log_path, buffer, (size_t)n);
        else if (!n) { close(s->log_fd); s->log_fd = -1; }
        else break;
    }
}

static void stopping(struct service *s)
{
    s->stop_pending = 0;
    if (s->group > 0) kill(s->terminate_leader && s->pid > 0 ? s->pid : -s->group, SIGTERM);
    s->state = SV_STOPPING;
    s->deadline = monotonic_ms() + s->stop_ms;
}

static void failed(struct service *s, const char *reason)
{
    /* Only a sustained healthy interval clears the failure budget. Slow
     * readiness timeouts must not evade the limit by outlasting a window. */
    s->failures++;
    event(s, "3-SERVICE_EXIT", reason);
    stopping(s);
}

static void launch(struct service *s)
{
    int pipes[2], notification[2];
    if (pipe2(pipes, O_CLOEXEC)) { failed(s, "Service output channel unavailable."); return; }
    if (pipe2(notification, O_CLOEXEC | O_NONBLOCK)) {
        close(pipes[0]); close(pipes[1]); failed(s, "Readiness channel unavailable."); return;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipes[0]); close(pipes[1]); close(notification[0]); close(notification[1]);
        failed(s, "Cannot start service process."); return;
    }
    if (!child) {
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        int null = open("/dev/null", O_RDONLY);
        if (null < 0 || dup2(null, 0) < 0 || dup2(pipes[1], 1) < 0 || dup2(pipes[1], 2) < 0) _exit(126);
        close(null); close(pipes[0]); close(pipes[1]);
        close(notification[0]);
        char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%d", notification[1]);
        if (fcntl(notification[1], F_SETFD, 0) || setenv("FCR_READY_FD", descriptor, 1)) _exit(126);
        if (s->console) {
            int tty = open("/dev/console", O_RDWR);
            if (tty < 0 || dup2(tty, 0) < 0 || dup2(tty, 1) < 0) _exit(126);
            if (tty > 2) close(tty);
        }
        execv(s->argv[0], s->argv);
        perror(s->argv[0]); _exit(127);
    }
    close(pipes[1]);
    close(notification[1]); s->notify_fd = notification[0]; s->notified = 0;
    setpgid(child, child);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    s->log_fd = pipes[0]; s->pid = child; s->group = child;
    s->starts++; s->health_misses = 0;
    s->stop_pending = 0;
    s->state = SV_STARTING;
    s->deadline = monotonic_ms() + s->start_ms;
    s->next_probe = 0;
}

void service_start(struct service *s)
{
    if (s->state != SV_STOPPED && s->state != SV_FAILED && s->state != SV_COMPLETE) return;
    s->wanted = 1; s->failures = 0;
    launch(s);
}

void service_stop(struct service *s)
{
    s->wanted = 0;
    if (s->finish_start_before_stop && s->state == SV_STARTING && s->pid > 0) {
        if (!s->stop_pending) event(s, "5-SERVICE_STOP_PENDING", "Finishing initialization before stopping.");
        s->stop_pending = 1;
        return;
    }
    if (s->group > 0) stopping(s);
    else s->state = SV_STOPPED;
}

void service_restart(struct service *s)
{
    if (s->group <= 0) { service_start(s); return; }
    s->wanted = 1; s->failures = 0;
    event(s, "5-SERVICE_RESTART", "Restart requested.");
    if (s->finish_start_before_stop && s->state == SV_STARTING && s->pid > 0) {
        s->stop_pending = 1;
        return;
    }
    stopping(s);
}

static void exited(struct service *s, int status)
{
    s->pid = 0;
    if (s->oneshot && s->state != SV_STOPPING && WIFEXITED(status) && !WEXITSTATUS(status)) {
        s->wanted = 0;
        stopping(s);
        s->notified = 2; /* Report completion only after descendants are gone. */
        return;
    }
    if (s->state != SV_STOPPING) {
        char text[128];
        if (WIFSIGNALED(status)) snprintf(text, sizeof(text), "Service terminated by signal %d.", WTERMSIG(status));
        else snprintf(text, sizeof(text), "Service exited with status %d.", WEXITSTATUS(status));
        failed(s, text);
    }
}

int service_reap_children(struct service *s, pid_t watched, int *status_out)
{
    int found = 0;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == s->pid) exited(s, status);
        else if (pid == watched) { *status_out = status; found = 1; }
    }
    return found;
}

void service_reap_orphans(struct service *s) { (void)service_reap_children(s, 0, NULL); }

void services_reap(struct service *services, size_t count)
{
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        for (size_t i = 0; i < count; i++)
            if (pid == services[i].pid) { exited(&services[i], status); break; }
}

void service_notify(void)
{
    const char *text = getenv("FCR_READY_FD");
    if (!text) return;
    int fd = atoi(text);
    if (fd > 2) { if (write(fd, "R", 1) != 1) _exit(125); close(fd); }
    unsetenv("FCR_READY_FD");
}

void service_tick(struct service *s)
{
    long long now = monotonic_ms();
    drain(s);
    if (s->notify_fd >= 0) {
        char c;
        ssize_t n = read(s->notify_fd, &c, 1);
        if (n == 1 && c == 'R') { s->notified = 1; s->next_probe = 0; }
        if (n == 1 || n == 0) { close(s->notify_fd); s->notify_fd = -1; }
    }
    int status;
    if (s->pid > 0 && waitpid(s->pid, &status, WNOHANG) == s->pid) exited(s, status);
    if (s->state == SV_STOPPING) {
        if (s->group > 0 && now >= s->deadline) kill(-s->group, SIGKILL);
        if (!s->pid && (s->group <= 0 || (kill(-s->group, 0) && errno == ESRCH))) {
            drain(s);
            if (s->log_fd >= 0) { close(s->log_fd); s->log_fd = -1; }
            s->group = 0;
            if (s->notify_fd >= 0) { close(s->notify_fd); s->notify_fd = -1; }
            if (s->oneshot && s->notified == 2) {
                s->state = SV_COMPLETE; event(s, "5-SERVICE_COMPLETE", "Completed.");
            } else if (!s->wanted) { s->state = SV_STOPPED; event(s, "5-SERVICE_STOP", "Stopped."); }
            else if (s->failures >= s->max_failures) {
                s->state = SV_FAILED; s->wanted = 0;
                event(s, "3-SERVICE_FAILED", "Restart limit reached; manual start required.");
            } else {
                s->state = SV_BACKOFF;
                unsigned shift = s->failures > 4 ? 4 : s->failures;
                s->deadline = now + ((long long)s->backoff_ms << shift);
            }
        }
    } else if (s->state == SV_BACKOFF && now >= s->deadline && !s->hold) launch(s);
    else if (s->state == SV_STARTING || s->state == SV_RUNNING) {
        if (now >= s->next_probe) {
            s->next_probe = now + 1000;
            int health = 0;
            if (!s->oneshot && (!s->notify || s->notified)) health = s->probe ? s->probe() : 1;
            if (health < 0) s->next_probe = now + 50;
            if (health > 0) {
                s->health_misses = 0;
                if (s->stop_pending) {
                    stopping(s);
                } else if (s->state == SV_STARTING) {
                    s->state = SV_RUNNING; s->stable_since = now;
                    event(s, "5-SERVICE_READY", "Ready.");
                } else if (now - s->stable_since > s->healthy_ms) s->failures = 0;
            } else if (!health && s->state == SV_RUNNING && ++s->health_misses >= 3) {
                failed(s, "Service health check failed.");
            }
        }
        if (s->state == SV_STARTING && now >= s->deadline) failed(s, "Service readiness timed out.");
    }
}
