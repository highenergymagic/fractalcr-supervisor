/* SPDX-License-Identifier: MIT -- restartable kernel ring collector. */
#define _GNU_SOURCE
#include "control.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int kernel_logger(void)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 1;
    /* Persist the last sequence in tmpfs. Restart does not duplicate the ring. */
    unsigned long long last = 0;
    int have_last = 0;
    FILE *checkpoint = fopen("/run/kernel-sequence", "r");
    if (checkpoint) { have_last = fscanf(checkpoint, "%llu", &last) == 1; fclose(checkpoint); }
    service_notify();
    for (;;) {
        char buffer[8192];
        ssize_t n = read(fd, buffer, sizeof(buffer)-1);
        if (n > 0) {
            buffer[n] = 0;
            unsigned long long sequence;
            if (sscanf(buffer, "%*u,%llu,", &sequence) == 1 && (!have_last || sequence > last)) {
                log_append("/run/log/kernel.log", buffer, (size_t)n);
                last = sequence; have_last = 1;
                checkpoint = fopen("/run/kernel-sequence.new", "w");
                if (checkpoint) {
                    fprintf(checkpoint, "%llu\n", last); fclose(checkpoint);
                    rename("/run/kernel-sequence.new", "/run/kernel-sequence");
                }
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR && errno != EPIPE) return 1;
        else { struct pollfd p = {.fd=fd, .events=POLLIN}; poll(&p, 1, 100); }
    }
}
