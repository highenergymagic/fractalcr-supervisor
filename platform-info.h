/* SPDX-License-Identifier: MIT -- shared firmware inventory, no subprocesses. */
#ifndef FCR_PLATFORM_INFO_H
#define FCR_PLATFORM_INFO_H
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

static int platform_field_available(const char *value)
{
    return value[0] && strcasecmp(value, "Not Specified") &&
        strcasecmp(value, "To Be Filled By O.E.M.") &&
        strcasecmp(value, "Default string") && strcasecmp(value, "None");
}

static void platform_inventory(int detail)
{
    char vendor[256], product[256], serial[256], bios[256];
    char sku[256], board[256], board_serial[256], chassis_serial[256];
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
    platform_field("/sys/class/dmi/id/product_sku", sku, sizeof(sku), "");
    platform_field("/sys/class/dmi/id/board_name", board, sizeof(board), "");
    platform_field("/sys/class/dmi/id/board_serial", board_serial, sizeof(board_serial), "");
    platform_field("/sys/class/dmi/id/chassis_serial", chassis_serial, sizeof(chassis_serial), "");
    if (platform_field_available(sku)) printf("Model Number: %s\n", sku);
    if (platform_field_available(board)) printf("Motherboard Assembly Number: %s\n", board);
    if (platform_field_available(board_serial) && strcmp(board_serial, serial))
        printf("Motherboard Serial Number: %s\n", board_serial);
    if (platform_field_available(chassis_serial) && strcmp(chassis_serial, serial))
        printf("Chassis Serial Number: %s\n", chassis_serial);
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
    if (detail == 1)
        printf("System uptime is %ld days, %ld hours, %ld minutes\n",
            info.uptime/86400, info.uptime/3600%24, info.uptime/60%60);
}
#endif
