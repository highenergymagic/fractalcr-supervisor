/* SPDX-License-Identifier: MIT */
#ifndef FRACTAL_GRAPH_H
#define FRACTAL_GRAPH_H
#include "control.h"
struct service_graph {
    struct service services[FCR_SERVICES];
    unsigned dependencies[FCR_SERVICES];
    int enabled[FCR_SERVICES], cycling[FCR_SERVICES];
    unsigned count;
};
int graph_validate(const struct service_graph *g);
int graph_ready(const struct service_graph *g, unsigned i);
int graph_request(struct service_graph *g, unsigned i, enum control_op op);
void graph_tick(struct service_graph *g);
void graph_stop(struct service_graph *g);
int graph_stopped(const struct service_graph *g);
void graph_status(const struct service_graph *g, struct control_reply *reply);
#endif
