# Fresh-machine installation: Lenovo ThinkCentre M920q fan control

This document is written for a fresh Proxmox/Debian-based installation of a Lenovo ThinkCentre M920q using the NCT6686D-L controller.

## What this project does

`m920q-fanctl` is a root-only userspace CLI that talks directly to the Nuvoton NCT6686D EC-space registers through `/dev/port`.

It controls the single fan channel that was verified on the target M920q:

- fan2 tachometer: `0x0142`
- pwm2 feedback: `0x0161`
- pwm2 command: `0x0A29`
- manual-control bit: bit 1 of `0x0A00`
- fan configuration request: `0x0A01`
- fan engine status: `0x0CF8`
- NCT I/O page/index/data ports: `0x0A24/0x0A25/0x0A26`

This is **not** a generic Lenovo or NCT fan controller. The register mapping was verified on one ThinkCentre M920q/NCT6686D-L.

## Important safety rule

Do not let Linux `nct6683`/`nct6687` access the same controller while `m920q-fanctl` is performing a raw I/O transaction.

The program therefore refuses to run when either kernel module is loaded, unless `--force` is explicitly supplied.

For the safest operation:

```bash
modprobe -r nct6683 2>/dev/null || true
modprobe -r nct6687 2>/dev/null || true
```

Verify:

```bash
lsmod | grep -E '^nct6683|^nct6687'
```

Expected: no output.

## Dependencies

Fresh Debian/Proxmox system:

```bash
apt update
apt install -y build-essential
```

The utility does not require DKMS, kernel headers, Python, lm-sensors, or a custom kernel module.

## Build

From this project directory:

```bash
make
```

Verify:

```bash
./m920q-fanctl help
```

## Install

```bash
make install
```

Binary:

```text
/usr/local/sbin/m920q-fanctl
```

Documentation:

```text
/usr/local/share/doc/m920q-fanctl/README.md
```

## First test

Make sure the NCT kernel modules are unloaded:

```bash
modprobe -r nct6683 2>/dev/null || true
modprobe -r nct6687 2>/dev/null || true
```

Then:

```bash
m920q-fanctl status
```

Typical automatic-mode output on the verified machine is approximately:

```text
Mode:       automatic
A00:        0x00
PWM command (A29): 0x80 (128/255)
PWM feedback:      0x4B (75/255 = 29.4%)
Fan2 RPM:           ~1400
CF8:       0x60 (...)
```

Exact RPM/PWM values vary with temperature and firmware behavior.

## Set fan speed

Percentage is translated to the NCT's 0..255 scale:

```text
0%   -> 0
25%  -> 64
50%  -> 128
75%  -> 191
100% -> 255
```

Examples:

```bash
m920q-fanctl set 25
m920q-fanctl set 50
m920q-fanctl set 75
m920q-fanctl set 100
```

Or use a raw value:

```bash
m920q-fanctl raw 128
```

After a successful manual change, check:

```bash
m920q-fanctl status
```

The important state is:

```text
Mode: manual
A00 has bit 1 set
A29 equals the requested raw PWM value
```

The physical RPM is not expected to equal the percentage numerically; `50% PWM` is not necessarily `50% RPM`.

## Return control to Lenovo firmware

Use:

```bash
m920q-fanctl auto
```

This clears only the verified fan2 manual-control bit in `A00` and leaves other bits unchanged.

Then verify:

```bash
m920q-fanctl status
```

Expected:

```text
Mode: automatic
```

## 0% / zero-RPM behavior

The tool blocks zero by default because some fans support zero-RPM and some do not. To explicitly test it:

```bash
m920q-fanctl set 0 --allow-zero
```

or:

```bash
m920q-fanctl raw 0 --allow-zero
```

Do not use zero as a normal operating setting without a temperature-control/failsafe policy.

## How the hardware transaction works

For this M920q the verified manual sequence is:

```text
A01 = 0x80
   |
   v
wait until CF8 says:
  lock=0, phase=1, invalid=0
   |
   v
A00 = A00 | 0x02
   |
   v
A29 = requested PWM (0..255)
   |
   v
A01 = 0x00
   |
   v
wait until CF8 says:
  lock=1, check_done=1, phase=0
```

The program validates the configuration-window state before and after the transaction rather than assuming that the controller accepted the request.

## Why `0x00` is used to finish on this M920q

The generic NCT6686D/6687D implementations in current open-source projects use the NCT configuration protocol around `A01` and `A00/A28+`. LibreHardwareMonitor documents the NCT6683D/NCT6686D/NCT6687D register mappings and its generic manual-mode transaction, while the Fred78290 `nct6687d` project implements the related fan-engine handshake. The M920q test work documented with this project verified that ending the transaction with `A01=0x00` preserves the manual setting on this machine. A previous `A01=0x40` test on the same machine immediately restored `A00=0x00` and `A29=0x80`, so this project deliberately uses the M920q-proven `0x00` terminator.

Do not substitute a different completion byte based only on a different motherboard's implementation.

## Kernel-driver coexistence

The stock Linux `nct6683` driver is useful for sensor telemetry on this machine but its `pwm2` sysfs node is read-only and changing the mode externally does not provide the verified direct-control behavior.

For raw userspace control, keep `nct6683` and `nct6687` unloaded during control operations.

If you want this utility to be the permanent fan-control mechanism, you may choose to prevent automatic loading of `nct6683`. Do that only after confirming that you no longer need its hwmon interface.

## Optional: disable nct6683 autoload

First inspect where it is being loaded from:

```bash
grep -Rni -- 'nct6683' /etc/modules /etc/modules-load.d /etc/modprobe.d 2>/dev/null || true
```

For a machine dedicated to this userspace controller, a common approach is a modprobe blacklist file:

```bash
cat >/etc/modprobe.d/m920q-fanctl.conf <<'EOF'
blacklist nct6683
blacklist nct6687
