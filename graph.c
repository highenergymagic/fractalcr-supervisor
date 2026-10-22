/* SPDX-License-Identifier: MIT -- dependency order is verified, never inferred. */
#include "graph.h"
#include <stdio.h>
#include <string.h>

int graph_validate(const struct service_graph *g)
{
    if (!g->count || g->count > FCR_SERVICES) return -1;
    for (unsigned i = 0; i < g->count; i++) {
        /* Definitions are topologically ordered. Reject forward edges/cycles. */
        if (g->dependencies[i] & ~((1U << i)-1)) return -1;
        if (!g->services[i].name || strlen(g->services[i].name) >= 32) return -1;
        for (unsigned j = 0; j < i; j++)
            if (!strcmp(g->services[i].name, g->services[j].name)) return -1;
    }
    return 0;
}

int graph_ready(const struct service_graph *g, unsigned i)
{
    for (unsigned j = 0; j < i; j++) if (g->dependencies[i] & (1U << j)) {
        if (!g->enabled[j] || g->cycling[j] || !graph_ready(g, j) ||
            (g->services[j].state != SV_RUNNING && g->services[j].state != SV_COMPLETE)) return 0;
    }
    return 1;
}

static unsigned dependents(const struct service_graph *g, unsigned i)
{
    unsigned mask = 1U << i;
    for (unsigned j = i+1; j < g->count; j++)
        if (g->dependencies[j] & mask) mask |= 1U << j;
    return mask;
}

int graph_request(struct service_graph *g, unsigned i, enum control_op op)
{
    if (i >= g->count) return -1;
    if (op == CT_START || op == CT_RESTART) {
        if (!graph_ready(g, i)) return -2;
        g->enabled[i] = 1;
        g->services[i].failures = 0;
        if (g->services[i].state == SV_FAILED || g->services[i].state == SV_COMPLETE)
            g->services[i].state = SV_STOPPED;
    } else if (op == CT_STOP) g->enabled[i] = 0;
    else return -1;
    if (op == CT_RESTART) {
        unsigned mask = dependents(g, i);
        for (unsigned j = i; j < g->count; j++) if ((mask & (1U << j)) && g->enabled[j]) {
            struct service *s = &g->services[j];
            if (j != i && s->oneshot) {
                /* A provider restart does not request another execution of a
                 * completed job. An interrupted job requires explicit retry. */
                if (s->state != SV_COMPLETE && s->state != SV_FAILED) g->enabled[j] = 0;
            } else g->cycling[j] = 1;
        }
    }
    return 0;
}

void graph_tick(struct service_graph *g)
{
    services_reap(g->services, g->count);
    /* Stop consumers before their providers. A provider remains alive until
     * every dependent process group is gone, including forced cleanup. */
    for (unsigned n = g->count; n > 0; n--) {
        unsigned i = n-1;
        struct service *s = &g->services[i];
        unsigned mask = dependents(g, i) & ~(1U << i);
        int busy = 0;
        for (unsigned j = i+1; j < g->count; j++)
            if ((mask & (1U << j)) && g->services[j].group > 0) busy = 1;
        s->hold = busy;
        if (s->oneshot && s->group > 0 && !graph_ready(g, i)) g->enabled[i] = 0;
        if (!g->enabled[i] || g->cycling[i] || !graph_ready(g, i)) {
            int completed_job = g->enabled[i] && s->oneshot && s->state == SV_COMPLETE;
            if (!busy && !completed_job && s->state != SV_STOPPED && s->state != SV_FAILED && s->state != SV_STOPPING)
                service_stop(s);
        }
        service_tick(s);
    }
    for (unsigned i = 0; i < g->count; i++) {
        struct service *s = &g->services[i];
        if (g->cycling[i] && !s->group && s->state != SV_STOPPING) {
            g->cycling[i] = 0;
            s->state = SV_STOPPED;
        }
        if (g->enabled[i] && !g->cycling[i] && graph_ready(g, i) && s->state == SV_STOPPED)
            service_start(s);
    }
}

void graph_stop(struct service_graph *g) { for (unsigned i = 0; i < g->count; i++) g->enabled[i] = 0; }
int graph_stopped(const struct service_graph *g)
{
    for (unsigned i = 0; i < g->count; i++) if (g->services[i].group > 0) return 0;
    return 1;
}
void graph_status(const struct service_graph *g, struct control_reply *reply)
{
    memset(reply, 0, sizeof(*reply)); reply->version = FCR_PROTOCOL; reply->count = g->count;
    for (unsigned i = 0; i < g->count; i++) {
        const struct service *s = &g->services[i];
        struct service_status *r = &reply->rows[i];
        snprintf(r->name, sizeof(r->name), "%s", s->name);
        r->state = s->state; r->pid = s->pid; r->starts = s->starts; r->failures = s->failures;
        r->enabled = g->enabled[i]; r->blocked = g->enabled[i] && !graph_ready(g, i);
        r->dependencies = g->dependencies[i];
    }
}
