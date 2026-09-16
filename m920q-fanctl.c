#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

/*
 * Lenovo ThinkCentre M920q / NCT6686D-L specific userspace fan control.
 *
 * Hardware mapping verified on a ThinkCentre M920q (XXXXXXX):
 *   NCT I/O page/index/data base: 0x0A24..0x0A26
 *   fan2 tachometer:              0x0142 (16-bit RPM)
 *   pwm2 feedback:                0x0161 (0..255)
 *   pwm2 command:                 0x0A29 (0..255)
 *   fan control mode:             0x0A00, bit 1 for this physical fan
 *   fan config request:           0x0A01
 *   fan engine status:            0x0CF8
 *
 * M920q-specific transaction verified live:
 *   A01 = 0x80
 *   wait for CF8: lock=0, phase=1
 *   A00 |= 0x02 (manual control for tested fan channel)
 *   A29 = desired PWM
 *   A01 = 0x00
 *   wait for CF8: lock=1, check_done=1
 *
 * IMPORTANT: This tool intentionally does not load/unload kernel modules.
 * The nct6683/nct6687 hwmon drivers must not be accessing the controller while
 * this program performs raw I/O. See README.md.
 */

#define IO_BASE       0x0A24
#define IO_PAGE       (IO_BASE + 0)
#define IO_INDEX      (IO_BASE + 1)
#define IO_DATA       (IO_BASE + 2)

#define REG_FAN_MODE      0x0A00
#define REG_FAN_CFG       0x0A01
#define REG_PWM2_CMD      0x0A29
#define REG_PWM2_FEEDBACK 0x0161
#define REG_FAN2_RPM      0x0142
#define REG_ENGINE_STATUS 0x0CF8
#define REG_CHIP_ID       0x00FE

#define FAN2_MANUAL_BIT   0x02

#define CF8_LOCK          0x40
#define CF8_PHASE         0x08
#define CF8_INVALID       0x10
#define CF8_CHECK_DONE    0x20

#define CFG_REQ            0x80
#define CFG_DONE_M920Q     0x00

#define CONFIG_TIMEOUT_MS  1000
#define CONFIG_POLL_MS       1

#define LOCK_PATH "/run/lock/m920q-fanctl.lock"

static int g_io = -1;
static int g_lock = -1;
static bool g_config_open = false;

static void die(const char *msg)
{
    fprintf(stderr, "m920q-fanctl: ERROR: %s\n", msg);
    exit(EXIT_FAILURE);
}

static void die_errno(const char *msg)
{
    fprintf(stderr, "m920q-fanctl: ERROR: %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR)
            die_errno("nanosleep");
    }
}

static void port_write(int port, uint8_t value)
{
    if (pwrite(g_io, &value, 1, port) != 1)
        die_errno("writing /dev/port");
}

static uint8_t port_read(int port)
{
    uint8_t value = 0;
    if (pread(g_io, &value, 1, port) != 1)
        die_errno("reading /dev/port");
    return value;
}

static uint8_t nct_read8(uint16_t reg)
{
    port_write(IO_PAGE, 0xFF);
    port_write(IO_PAGE, (uint8_t)(reg >> 8));
    port_write(IO_INDEX, (uint8_t)(reg & 0xFF));
    return port_read(IO_DATA);
}

static uint16_t nct_read16_be(uint16_t reg)
{
    uint8_t hi = nct_read8(reg);
    uint8_t lo = nct_read8((uint16_t)(reg + 1));
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static void nct_write8(uint16_t reg, uint8_t value)
{
    port_write(IO_PAGE, 0xFF);
    port_write(IO_PAGE, (uint8_t)(reg >> 8));
    port_write(IO_INDEX, (uint8_t)(reg & 0xFF));
    port_write(IO_DATA, value);
}

static bool module_loaded(const char *name)
{
    FILE *f = fopen("/proc/modules", "r");
    if (!f)
        die_errno("opening /proc/modules");

    char line[4096];
    bool found = false;
    size_t nlen = strlen(name);

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, name, nlen) == 0 &&
            (line[nlen] == ' ' || line[nlen] == '\t')) {
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

static void check_environment(bool force)
{
    if (geteuid() != 0)
        die("run as root");

    if (access("/dev/port", R_OK | W_OK) != 0)
        die_errno("opening /dev/port");

    bool nct6683 = module_loaded("nct6683");
    bool nct6687 = module_loaded("nct6687");

    if ((nct6683 || nct6687) && !force) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: a kernel NCT hwmon driver is loaded.\n"
                "  nct6683=%s\n"
                "  nct6687=%s\n"
                "Raw /dev/port access must not race the kernel driver.\n\n"
                "Unload it first:\n"
                "  modprobe -r nct6683 2>/dev/null || true\n"
                "  modprobe -r nct6687 2>/dev/null || true\n\n"
                "Then retry.\n"
                "Use --force only if you fully understand the concurrency risk.\n",
                nct6683 ? "loaded" : "not loaded",
                nct6687 ? "loaded" : "not loaded");
        exit(EXIT_FAILURE);
    }

    FILE *f;
    char buf[256] = {0};

    f = fopen("/sys/class/dmi/id/product_family", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            if (strstr(buf, "ThinkCentre M920q") == NULL && !force) {
                fprintf(stderr,
                        "m920q-fanctl: ERROR: DMI product_family is '%s', not ThinkCentre M920q.\n"
                        "Use --force only for a board you have independently verified.\n",
                        buf);
                fclose(f);
                exit(EXIT_FAILURE);
            }
        }
        fclose(f);
    }

    /* The Lenovo ACPI device reserves the surrounding NCT I/O range.
     * Do not require the nct6683 module itself to be loaded: this utility
     * intentionally operates with that module unloaded.
     */
    f = fopen("/proc/ioports", "r");
    if (f) {
        bool found = false;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "0a20-0a2f")) {
                found = true;
                break;
            }
        }
        fclose(f);
        if (!found && !force) {
            die("NCT I/O range 0x0A20-0x0A2F is not reported in /proc/ioports");
        }
    }
}

static void acquire_lock(void)
{
    mkdir("/run/lock", 0755);

    g_lock = open(LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (g_lock < 0)
        die_errno("opening lock file");

    if (flock(g_lock, LOCK_EX) != 0)
        die_errno("locking fan controller");
}

static void open_io(void)
{
    g_io = open("/dev/port", O_RDWR);
    if (g_io < 0)
        die_errno("opening /dev/port");
}

static void close_resources(void)
{
    if (g_config_open) {
        /* Best-effort return to a locked/normal transaction state. */
        nct_write8(REG_FAN_CFG, CFG_DONE_M920Q);
        g_config_open = false;
    }

    if (g_io >= 0) {
        close(g_io);
        g_io = -1;
    }

    if (g_lock >= 0) {
        flock(g_lock, LOCK_UN);
        close(g_lock);
        g_lock = -1;
    }
}

static void atexit_cleanup(void)
{
    close_resources();
}

static void print_engine(uint8_t s)
{
    printf("CF8:       0x%02X (lock=%d phase=%d invalid=%d check_done=%d)\n",
           s,
           !!(s & CF8_LOCK),
           !!(s & CF8_PHASE),
           !!(s & CF8_INVALID),
           !!(s & CF8_CHECK_DONE));
}

static bool wait_for_engine(bool opened)
{
    const int loops = CONFIG_TIMEOUT_MS / CONFIG_POLL_MS;

    for (int i = 0; i < loops; i++) {
        uint8_t s = nct_read8(REG_ENGINE_STATUS);

        if (opened) {
            if (!(s & CF8_LOCK) && (s & CF8_PHASE) && !(s & CF8_INVALID))
                return true;
        } else {
            if ((s & CF8_LOCK) && (s & CF8_CHECK_DONE) && !(s & CF8_PHASE))
                return true;
        }

        sleep_ms(CONFIG_POLL_MS);
    }

    return false;
}

static void begin_config(void)
{
    nct_write8(REG_FAN_CFG, CFG_REQ);

    if (!wait_for_engine(true)) {
        uint8_t s = nct_read8(REG_ENGINE_STATUS);
        fprintf(stderr, "m920q-fanctl: ERROR: fan configuration window did not open.\n");
        print_engine(s);
        exit(EXIT_FAILURE);
    }

    g_config_open = true;
}

static void finish_config(void)
{
    nct_write8(REG_FAN_CFG, CFG_DONE_M920Q);

    if (!wait_for_engine(false)) {
        uint8_t s = nct_read8(REG_ENGINE_STATUS);
        fprintf(stderr, "m920q-fanctl: ERROR: fan configuration window did not close cleanly.\n");
        print_engine(s);
        g_config_open = false;
        exit(EXIT_FAILURE);
    }

    g_config_open = false;
}

static int percent_to_pwm(int percent)
{
    if (percent < 0 || percent > 100)
        die("percentage must be between 0 and 100");

    /* Round to nearest integer: 50% => 128. */
    return (percent * 255 + 50) / 100;
}

static int parse_int(const char *text, const char *what)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(text, &end, 10);

    if (errno || end == text || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
        fprintf(stderr, "m920q-fanctl: ERROR: invalid %s: '%s'\n", what, text);
        exit(EXIT_FAILURE);
    }

    return (int)v;
}

static void status(void)
{
    uint8_t mode = nct_read8(REG_FAN_MODE);
    uint8_t cmd = nct_read8(REG_PWM2_CMD);
    uint8_t fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);
    uint8_t cf8 = nct_read8(REG_ENGINE_STATUS);

    bool manual = !!(mode & FAN2_MANUAL_BIT);

    printf("ThinkCentre M920q fan controller\n");
    printf("--------------------------------\n");
    printf("Mode:       %s\n", manual ? "manual" : "automatic");
    printf("A00:        0x%02X\n", mode);
    printf("PWM command (A29): 0x%02X (%u/255)\n", cmd, cmd);
    printf("PWM feedback:      0x%02X (%u/255 = %.1f%%)\n", fb, fb, (100.0 * fb) / 255.0);
    printf("Fan2 RPM:           %u\n", rpm);
    print_engine(cf8);
}

static void set_raw(int raw)
{
    if (raw < 0 || raw > 255)
        die("raw PWM must be between 0 and 255");

    uint8_t old_mode = nct_read8(REG_FAN_MODE);
    uint8_t old_cmd = nct_read8(REG_PWM2_CMD);

    begin_config();

    uint8_t new_mode = (uint8_t)(old_mode | FAN2_MANUAL_BIT);

    nct_write8(REG_FAN_MODE, new_mode);
    nct_write8(REG_PWM2_CMD, (uint8_t)raw);

    finish_config();
    sleep_ms(200);

    uint8_t final_mode = nct_read8(REG_FAN_MODE);
    uint8_t final_cmd = nct_read8(REG_PWM2_CMD);
    uint8_t final_fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);

    if ((final_mode & FAN2_MANUAL_BIT) == 0 || final_cmd != (uint8_t)raw) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: controller did not retain the requested manual setting.\n"
                "  before: A00=0x%02X A29=0x%02X\n"
                "  after:  A00=0x%02X A29=0x%02X\n",
                old_mode, old_cmd, final_mode, final_cmd);
        exit(EXIT_FAILURE);
    }

    printf("Set M920q fan2 to raw PWM %d/255 (%.1f%%).\n",
           raw, (100.0 * raw) / 255.0);
    printf("PWM feedback: %u/255 (%.1f%%)\n", final_fb, (100.0 * final_fb) / 255.0);
    printf("Fan2 RPM:     %u\n", rpm);
}

static void set_percent(int percent, bool allow_zero)
{
    if (percent == 0 && !allow_zero)
        die("0% is blocked by default; use --allow-zero only if you explicitly want to test zero-RPM behavior");

    int raw = percent_to_pwm(percent);
    set_raw(raw);
}

static void set_auto(void)
{
    uint8_t old_mode = nct_read8(REG_FAN_MODE);

    begin_config();

    /* Clear only our verified M920q fan2 manual bit; preserve other bits. */
    uint8_t new_mode = (uint8_t)(old_mode & (uint8_t)~FAN2_MANUAL_BIT);
    nct_write8(REG_FAN_MODE, new_mode);

    finish_config();
    sleep_ms(200);

    uint8_t mode = nct_read8(REG_FAN_MODE);
    uint8_t fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);

    if (mode & FAN2_MANUAL_BIT) {
        fprintf(stderr, "m920q-fanctl: ERROR: fan2 manual bit did not clear; current A00=0x%02X\n", mode);
        exit(EXIT_FAILURE);
    }

    printf("Returned M920q fan2 control to automatic firmware mode.\n");
    printf("A00:          0x%02X\n", mode);
    printf("PWM feedback: %u/255 (%.1f%%)\n", fb, (100.0 * fb) / 255.0);
    printf("Fan2 RPM:     %u\n", rpm);
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage:\n"
        "  m920q-fanctl status\n"
        "  m920q-fanctl set PERCENT [--allow-zero] [--force]\n"
        "  m920q-fanctl raw VALUE [--allow-zero] [--force]\n"
        "  m920q-fanctl auto [--force]\n"
        "  m920q-fanctl help\n\n"
        "Examples:\n"
        "  m920q-fanctl status\n"
        "  m920q-fanctl set 50\n"
        "  m920q-fanctl set 100\n"
        "  m920q-fanctl raw 128\n"
        "  m920q-fanctl auto\n\n"
        "The utility controls the verified M920q fan2/pwm2 channel.\n"
        "Percent is mapped onto the NCT 0..255 PWM range.\n"
        "0%% is blocked unless --allow-zero is supplied.\n"
        "--force bypasses hardware/module safety checks and should normally not be used.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    const char *command = argv[1];
    bool force = false;
    bool allow_zero = false;

    static const struct option opts[] = {
        {"force", no_argument, 0, 'f'},
        {"allow-zero", no_argument, 0, 'z'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "fzh", opts, NULL)) != -1) {
        switch (c) {
        case 'f': force = true; break;
        case 'z': allow_zero = true; break;
        case 'h': usage(stdout); return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (strcmp(command, "help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    check_environment(force);
    acquire_lock();
    open_io();
    atexit(atexit_cleanup);

    if (strcmp(command, "status") == 0) {
        status();
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "set") == 0) {
        if (optind >= argc)
            die("set requires a percentage argument");
        int percent = parse_int(argv[optind], "percentage");
        set_percent(percent, allow_zero);
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "raw") == 0) {
        if (optind >= argc)
            die("raw requires a PWM value");
        int raw = parse_int(argv[optind], "raw PWM value");
        if (raw == 0 && !allow_zero)
            die("raw 0 is blocked by default; use --allow-zero");
        set_raw(raw);
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "auto") == 0) {
        set_auto();
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "m920q-fanctl: ERROR: unknown command '%s'\n\n", command);
    usage(stderr);
    return EXIT_FAILURE;
}
