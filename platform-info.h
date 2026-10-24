/* SPDX-License-Identifier: MIT -- shared firmware inventory, no subprocesses. */
#ifndef FCR_PLATFORM_INFO_H
#define FCR_PLATFORM_INFO_H
#include <stdio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static void platform_field(const char *path, char *out, size_t size, const char *fallback)
{
    snprintf(out, size, "%s", fallback);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char value[256];
    if (fgets(value, sizeof(value), f)) {
        size_t n = strcspn(value, "\r\n");
        while (n && value[n-1] == ' ') n--;
        value[n] = 0;
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)value[i] < 32 || (unsigned char)value[i] > 126) value[i] = ' ';
        if (n) snprintf(out, size, "%s", value);
    }
    fclose(f);
}

static void platform_inventory(int detail)
{
    char vendor[256], product[256], serial[256], bios[256];
    struct sysinfo info;
    memset(&info, 0, sizeof(info));
    (void)sysinfo(&info);
    platform_field("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor), "Generic");
    platform_field("/sys/class/dmi/id/product_name", product, sizeof(product), "x86-64");
    printf("%s %s platform with %lluK bytes of memory.\n", vendor, product,
        (unsigned long long)info.totalram * info.mem_unit / 1024);
    if (!detail) return;
    platform_field("/sys/class/dmi/id/product_serial", serial, sizeof(serial), "Not specified");
    platform_field("/sys/class/dmi/id/bios_version", bios, sizeof(bios), "Not specified");
    printf("Processor board ID %s\n%ld logical processors\nSystem firmware: %s\n",
        serial, sysconf(_SC_NPROCESSORS_ONLN), bios);
    FILE *cpu = fopen("/proc/cpuinfo", "r");
    if (cpu) {
        char line[512];
        while (fgets(line,sizeof(line),cpu)) {
            if (!strncmp(line,"model name",10)) {
                char *model=strchr(line,':');
                if (model) { model++;while (*model==' ') model++;model[strcspn(model,"\r\n")]=0;printf("Processor: %s\n",model); }
                break;
            }
        }
        fclose(cpu);
    }
    printf("System uptime is %ld days, %ld hours, %ld minutes\n",
        info.uptime/86400, info.uptime/3600%24, info.uptime/60%60);
}
#endif
