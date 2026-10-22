/* SPDX-License-Identifier: MIT -- process-backed dependency lifecycle tests. */
#define _GNU_SOURCE
#include "graph.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
static void tick(struct service_graph *g) { graph_tick(g); usleep(1000); }
static int all_running(struct service_graph *g)
{
    for (unsigned i = 0; i < g->count; i++) if (g->services[i].state != SV_RUNNING) return 0;
    return 1;
}
static void ready(struct service_graph *g)
{
    long long end = monotonic_ms() + 5000;
    while (!all_running(g) && monotonic_ms() < end) tick(g);
    assert(all_running(g));
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        if (!strcmp(argv[1], "--notify")) { usleep(100000); service_notify(); for (;;) pause(); }
        if (!strcmp(argv[1], "--complete")) return 0;
        return 7;
    }
    assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
    char *args[] = {argv[0], "--notify", NULL};
    struct service_graph g = {.count=3};
    for (unsigned i = 0; i < g.count; i++) {
        const char *names[] = {"storage", "configuration", "engine"};
        g.services[i] = (struct service){.name=names[i], .argv=args, .log_path="/dev/null", .notify=1,
            .start_ms=2000, .stop_ms=100, .backoff_ms=5};
        g.enabled[i] = 1; g.dependencies[i] = i ? 1U << (i-1) : 0;
        service_init(&g.services[i]);
    }
    assert(!graph_validate(&g));
    g.dependencies[0] = 4; assert(graph_validate(&g)); g.dependencies[0] = 0;
    tick(&g); assert(g.services[0].state == SV_STARTING && !g.services[1].pid);
    ready(&g);
    pid_t old[3]; for (int i = 0; i < 3; i++) old[i] = g.services[i].pid;
    assert(!graph_request(&g, 0, CT_RESTART));
    long long end = monotonic_ms() + 5000;
    int stopped = 0;
    while (monotonic_ms() < end) {
        tick(&g);
        if (!g.services[0].pid || (g.services[0].starts == 2 && g.services[0].state == SV_STARTING)) {
            assert(!g.services[1].group && !g.services[2].group); stopped = 1;
        }
        if (all_running(&g) && g.services[0].pid != old[0]) break;
    }
    assert(stopped && all_running(&g));
    for (int i = 0; i < 3; i++) assert(g.services[i].pid != old[i] && g.services[i].starts == 2);
    puts("PASS readiness gating and dependency restart stop consumers before providers");
    assert(!graph_request(&g, 0, CT_STOP));
    end = monotonic_ms() + 4000;
    while (!graph_stopped(&g) && monotonic_ms() < end) tick(&g);
    assert(graph_stopped(&g));
    assert(graph_request(&g, 2, CT_START) == -2);
    for (int i = 0; i < 100; i++) tick(&g);
    assert(graph_stopped(&g));
    assert(!graph_request(&g, 0, CT_START)); ready(&g);
    assert(!graph_request(&g, 2, CT_STOP));
    end = monotonic_ms()+2000;
    while (g.services[2].group && monotonic_ms() < end) tick(&g);
    assert(!g.services[2].group);
    unsigned consumer_starts = g.services[2].starts;
    assert(!graph_request(&g, 1, CT_RESTART));
    end = monotonic_ms()+3000;
    do { tick(&g); } while (monotonic_ms() < end && (g.cycling[1] || g.services[1].state != SV_RUNNING));
    assert(g.services[1].state == SV_RUNNING);
    assert(!g.services[2].group && g.services[2].starts == consumer_starts);
    assert(!graph_request(&g, 2, CT_START)); ready(&g);
    puts("PASS manual provider stop blocks transitive dependents until explicit start");
    pid_t engine = g.services[2].pid;
    kill(g.services[0].pid, SIGKILL);
    end = monotonic_ms() + 5000;
    do { tick(&g); } while (monotonic_ms() < end && (g.services[2].pid == engine || !all_running(&g)));
    assert(all_running(&g) && g.services[2].pid != engine);
    puts("PASS unexpected provider death recovers the dependent chain");
    graph_stop(&g); end = monotonic_ms()+4000;
    while (!graph_stopped(&g) && monotonic_ms() < end) tick(&g);
    assert(graph_stopped(&g));
    char *once[] = {argv[0], "--complete", NULL};
    g.count = 1; g.services[0].argv = once; g.services[0].oneshot = 1;
    assert(!graph_request(&g, 0, CT_START));
    end = monotonic_ms()+2000;
    while (g.services[0].state != SV_COMPLETE && monotonic_ms() < end) tick(&g);
    assert(g.services[0].state == SV_COMPLETE);
    unsigned starts = g.services[0].starts;
    for (int i = 0; i < 100; i++) tick(&g);
    assert(g.services[0].starts == starts);
    puts("PASS successful one-shot jobs complete without an automatic restart");
    g = (struct service_graph){.count=2};
    for (unsigned i = 0; i < 2; i++) {
        g.services[i] = (struct service){.name=i ? "job" : "provider", .argv=i ? once : args,
            .log_path="/dev/null", .notify=!i, .oneshot=!!i, .start_ms=2000,
            .stop_ms=100, .backoff_ms=5};
        g.enabled[i] = 1; g.dependencies[i] = i ? 1 : 0;
        service_init(&g.services[i]);
    }
    end = monotonic_ms()+4000;
    while (g.services[1].state != SV_COMPLETE && monotonic_ms() < end) tick(&g);
    assert(g.services[1].state == SV_COMPLETE && g.services[1].starts == 1);
    assert(!graph_request(&g, 0, CT_RESTART));
    end = monotonic_ms()+4000;
    do { tick(&g); } while (monotonic_ms() < end && (g.cycling[0] || g.services[0].state != SV_RUNNING));
    assert(g.services[0].state == SV_RUNNING && g.services[0].starts == 2);
    assert(g.services[1].state == SV_COMPLETE && g.services[1].starts == 1);
    assert(!graph_request(&g, 0, CT_STOP));
    end = monotonic_ms()+4000;
    while (!graph_stopped(&g) && monotonic_ms() < end) tick(&g);
    assert(graph_stopped(&g) && g.services[1].state == SV_COMPLETE);
    assert(!graph_request(&g, 0, CT_START));
    end = monotonic_ms()+4000;
    while (g.services[0].state != SV_RUNNING && monotonic_ms() < end) tick(&g);
    assert(g.services[1].starts == 1);
    assert(!graph_request(&g, 1, CT_START));
    end = monotonic_ms()+4000;
    while (g.services[1].state != SV_COMPLETE && monotonic_ms() < end) tick(&g);
    assert(g.services[1].state == SV_COMPLETE && g.services[1].starts == 2);
    puts("PASS completed one-shot jobs survive provider restart and stop/start without rerunning");

    g.services[1].argv = args;
    assert(!graph_request(&g, 1, CT_START)); tick(&g);
    assert(g.services[1].group > 0);
    starts = g.services[1].starts;
    assert(!graph_request(&g, 0, CT_RESTART));
    end = monotonic_ms()+4000;
    do { tick(&g); } while (monotonic_ms() < end && (g.cycling[0] || g.services[0].state != SV_RUNNING));
    assert(g.services[0].state == SV_RUNNING && !g.services[1].group && !g.enabled[1]);
    for (int i = 0; i < 100; i++) tick(&g);
    assert(g.services[1].starts == starts);
    puts("PASS interrupted one-shot jobs require explicit retry after provider restart");
    graph_stop(&g); end = monotonic_ms()+4000;
    while (!graph_stopped(&g) && monotonic_ms() < end) tick(&g);
    assert(graph_stopped(&g));
    return 0;
}
