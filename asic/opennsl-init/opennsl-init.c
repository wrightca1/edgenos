/*
 * opennsl-init.c - Initialize BCM56846 using OpenNSL (libopennsl.so.1)
 *
 * Uses dlopen() to load the Cumulus-verified Memory SDK at runtime.
 * This avoids glibc version mismatch at link time.
 *
 * Usage: opennsl-init [-c config.bcm] [-l libopennsl.so.1]
 *   Default config: /etc/switchd/config.bcm
 *   Default lib:    /usr/lib/libopennsl.so.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* OpenNSL init structure (from sal/driver.h) */
typedef struct {
    char         *cfg_fname;
    unsigned int flags;
    char         *wb_fname;
    char         *rmcfg_fname;
    char         *cfg_post_fname;
    unsigned int opennsl_flags;
} opennsl_init_t;

typedef int (*opennsl_driver_init_f)(opennsl_init_t *init);
typedef int (*opennsl_driver_exit_f)(void);

int main(int argc, char *argv[])
{
    char *config_file = "/etc/switchd/config.bcm";
    char *lib_path = "/usr/lib/libopennsl.so.1";
    void *handle;
    opennsl_driver_init_f driver_init;
    opennsl_init_t init;
    int rv;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            config_file = argv[++i];
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            lib_path = argv[++i];
    }

    printf("opennsl-init: BCM56846 initialization via OpenNSL\n");
    printf("  Library: %s\n", lib_path);
    printf("  Config:  %s\n", config_file);

    /* Load libopennsl */
    handle = dlopen(lib_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "ERROR: dlopen(%s): %s\n", lib_path, dlerror());
        return 1;
    }
    printf("  Library loaded OK\n");

    /* Find opennsl_driver_init */
    driver_init = (opennsl_driver_init_f)dlsym(handle, "opennsl_driver_init");
    if (!driver_init) {
        fprintf(stderr, "ERROR: dlsym(opennsl_driver_init): %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    /* Initialize */
    memset(&init, 0, sizeof(init));
    init.cfg_fname = config_file;
    init.flags = 0;

    printf("  Calling opennsl_driver_init()...\n");
    rv = driver_init(&init);
    printf("  opennsl_driver_init returned: %d\n", rv);

    if (rv != 0) {
        fprintf(stderr, "ERROR: init failed with code %d\n", rv);
        dlclose(handle);
        return 1;
    }

    printf("opennsl-init: ASIC ready!\n");
    printf("  Run sfp-enable.sh for retimer init.\n");

    /* Don't dlclose — keep ASIC state active */
    return 0;
}
