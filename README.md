# m920q-fanctl

Userspace CLI for live fan control on a **Lenovo ThinkCentre M920q / NCT6686D-L**.

This project is deliberately hardware-specific. The NCT register mapping and fan-control transaction were verified on a physical ThinkCentre M920q.

## Verified hardware mapping

| Function | Register |
|---|---:|
| NCT page/index/data ports | `0x0A24 / 0x0A25 / 0x0A26` |
| Fan 2 tachometer | `0x0142` |
| PWM 2 feedback | `0x0161` |
| PWM 2 command | `0x0A29` |
| Fan control mode | `0x0A00`, bit `1` |
| Fan configuration command | `0x0A01` |
| Fan engine status | `0x0CF8` |

The physical fan mapping was verified experimentally:

- automatic mode: `A00 bit 1 = 0`
- manual mode: `A00 bit 1 = 1`
- PWM command: `A29 = 0..255`
- 50% command: `128/255`
- 75% command: `191/255`
- 100% command: `255/255`

A verified 50% test produced approximately 2940 RPM on the target machine, while automatic firmware control was approximately 1350 RPM at the time of testing.

## Important: this is not a generic controller

Do not use this program on another Lenovo ThinkCentre, ThinkStation, or another NCT6686D motherboard without independently verifying its register mapping.

The target is specifically the M920q platform tested with this project.

## How control works

The direct NCT transaction used by this project is:

```text
A01 = 0x80
    |
    v
wait for CF8:
  lock=0, phase=1, invalid=0
    |
    v
A00 |= 0x02          <- fan2 manual mode
    |
    v
A29 = requested PWM
    |
    v
A01 = 0x00          <- M920q/NCT6686D completion
    |
    v
verify A00/A29
    |
    v
observe PWM feedback/RPM
```

On the tested M920q, `CF8=0x08` can remain set after the transaction. Therefore the program does not incorrectly require `CF8` to return to `0x60` after every successful manual setting.

## Why the Linux nct6683 module is handled explicitly

The stock Linux `nct6683` driver is useful for normal hwmon telemetry, but direct `/dev/port` writes and the kernel driver can access the same NCT controller concurrently.

That is undesirable because the two paths do not share the same userspace lock.

This version of `m920q-fanctl` therefore manages the handoff:

### Normal state

```text
nct6683 loaded
    |
    +-- sensors / Proxmox hwmon telemetry
```

### `m920q-fanctl set 50`

```text
detect nct6683
    |
remember whether it was loaded
    |
unload nct6683
    |
direct NCT transaction
    |
manual fan control remains active
```

### `m920q-fanctl auto`

```text
return fan2 to firmware control
    |
if nct6683 was loaded before manual mode:
    |
reload nct6683
    |
verify hwmon telemetry is available
```

A runtime state file is stored at:

```text
/run/m920q-fanctl.state
```

This state disappears on reboot. That is intentional: a reboot lets Lenovo firmware initialize the fan normally.

## Commands

```bash
m920q-fanctl status

m920q-fanctl set 25
m920q-fanctl set 50
m920q-fanctl set 75
m920q-fanctl set 100

m920q-fanctl raw 128

m920q-fanctl auto
```

Zero is blocked by default:

```bash
m920q-fanctl set 0 --allow-zero
```

Do not use zero-RPM mode as a normal unattended setting without an independent thermal failsafe.

There is intentionally **no `--force` option** in this version. Concurrent raw `/dev/port` and hwmon access should not be the normal operating model.

## Safe status behavior

When `nct6683` is loaded:

```bash
m920q-fanctl status
```

reads fan telemetry through the kernel hwmon sysfs interface instead of accessing NCT registers directly.

When `nct6683` is unloaded, `status` reads the verified NCT registers directly.

## Fresh installation

On a fresh Proxmox/Debian system:

```bash
apt update
apt install -y build-essential
```

No DKMS driver, Python package, kernel headers, or custom kernel build is required.

Build:

```bash
make clean
make
```

The source should compile cleanly with:

```bash
gcc -O2 -Wall -Wextra -Wpedantic -Werror -std=c11 \
    -o m920q-fanctl m920q-fanctl.c
```

Install:

```bash
make install
```

Installed binary:

```text
/usr/local/sbin/m920q-fanctl
```

Installed documentation:

```text
/usr/local/share/doc/m920q-fanctl/README.md
```

## First-use procedure

Check the current driver:

```bash
lsmod | grep -E '^nct6683|^nct6687'
```

If `nct6687` is present, unload it before using this utility:

```bash
modprobe -r nct6687
```

Normally `nct6683` may be loaded. The CLI will automatically hand it off when entering manual mode.

Check:

```bash
m920q-fanctl status
```

Then:

```bash
m920q-fanctl set 50
```

Wait a couple of seconds and inspect:

```bash
m920q-fanctl status
```

Return to firmware:

```bash
m920q-fanctl auto
```

If `nct6683` was loaded before manual mode, the command reloads it automatically.

Verify:

```bash
sensors | grep -E 'fan2|pwm2'
```

## Failure and recovery behavior

If the CLI successfully unloads `nct6683` but then encounters an error before a manual setting is committed, its cleanup path attempts to:

1. return fan2 to firmware control;
2. reload `nct6683` if it was originally loaded;
3. remove the runtime state file.

If a process is forcibly killed (for example `SIGKILL`) before cleanup can execute, inspect the state:

```bash
cat /run/m920q-fanctl.state
```

Then run:

```bash
m920q-fanctl auto
```

If the driver is not restored automatically:

```bash
modprobe nct6683
```

A reboot is also a hardware-level reset of the fan-controller initialization path and the `/run` state file is intentionally lost.

## Why this version does not use `--force`

Earlier development builds allowed:

```bash
m920q-fanctl set 50 --force
```

while `nct6683` was loaded.

That was useful as an experiment and proved that the physical fan responds, but it permits two independent software paths to access the same NCT controller.

This production CLI instead performs the module handoff itself.

## Development notes

The implementation is intentionally direct and small:

```text
CLI
 |
 +-- DMI/platform safety checks
 |
 +-- driver ownership/handoff
 |
 +-- /run state
 |
 +-- /run/lock/m920q-fanctl.lock
 |
 +-- direct NCT6686D access
 |
 +-- verification
```

The lock prevents two `m920q-fanctl` processes from manipulating the controller simultaneously.

The state file is runtime-only so a reboot never leaves the system expecting an old manual-control session.

## Kernel telemetry

The stock `nct6683` driver remains the preferred telemetry source whenever the fan is under firmware control.

It reports the verified target channel as:

```text
fan2
pwm2
```

The kernel driver is not patched by this project.

## Unrelated experimental drivers

This project does not require the experimental `nct6687d` DKMS module.

Do not install the DKMS `nct6687d` module as a dependency of this project.

## Testing status

Verified on the target M920q:

- NCT6686D detection
- direct NCT configuration-window handshake
- fan2 manual-mode bit
- PWM command register
- 50% manual control
- 75% manual control
- 100% manual control
- return to automatic firmware control
- stock `nct6683` telemetry after returning to automatic control
- runtime kernel-driver conflict protection

The automatic module handoff introduced by this revision should be tested on the target Proxmox node before it is used unattended.
