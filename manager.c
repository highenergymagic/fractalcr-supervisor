/* SPDX-License-Identifier: MIT -- shared appliance PID 1. No command parser,
 * network policy, datastore probing, or image verification runs in PID 1. */
#define _GNU_SOURCE
#include "graph.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static struct service_graph graph;
static volatile sig_atomic_t shutdown_requested;
static int booting = 1;
static char logs[FCR_SERVICES][96];
static char *arguments[FCR_SERVICES][4];
static void shutdown_signal(int sig) { (void)sig; shutdown_requested = 1; }

static void event(struct service *s, const char *code, const char *message)
{
    char text[384];
    snprintf(text, sizeof(text), "%%FCR-%s: %s: %s\n", code, s->name, message);
    log_append("/run/log/events.log", text, strlen(text));
#ifndef FCR_ROMMON
    if (booting) fputs(text, stdout);
#endif
}

static unsigned add(const char *name, const char *program, const char *argument, unsigned deps, int enable)
{
    unsigned i = graph.count++;
    snprintf(logs[i], sizeof(logs[i]), "/run/log/%s.log", name);
    arguments[i][0] = (char *)program; arguments[i][1] = (char *)argument;
    graph.services[i] = (struct service){.name=name, .log_path=logs[i], .argv=arguments[i],
        .event=event, .notify=1};
    graph.dependencies[i] = deps; graph.enabled[i] = enable;
    service_init(&graph.services[i]);
    return i;
}

#ifndef FCR_ROMMON
static struct service *container_engine;
static const char *docker_socket = "/run/docker.sock";
static struct {
    int fd;
    pid_t pid;
    long long deadline;
    size_t sent, used;
    char response[256];
} docker_health = {.fd=-1};

static int docker_result(int result)
{
    if (docker_health.fd >= 0) close(docker_health.fd);
    docker_health.fd = -1; docker_health.sent = docker_health.used = 0;
    return result;
}

static int docker_ping(void)
{
    if (docker_health.pid != container_engine->pid) {
        docker_result(0); docker_health.pid = container_engine->pid;
    }
    const char request[] = "GET /_ping HTTP/1.0\r\nHost: docker\r\n\r\n";
    if (docker_health.fd < 0) {
        docker_health.fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (docker_health.fd < 0) return 0;
        struct sockaddr_un address = {.sun_family=AF_UNIX};
        snprintf(address.sun_path, sizeof(address.sun_path), "%s", docker_socket);
        if (connect(docker_health.fd, (struct sockaddr *)&address, sizeof(address))) return docker_result(0);
        docker_health.deadline = monotonic_ms() + 3000;
    }
    if (docker_health.sent < sizeof(request)-1) {
        ssize_t n = send(docker_health.fd, request + docker_health.sent,
                         sizeof(request)-1-docker_health.sent, MSG_NOSIGNAL);
        if (n > 0) docker_health.sent += (size_t)n;
        else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return docker_result(0);
    }
    if (docker_health.sent == sizeof(request)-1) {
        ssize_t n = recv(docker_health.fd, docker_health.response + docker_health.used,
                         sizeof(docker_health.response)-1-docker_health.used, 0);
        if (n > 0) {
            docker_health.used += (size_t)n;
            docker_health.response[docker_health.used] = 0;
            if (strstr(docker_health.response, "\r\n"))
                return docker_result(!strncmp(docker_health.response, "HTTP/1.0 200 ", 13) ||
                                     !strncmp(docker_health.response, "HTTP/1.1 200 ", 13));
            if (docker_health.used == sizeof(docker_health.response)-1) return docker_result(0);
        } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return docker_result(0);
    }
    return monotonic_ms() < docker_health.deadline ? -1 : docker_result(0);
}
#endif

static int bootstrap(void)
{
    /* The irreducible PID 1 bootstrap: IPC and device access for services.
     * Bind root before any child mounts (runc's no-pivot requirement). */
    if (mount("/", "/", NULL, MS_BIND, NULL)) return -1;
    const char *paths[] = {"/proc", "/sys", "/dev", "/run"};
    const char *types[] = {"proc", "sysfs", "devtmpfs", "tmpfs"};
    for (unsigned i = 0; i < 4; i++) {
        if (mkdir(paths[i], 0755) && errno != EEXIST) return -1;
        if (mount(types[i], paths[i], types[i], MS_NOSUID | (i == 2 ? 0 : MS_NODEV), NULL)) return -1;
    }
    if (mkdir("/run/log", 0700)) return -1;
    int fd = open("/proc/sys/kernel/printk", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) { if (write(fd, "0 4 0 0\n", 8) != 8) { close(fd); return -1; } close(fd); }
#ifdef FCR_ROMMON
    mkdir("/media", 0700);
    mkdir("/media/bootflash", 0700);
    for (int i = 0; i < 4; i++) { char path[32]; snprintf(path, sizeof(path), "/media/usb%d", i); mkdir(path, 0700); }
    if (mount("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) return -1;
    if (mount(NULL, "/", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL)) return -1;
#endif
    return 0;
}

static int listener(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un address = {.sun_family=AF_UNIX}; strcpy(address.sun_path, FCR_CONTROL);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) ||
        chmod(FCR_CONTROL, 0600) || listen(fd, 8)) return -1;
    return fd;
}

static void control(int listener_fd)
{
    /* A client may be descheduled between connect and send. Retain its socket
     * across ticks rather than racing it with a short blocking poll. Services
     * continue to run while the root-only client has one second to send. */
    static int fd = -1;
    static long long deadline;
    if (fd < 0) {
        fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        struct ucred peer; socklen_t length = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) || peer.uid) {
            close(fd); fd = -1; return;
        }
        deadline = monotonic_ms() + 1000;
    }
    struct control_request request;
    struct control_reply reply;
    int result = -1;
    ssize_t received = recv(fd, &request, sizeof(request), MSG_TRUNC);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
        monotonic_ms() < deadline) return;
    if (received == sizeof(request) &&
        request.version == FCR_PROTOCOL && memchr(request.name, 0, sizeof(request.name))) {
        if (request.op == CT_LIST) result = 0;
        else if (request.op == CT_RELOAD) { shutdown_requested = 1; result = 0; }
        else if (!shutdown_requested) {
            for (unsigned i = 0; i < graph.count; i++) if (!strcmp(request.name, graph.services[i].name)) {
                /* A remote console must remain recoverable after an accidental stop. */
                unsigned affected = 1U << i;
                int removes_console = graph.services[i].console;
                for (unsigned j = i+1; j < graph.count; j++) {
                    if (graph.dependencies[j] & affected) affected |= 1U << j;
                    if ((affected & (1U << j)) && graph.services[j].console) removes_console = 1;
                }
                if (removes_console && request.op == CT_STOP) result = -3;
                else {
                    result = graph_request(&graph, i, request.op);
                    if (!result && request.op == CT_RESTART) event(&graph.services[i], "5-SERVICE_RESTART", "Restart requested.");
                }
                break;
            }
        }
    }
    graph_status(&graph, &reply); reply.result = result;
    send(fd, &reply, sizeof(reply), MSG_NOSIGNAL); close(fd); fd = -1;
}

int main(void)
{
    if (getpid() != 1) return 1;
    setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, shutdown_signal); signal(SIGINT, shutdown_signal);
    setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
    if (bootstrap()) goto fatal;
    unsigned console;
#ifdef FCR_ROMMON
    const char *worker = "/usr/libexec/fractal-recovery";
    add("logging", worker, "logging", 0, 1);
    add("recovery-storage", worker, "storage", 0, 1);
    add("recovery-network", worker, "network", 0, 1);
    console = add("recovery-console", worker, "console", 0, 0);
#else
    const char *worker = "/usr/libexec/fractal-platform";
    puts("\nFractalCR Software, Version 0.5 (development)\nInitializing platform services...");
    add("platform", worker, "platform", 0, 1);
    add("logging", worker, "logging", 0, 1);
    add("datastore", worker, "datastore", 1U << 0, 1);
    add("configuration", worker, "configuration", 1U << 2, 1);
    add("networking", worker, "networking", 1U << 3, 1);
    unsigned syslog = add("syslog", worker, "syslog", 0, 1);
    unsigned engine = add("container-engine", worker, "container-engine", (1U << 2) | (1U << 4) | (1U << syslog), 1);
    container_engine = &graph.services[engine];
    graph.services[engine].notify = 0; graph.services[engine].probe = docker_ping;
    unsigned diagnostics = add("diagnostics", "/usr/libexec/fractal-selftest", NULL, 1U << engine, 0);
    graph.services[diagnostics].oneshot = 1; graph.services[diagnostics].notify = 0;
    graph.services[diagnostics].start_ms = 120000; graph.services[diagnostics].max_failures = 1;
    unsigned management = add("management", "/usr/libexec/fractal-management", "serve", 0, 1);
    graph.services[management].start_ms = 180000;
    add("ssh", "/usr/libexec/fractal-management", "ssh-service", (1U << 2) | (1U << 4) | (1U << management), 1);
    unsigned debug = add("debug-container", "/usr/libexec/fractal-management", "debug-service", 1U << engine, 0);
    graph.services[debug].start_ms = 180000;
    /* Two bounded Docker calls remove the owned container before providers stop. */
    graph.services[debug].stop_ms = 70000;
    unsigned buildkit = add("buildkit",worker,"buildkit",(1U << 2) | (1U << 4),0);
    graph.services[buildkit].start_ms=120000;
    console = add("console", worker, "console", 0, 0);
    setenv("DOCKER_RAMDISK", "1", 1);
#endif
    graph.services[console].console = 1;
    if (graph_validate(&graph)) goto fatal;
    int socket_fd = listener();
    if (socket_fd < 0) goto fatal;
    for (;;) {
        control(socket_fd);
        if (shutdown_requested) {
            /* Stop interactive recovery operations before unmounting media.
             * Console availability is independent of storage during operation. */
            if (graph.services[console].group > 0) graph.enabled[console] = 0;
            else graph_stop(&graph);
        }
        graph_tick(&graph);
        if (booting && !shutdown_requested) {
            int pending = 0;
            for (unsigned i = 0; i < console; i++) {
                struct service *s = &graph.services[i];
                if (graph.enabled[i] && graph_ready(&graph, i) && s->state != SV_RUNNING &&
                    s->state != SV_FAILED && s->state != SV_COMPLETE) pending = 1;
            }
            if (!pending) {
#ifndef FCR_ROMMON
                puts(graph.services[engine].state == SV_RUNNING ?
                    "%FCR-5-SYSTEM_READY: System initialization complete." :
                    "%FCR-3-SYSTEM_DEGRADED: System is available for diagnostics.");
#endif
                booting = 0; graph.enabled[console] = 1;
            }
        }
        if (shutdown_requested && graph_stopped(&graph)) {
            puts("Restarting system..."); sync(); reboot(RB_AUTOBOOT);
            goto fatal;
        }
        struct pollfd input = {.fd=socket_fd, .events=POLLIN}; poll(&input, 1, 50);
    }
fatal:
    puts("%FCR-2-SUPERVISOR: Platform bootstrap failed. Reloading in 10 seconds.");
    sleep(10); sync(); reboot(RB_AUTOBOOT);
    for (;;) pause();
}
