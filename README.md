# HamClock Backup & Restore

A small FLTK companion app for backing up and restoring saved HamClock configurations with a USB drive.

The app works with saved HamClock configurations in:

```text
~/.hamclock/configurations/
```

It copies `*.eeprom` files between that folder and the root of a mounted FAT32/exFAT USB drive. Optional metadata sidecar files named `*.eeprom.info.txt` are supported when present, but are not required.

## Install on Raspberry Pi OS / Linux

Install build dependencies:

```sh
sudo apt update
sudo apt install g++ make libfltk1.3-dev
```

Build and install:

```sh
make
sudo make install
make install-desktop
gio set "$HOME/Desktop/HamClock Backup.desktop" metadata::trusted true
```

If prompted, right-click the desktop shortcut and choose **Trust this executable** or **Allow Launching**.

## Run

From the desktop, launch:

```text
HamClock Backup
```

Or from a terminal:

```sh
hamclock-backup
```

## USB drives

Insert one FAT32 or exFAT USB drive before launching the app.

If more than one removable FAT/exFAT drive is found, the app will ask which one to use.

System FAT mounts such as the Raspberry Pi boot partition are ignored:

```text
/boot
/boot/firmware
/efi
```

## WSL USB note

On WSL, mount the Windows USB drive first. For example, if Windows assigns the USB drive as `D:`:

```sh
sudo mkdir -p /mnt/d
sudo mount -t drvfs D: /mnt/d
HAMCLOCK_BACKUP_USB=/mnt/d hamclock-backup
```

Replace `/mnt/d` with the correct drive letter path.

## Behavior

- Local saved configs are listed from `~/.hamclock/configurations/*.eeprom`.
- USB configs are listed from the root of the selected USB drive.
- Backup and restore are blocked while HamClock appears to be running.
- Copy operations verify the EEPROM file using CRC32 after copy.
- Missing sidecar metadata files are accepted and shown as `(no metadata)`.
- Multi-select is supported for backup and restore.

## Sidecar metadata

When present, sidecar files use this name format:

```text
<config>.eeprom.info.txt
```

Example:

```ini
[hamclock_backup]
format_version=1
config_name=Home Station
created_utc=2026-05-11T14:32:07Z
hostname=hamclock-pi.local
platform=linux-arm64
os_description=Raspberry Pi OS
hamclock_version=V4_24b05
eeprom_size=8192
eeprom_crc32=A3F2C1E8
callsign=W1AW
user_note=
```

The app reads sidecars when present, but restore does not require them.

## Uninstall

```sh
sudo make uninstall
make uninstall-desktop
```
