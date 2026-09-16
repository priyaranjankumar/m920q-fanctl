#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>

#define IO_BASE             0x0A24
#define IO_PAGE             (IO_BASE + 0)
#define IO_INDEX            (IO_BASE + 1)
#define IO_DATA             (IO_BASE + 2)

#define REG_FAN_MODE        0x0A00
#define REG_FAN_CFG         0x0A01
#define REG_PWM2_CMD        0x0A29
#define REG_PWM2_FEEDBACK   0x0161
#define REG_FAN2_RPM        0x0142
#define REG_ENGINE_STATUS   0x0CF8

#define FAN2_MANUAL_BIT    0x02

#define CF8_LOCK           0x40
#define CF8_PHASE          0x08
#define CF8_INVALID        0x10
#define CF8_CHECK_DONE     0x20

#define CFG_REQ            0x80
#define CFG_DONE_M920Q     0x00

#define CONFIG_TIMEOUT_MS  1000
#define CONFIG_POLL_MS     1

#define SETTLE_MS         1000
#define STATE_PATH        "/run/m920q-fanctl.state"
#define LOCK_PATH         "/run/lock/m920q-fanctl.lock"

#define MODULE_NCT6683    "nct6683"
#define MODULE_NCT6687    "nct6687"

static int g_io = -1;
static int g_lock = -1;
static bool g_config_open = false;

/* Driver handoff state for this process. */
static bool g_handoff_active = false;
static bool g_restore_nct6683 = false;
static bool g_manual_commit = false;

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

static bool hwmon_driver_bound(const char *driver_name)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir)
        return false;

    struct dirent *ent;
    bool found = false;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        char name_path[PATH_MAX];
        char driver_path[PATH_MAX];

        snprintf(name_path, sizeof(name_path),
                 "/sys/class/hwmon/%s/name", ent->d_name);
        snprintf(driver_path, sizeof(driver_path),
                 "/sys/class/hwmon/%s/device/driver",
                 ent->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f)
            continue;

        char name[128];
        bool is_nct = false;

        if (fgets(name, sizeof(name), f)) {
            name[strcspn(name, "\r\n")] = '\0';
            is_nct = strcmp(name, "nct6686") == 0;
        }
        fclose(f);

        if (!is_nct)
            continue;

        char link[PATH_MAX];
        ssize_t n = readlink(driver_path, link, sizeof(link) - 1);
        if (n < 0)
            continue;

        link[n] = '\0';

        const char *base = strrchr(link, '/');
        base = base ? base + 1 : link;

        if (strcmp(base, driver_name) == 0) {
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

static bool nct_driver_active(const char *name)
{
    return module_loaded(name) || hwmon_driver_bound(name);
}

static int run_modprobe(bool remove, const char *module)
{
    pid_t pid = fork();

    if (pid < 0)
        die_errno("forking for modprobe");

    if (pid == 0) {
        if (remove)
            execlp("modprobe", "modprobe", "-r", module, (char *)NULL);
        else
            execlp("modprobe", "modprobe", module, (char *)NULL);

        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        die_errno("waiting for modprobe");
    }

    if (!WIFEXITED(status))
        return 128;

    return WEXITSTATUS(status);
}

static void write_state(bool restore_nct6683)
{
    int fd = open(STATE_PATH, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0)
        die_errno("creating runtime state");

    const char *text = restore_nct6683
        ? "nct6683_was_loaded=1\n"
        : "nct6683_was_loaded=0\n";

    size_t len = strlen(text);

    if (write(fd, text, len) != (ssize_t)len) {
        close(fd);
        die_errno("writing runtime state");
    }

    if (fsync(fd) < 0) {
        close(fd);
        die_errno("syncing runtime state");
    }

    close(fd);
}

static bool read_state(bool *restore_nct6683)
{
    FILE *f = fopen(STATE_PATH, "r");
    if (!f)
        return false;

    char line[128];
    bool found = false;

    while (fgets(line, sizeof(line), f)) {
        if (strcmp(line, "nct6683_was_loaded=1\n") == 0) {
            *restore_nct6683 = true;
            found = true;
            break;
        }

        if (strcmp(line, "nct6683_was_loaded=0\n") == 0) {
            *restore_nct6683 = false;
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

static void remove_state(void)
{
    if (unlink(STATE_PATH) < 0 && errno != ENOENT)
        die_errno("removing runtime state");
}

static void remove_state_best_effort(void)
{
    if (unlink(STATE_PATH) < 0 && errno != ENOENT)
        (void)fprintf(stderr,
                      "m920q-fanctl: WARNING: could not remove %s: %s\n",
                      STATE_PATH, strerror(errno));
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

static bool wait_for_open(void)
{
    const int loops = CONFIG_TIMEOUT_MS / CONFIG_POLL_MS;

    for (int i = 0; i < loops; i++) {
        uint8_t s = nct_read8(REG_ENGINE_STATUS);

        if (!(s & CF8_LOCK) && (s & CF8_PHASE) && !(s & CF8_INVALID))
            return true;

        sleep_ms(CONFIG_POLL_MS);
    }

    return false;
}

static void begin_config(void)
{
    nct_write8(REG_FAN_CFG, CFG_REQ);

    if (!wait_for_open()) {
        uint8_t s = nct_read8(REG_ENGINE_STATUS);
        fprintf(stderr,
                "m920q-fanctl: ERROR: fan configuration window did not open.\n");
        print_engine(s);
        exit(EXIT_FAILURE);
    }

    g_config_open = true;
}

static void finish_config(void)
{
    /*
     * Verified M920q/NCT6686D completion command.
     * CF8 may legitimately remain 0x08 after completion on this machine.
     */
    nct_write8(REG_FAN_CFG, CFG_DONE_M920Q);
    sleep_ms(50);

    uint8_t s = nct_read8(REG_ENGINE_STATUS);

    if (s & CF8_INVALID) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: controller reported an invalid fan configuration.\n");
        print_engine(s);
        g_config_open = false;
        exit(EXIT_FAILURE);
    }

    g_config_open = false;
}

static void open_io(void)
{
    g_io = open("/dev/port", O_RDWR | O_CLOEXEC);
    if (g_io < 0)
        die_errno("opening /dev/port");
}

static void acquire_lock(void)
{
    (void)mkdir("/run/lock", 0755);

    g_lock = open(LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (g_lock < 0)
        die_errno("opening lock file");

    if (flock(g_lock, LOCK_EX) != 0)
        die_errno("locking fan controller");
}

static void close_resources(void)
{
    if (g_config_open && g_io >= 0) {
        /* Best-effort finish if the process exits while a transaction is open. */
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

static bool find_hwmon(char *out, size_t out_size)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir)
        return false;

    struct dirent *ent;
    bool found = false;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char name[128];
        if (fgets(name, sizeof(name), f)) {
            name[strcspn(name, "\r\n")] = '\0';

            if (strcmp(name, "nct6686") == 0) {
                snprintf(out, out_size, "/sys/class/hwmon/%s", ent->d_name);
                found = true;
                fclose(f);
                break;
            }
        }

        fclose(f);
    }

    closedir(dir);
    return found;
}

static bool read_sysfs_u32(const char *path, unsigned *value)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    unsigned long v;
    int ok = fscanf(f, "%lu", &v) == 1 && v <= 0xFFFFFFFFUL;
    fclose(f);

    if (!ok)
        return false;

    *value = (unsigned)v;
    return true;
}

static void status_from_hwmon(bool managed_manual)
{
    char hwmon[PATH_MAX];
    if (!find_hwmon(hwmon, sizeof(hwmon))) {
        die("nct6683 is loaded but its nct6686 hwmon device could not be found");
    }

    char fan_path[PATH_MAX + 16];
    char pwm_path[PATH_MAX + 16];

    snprintf(fan_path, sizeof(fan_path), "%s/fan2_input", hwmon);
    snprintf(pwm_path, sizeof(pwm_path), "%s/pwm2", hwmon);

    unsigned rpm = 0;
    unsigned pwm = 0;

    if (!read_sysfs_u32(fan_path, &rpm))
        die("could not read nct6683 fan2_input");

    if (!read_sysfs_u32(pwm_path, &pwm))
        die("could not read nct6683 pwm2");

    printf("ThinkCentre M920q fan controller\n");
    printf("--------------------------------\n");

    if (managed_manual) {
        printf("Mode:       manual (WARNING: nct6683 is loaded during a managed manual session)\n");
    } else {
        printf("Mode:       unavailable via safe sysfs path (nct6683 owns NCT access)\n");
    }

    printf("PWM feedback:      %u/255 (%.1f%%)\n",
           pwm, (100.0 * pwm) / 255.0);
    printf("Fan2 RPM:           %u\n", rpm);
    printf("Kernel driver:      nct6683 (loaded)\n");
}

static void status_direct(bool managed_manual)
{
    uint8_t mode = nct_read8(REG_FAN_MODE);
    uint8_t cmd = nct_read8(REG_PWM2_CMD);
    uint8_t fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);
    uint8_t cf8 = nct_read8(REG_ENGINE_STATUS);

    bool manual = !!(mode & FAN2_MANUAL_BIT);

    printf("ThinkCentre M920q fan controller\n");
    printf("--------------------------------\n");
    printf("Mode:       %s%s\n",
           manual ? "manual" : "automatic",
           managed_manual && !manual ? " (managed session)" : "");
    printf("A00:        0x%02X\n", mode);
    printf("PWM command (A29): 0x%02X (%u/255)\n", cmd, cmd);
    printf("PWM feedback:      0x%02X (%u/255 = %.1f%%)\n",
           fb, fb, (100.0 * fb) / 255.0);
    printf("Fan2 RPM:           %u\n", rpm);
    print_engine(cf8);
    printf("Kernel driver:      nct6683=%s nct6687=%s\n",
           nct_driver_active(MODULE_NCT6683) ? "active" : "inactive",
           nct_driver_active(MODULE_NCT6687) ? "active" : "inactive");
}

static void prepare_manual_driver(void)
{
    if (nct_driver_active(MODULE_NCT6687))
        die("nct6687 kernel driver is active; unload it before direct control");

    bool state_restore = false;
    bool have_state = read_state(&state_restore);
    bool nct_active = nct_driver_active(MODULE_NCT6683);

    if (nct_active) {
        if (have_state && !state_restore)
            die("runtime state says nct6683 was not active, but it is currently active");

        if (!have_state)
            write_state(true);

        int rc = run_modprobe(true, MODULE_NCT6683);
        if (rc != 0 || nct_driver_active(MODULE_NCT6683)) {
            if (!have_state)
                remove_state_best_effort();

            fprintf(stderr,
                    "m920q-fanctl: ERROR: could not unload nct6683.\n");
            exit(EXIT_FAILURE);
        }

        g_handoff_active = true;
        g_restore_nct6683 = true;
        return;
    }

    if (have_state) {
        g_handoff_active = true;
        g_restore_nct6683 = state_restore;
    }
}

static void fail_safe_restore(void)
{
    if (!g_handoff_active || g_manual_commit)
        return;

    /*
     * If the process failed after unloading nct6683, return fan2 to
     * firmware control if possible, then restore the driver's prior state.
     */
    if (g_io >= 0) {
        uint8_t mode = nct_read8(REG_FAN_MODE);

        if (mode & FAN2_MANUAL_BIT) {
            begin_config();
            nct_write8(REG_FAN_MODE,
                       (uint8_t)(mode & (uint8_t)~FAN2_MANUAL_BIT));
            finish_config();
        }
    }

    if (g_restore_nct6683 && !nct_driver_active(MODULE_NCT6683))
        (void)run_modprobe(false, MODULE_NCT6683);

    remove_state_best_effort();
    g_handoff_active = false;
}

static void atexit_cleanup_all(void)
{
    fail_safe_restore();
    close_resources();
}

static void set_raw_value(int raw)
{
    if (raw < 0 || raw > 255)
        die("raw PWM must be between 0 and 255");

    uint8_t old_mode = nct_read8(REG_FAN_MODE);
    uint8_t old_cmd = nct_read8(REG_PWM2_CMD);

    begin_config();

    nct_write8(REG_FAN_MODE, (uint8_t)(old_mode | FAN2_MANUAL_BIT));
    nct_write8(REG_PWM2_CMD, (uint8_t)raw);

    finish_config();

    sleep_ms(SETTLE_MS);

    uint8_t final_mode = nct_read8(REG_FAN_MODE);
    uint8_t final_cmd = nct_read8(REG_PWM2_CMD);
    uint8_t final_fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);

    if ((final_mode & FAN2_MANUAL_BIT) == 0 || final_cmd != (uint8_t)raw) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: controller did not retain requested manual setting.\n"
                "  before: A00=0x%02X A29=0x%02X\n"
                "  after:  A00=0x%02X A29=0x%02X\n",
                old_mode, old_cmd, final_mode, final_cmd);
        exit(EXIT_FAILURE);
    }

    g_manual_commit = true;

    printf("Set M920q fan2 to raw PWM %d/255 (%.1f%%).\n",
           raw, (100.0 * raw) / 255.0);
    printf("PWM feedback: %u/255 (%.1f%%)\n",
           final_fb, (100.0 * final_fb) / 255.0);
    printf("Fan2 RPM:     %u\n", rpm);
}

static void set_percent(int percent, bool allow_zero)
{
    (void)allow_zero;

    int raw = (percent * 255 + 50) / 100;
    set_raw_value(raw);
}

static void set_auto(void)
{
    bool state_restore = false;
    bool have_state = read_state(&state_restore);

    if (nct_driver_active(MODULE_NCT6687))
        die("nct6687 kernel driver is active; unload it before direct control");

    if (nct_driver_active(MODULE_NCT6683)) {
        if (!have_state) {
            /*
             * No managed manual session exists. Do not access /dev/port
             * while the hwmon driver owns the controller.
             */
            printf("nct6683 is active and no managed manual session exists.\n");
            printf("Leaving Lenovo firmware/kernel fan control untouched.\n");
            return;
        }

        if (run_modprobe(true, MODULE_NCT6683) != 0 ||
            nct_driver_active(MODULE_NCT6683)) {
            die("could not unload nct6683 before returning to firmware mode");
        }
    }

    g_handoff_active = have_state;
    g_restore_nct6683 = have_state && state_restore;

    uint8_t old_mode = nct_read8(REG_FAN_MODE);

    begin_config();

    uint8_t new_mode =
        (uint8_t)(old_mode & (uint8_t)~FAN2_MANUAL_BIT);

    nct_write8(REG_FAN_MODE, new_mode);

    finish_config();
    sleep_ms(1000);

    uint8_t mode = nct_read8(REG_FAN_MODE);

    if (mode & FAN2_MANUAL_BIT) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: fan2 manual bit did not clear; A00=0x%02X\n",
                mode);
        exit(EXIT_FAILURE);
    }

    uint8_t fb = nct_read8(REG_PWM2_FEEDBACK);
    uint16_t rpm = nct_read16_be(REG_FAN2_RPM);

    printf("Returned M920q fan2 control to automatic firmware mode.\n");
    printf("A00:          0x%02X\n", mode);
    printf("PWM feedback: %u/255 (%.1f%%)\n",
           fb, (100.0 * fb) / 255.0);
    printf("Fan2 RPM:     %u\n", rpm);

    if (have_state && state_restore) {
        int rc = run_modprobe(false, MODULE_NCT6683);

        if (rc != 0 || !nct_driver_active(MODULE_NCT6683)) {
            fprintf(stderr,
                    "m920q-fanctl: ERROR: fan is back under firmware control, "
                    "but nct6683 could not be reloaded.\n"
                    "Run: modprobe nct6683\n");
                    return;
        }

        printf("nct6683 reloaded successfully; hwmon telemetry is available again.\n");
    }

    remove_state();
    g_handoff_active = false;
    g_manual_commit = true;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Usage:\n"
        "  m920q-fanctl status\n"
        "  m920q-fanctl set PERCENT [--allow-zero]\n"
        "  m920q-fanctl raw VALUE [--allow-zero]\n"
        "  m920q-fanctl auto\n"
        "  m920q-fanctl help\n\n"
        "Examples:\n"
        "  m920q-fanctl status\n"
        "  m920q-fanctl set 50\n"
        "  m920q-fanctl set 75\n"
        "  m920q-fanctl set 100\n"
        "  m920q-fanctl raw 128\n"
        "  m920q-fanctl auto\n\n"
        "Normal operation automatically coordinates the stock nct6683\n"
        "telemetry driver when entering/leaving manual control.\n"
        "No --force mode is provided.\n"
        "0%% is blocked unless --allow-zero is supplied.\n");
}

static int parse_int(const char *text, const char *what)
{
    char *end = NULL;

    errno = 0;
    long v = strtol(text, &end, 10);

    if (errno || end == text || *end != '\0' ||
        v < INT32_MIN || v > INT32_MAX) {
        fprintf(stderr,
                "m920q-fanctl: ERROR: invalid %s: '%s'\n",
                what, text);
        exit(EXIT_FAILURE);
    }

    return (int)v;
}

static void check_platform(void)
{
    if (geteuid() != 0)
        die("run as root");

    FILE *f = fopen("/sys/class/dmi/id/product_family", "r");
    if (f) {
        char buf[256];

        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\r\n")] = '\0';

            if (strstr(buf, "ThinkCentre M920q") == NULL) {
                fprintf(stderr,
                        "m920q-fanctl: ERROR: DMI product_family is '%s', "
                        "not ThinkCentre M920q.\n",
                        buf);
                fclose(f);
                exit(EXIT_FAILURE);
            }
        }

        fclose(f);
    }

    if (access("/dev/port", R_OK | W_OK) != 0)
        die_errno("accessing /dev/port");

    FILE *io = fopen("/proc/ioports", "r");
    if (io) {
        bool found = false;
        char line[512];

        while (fgets(line, sizeof(line), io)) {
            if (strstr(line, "0a20-0a2f")) {
                found = true;
                break;
            }
        }

        fclose(io);

        if (!found)
            die("NCT I/O range 0x0A20-0x0A2F is not reported in /proc/ioports");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    const char *command = argv[1];
    bool allow_zero = false;

    static const struct option opts[] = {
        {"allow-zero", no_argument, 0, 'z'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    optind = 2;

    int c;
    while ((c = getopt_long(argc, argv, "zh", opts, NULL)) != -1) {
        switch (c) {
        case 'z':
            allow_zero = true;
            break;
        case 'h':
            usage(stdout);
            return EXIT_SUCCESS;
        default:
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    if (strcmp(command, "help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    if (geteuid() != 0)
        die("run as root");

    /*
     * status uses the kernel hwmon sysfs interface when nct6683 is active.
     * No direct NCT I/O is performed in that case.
     */
    if (strcmp(command, "status") == 0) {
        bool managed_restore = false;
        bool managed_manual = read_state(&managed_restore);

        if (nct_driver_active(MODULE_NCT6683)) {
            status_from_hwmon(managed_manual);
            return EXIT_SUCCESS;
        }

        check_platform();
        acquire_lock();
        open_io();
        atexit(atexit_cleanup);

        status_direct(managed_manual);
        return EXIT_SUCCESS;
    }

    check_platform();

    /*
     * Serialize module handoff and direct controller access as one operation.
     */
    acquire_lock();
    open_io();
    atexit(atexit_cleanup_all);

    if (strcmp(command, "set") == 0) {
        if (optind >= argc)
            die("set requires a percentage argument");

        int percent = parse_int(argv[optind], "percentage");

        if (percent < 0 || percent > 100)
            die("percentage must be between 0 and 100");

        if (percent == 0 && !allow_zero)
            die("0% is blocked by default; use --allow-zero only if you explicitly want zero-RPM behavior");

        prepare_manual_driver();
        set_percent(percent, allow_zero);
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "raw") == 0) {
        if (optind >= argc)
            die("raw requires a PWM value");

        int raw = parse_int(argv[optind], "raw PWM value");

        if (raw < 0 || raw > 255)
            die("raw PWM must be between 0 and 255");

        if (raw == 0 && !allow_zero)
            die("raw 0 is blocked by default; use --allow-zero");

        prepare_manual_driver();
        set_raw_value(raw);
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "auto") == 0) {
        set_auto();
        return EXIT_SUCCESS;
    }

    fprintf(stderr,
            "m920q-fanctl: ERROR: unknown command '%s'\n\n",
            command);
    usage(stderr);
    return EXIT_FAILURE;
}
