# m920q-fanctl

Userspace fan control CLI for the **Lenovo ThinkCentre M920q** (NCT6686D-L).

Talks directly to the NCT6686D registers via `/dev/port`. No kernel module, DKMS, or lm-sensors required.

## Build & install

```bash
apt install -y build-essential   # if needed
make
make install                     # installs to /usr/local/sbin/
```

## Usage

```bash
m920q-fanctl status              # show current fan state
m920q-fanctl set 50              # set fan to 50% (128/255 PWM)
m920q-fanctl raw 128             # set fan to raw PWM value
m920q-fanctl auto                # return to firmware control
m920q-fanctl set 0 --allow-zero  # zero PWM (blocked by default)
```

## Safety checks

The program requires root and refuses to run if:

- The `nct6683` or `nct6687` kernel module is loaded (concurrent I/O risk)
- DMI product family is not ThinkCentre M920q
- NCT I/O range `0x0A20-0x0A2F` is not in `/proc/ioports`

Unload the kernel driver before use:

```bash
modprobe -r nct6683 2>/dev/null || true
modprobe -r nct6687 2>/dev/null || true
```

Use `--force` to bypass these checks (not recommended).

## Hardware details

Verified register mapping on one physical M920q:

| Function | Register |
|---|---|
| NCT page/index/data | `0x0A24 / 0x0A25 / 0x0A26` |
| Fan 2 tachometer | `0x0142` |
| PWM 2 feedback | `0x0161` |
| PWM 2 command | `0x0A29` |
| Fan control mode | `0x0A00` bit 1 |
| Config request | `0x0A01` |
| Engine status | `0x0CF8` |

This is **not** a generic NCT fan controller. Do not use on other hardware without verifying registers independently.
