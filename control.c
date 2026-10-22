/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "control.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int control_call(enum control_op op, const char *name, struct control_reply *reply)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval timeout = {.tv_sec=2};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    strcpy(address.sun_path, FCR_CONTROL);
    struct control_request request = {.version=FCR_PROTOCOL, .op=op};
    if (name) snprintf(request.name, sizeof(request.name), "%s", name);
    memset(reply, 0, sizeof(*reply));
    int ok = !connect(fd, (struct sockaddr *)&address, sizeof(address)) &&
        send(fd, &request, sizeof(request), MSG_NOSIGNAL) == sizeof(request) &&
        recv(fd, reply, sizeof(*reply), MSG_TRUNC) == sizeof(*reply) &&
        reply->version == FCR_PROTOCOL && reply->count <= FCR_SERVICES;
    close(fd);
    return ok ? reply->result : -1;
}

const struct service_status *control_find(const struct control_reply *reply, const char *name)
{
    for (unsigned i = 0; i < reply->count; i++)
        if (!strcmp(reply->rows[i].name, name)) return &reply->rows[i];
    return NULL;
}

void control_display(const struct control_reply *reply, void *output)
{
    FILE *f = output;
    fputs("Service             State       PID     Starts  Failures  Depends on\n", f);
    for (unsigned i = 0; i < reply->count; i++) {
        const struct service_status *s = &reply->rows[i];
        fprintf(f, "%-19s %-11s %-7d %-7u %-9u", s->name,
                s->blocked ? "blocked" : service_state_name(s->state), s->pid, s->starts, s->failures);
        for (unsigned j = 0; j < reply->count; j++)
            if (s->dependencies & (1U << j)) fprintf(f, " %s", reply->rows[j].name);
        fputc('\n', f);
    }
}
