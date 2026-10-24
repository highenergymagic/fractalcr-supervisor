/* SPDX-License-Identifier: MIT -- local, versioned supervisor protocol. */
#ifndef FRACTAL_CONTROL_H
#define FRACTAL_CONTROL_H
#include <stdint.h>
#include "supervisor.h"
#define FCR_CONTROL "/run/fractal-control.sock"
#define FCR_SERVICES 32
#define FCR_PROTOCOL 2
enum control_op { CT_LIST, CT_START, CT_STOP, CT_RESTART, CT_RELOAD };
struct control_request { uint32_t version, op; char name[32]; };
struct service_status {
    char name[32];
    int32_t state, pid, enabled, blocked;
    uint32_t starts, failures, dependencies;
};
struct control_reply {
    uint32_t version, count;
    int32_t result;
    struct service_status rows[FCR_SERVICES];
};
int control_call(enum control_op op, const char *name, struct control_reply *reply);
const struct service_status *control_find(const struct control_reply *reply, const char *name);
void control_display(const struct control_reply *reply, void *output);
int kernel_logger(void);
#endif
