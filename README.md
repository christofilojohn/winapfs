<p align="center">
  <img src="assets/logo.svg" width="128" height="128" alt="winapfs logo">
</p>

<h1 align="center">winapfs</h1>

<p align="center">
  <b>Mac (APFS) disks on Windows, as ordinary drive letters.</b><br>
  Plug in a Mac-formatted drive and open it in Explorer: read it, and (experimentally) write to it.
</p>

<p align="center">
  <a href="https://github.com/FOLKGG/winapfs/releases"><img alt="Release" src="https://img.shields.io/github/v/release/FOLKGG/winapfs?include_prereleases&label=release"></a>
  <a href="https://github.com/FOLKGG/winapfs/actions/workflows/build.yml"><img alt="Build Windows" src="https://github.com/FOLKGG/winapfs/actions/workflows/build.yml/badge.svg"></a>
  <a href="https://github.com/FOLKGG/winapfs/actions/workflows/fsck-apfs.yml"><img alt="macOS fsck_apfs" src="https://github.com/FOLKGG/winapfs/actions/workflows/fsck-apfs.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="Licence: GPL-2.0" src="https://img.shields.io/badge/licence-GPL--2.0-blue"></a>
  <img alt="Windows 10/11 x64" src="https://img.shields.io/badge/Windows-10%20%7C%2011%20x64-0078d4">
  <img alt="Status: beta" src="https://img.shields.io/badge/status-beta-orange">
</p>

---

APFS is the file system of every Mac since 2017, and of most external drives
formatted on a Mac. Windows can't read it. **winapfs** makes those volumes appear
in Explorer like any other drive: files, folders, Mac file names, compressed
files, symbolic links, extended attributes and timestamps all come through.

It is free software, built on the Linux APFS driver
([linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw)) running in user
mode on Windows through [WinFsp](https://winfsp.dev). No kernel driver of its own,
no driver signing, nothing proprietary.

> [!WARNING]
> **Experimental software. Use it at your own risk.** winapfs is beta, and
> writing to APFS disks is experimental: a bug could corrupt a volume and lose
> data. **Back up anything you care about before connecting a disk, and before
> enabling writes.** Disks mount read-only by default; leave them that way if you
> only need to copy files off. winapfs comes with no warranty (see
> [No warranty](#no-warranty)).

## Features

- **Automatic mounting & high-speed I/O.** A background service mounts every
  APFS volume when a disk is connected, remembers its pinned drive letter,
  flushes writable volumes on unmount, and unmounts cleanly on "Safely Remove
  Hardware", unplugging and shutdown. Uses 4 MiB aligned I/O batching and
  pooled bounce buffers for fast Thunderbolt / USB4 external SSD transfers.
- **Modern Dashboard & tray app.** Manage every volume from the Windows 11
  Fluent Dashboard (left-click the tray icon) or tray menu: open in Explorer,
  mount/unmount, **Safely Eject** (flush + unmount + eject confirmation), switch
  read-only / read-write, pin drive letters (`D:`–`Z:`), toggle hiding macOS
  dotfiles (`._*`, `.DS_Store`), view live **Read / Write MB/s** throughput and
  disk usage, run **Verify Disk** (`winapfs-fsck`), and check for / install
  updates directly from GitHub Releases.
- **Faithful to the Mac side.**
  - Unicode file names exactly as stored, including characters Windows forbids
    (`: ? * | < > "`), which are mapped and round-trip
  - Case-sensitive and case-insensitive volumes
  - Compressed files (zlib, LZVN, LZFSE) decompressed on the fly
  - Extended attributes as NTFS-style alternate data streams
  - Symbolic links as Windows symlinks; FIFOs and devices as WSL reparse points
  - Creation, modification and access times
  - Dotfiles and Finder-hidden files shown as hidden, as on macOS
- **Encrypted volumes** (FileVault / encrypted APFS): read with the password or
  recovery key (tray / Dashboard "Unlock…", optional saving in Windows
  Credential Manager for automatic unlock, or `--ask`).
- **Read-write** (experimental) on images and, opt-in, on physical disks, with
  conservative safety checks (see [Writing](#writing-experimental)).
- **Several volumes per container**, GPT disks with 512- and 4096-byte sectors,
  and raw disk images (`.img`).

### Status

| | |
|---|---|
| Reading (files, metadata, xattrs, links, compression) | ✅ Stable, checked against manifests made on macOS |
| Encrypted volumes | ✅ Read-only |
| Writing to unencrypted volumes | 🧪 Experimental. Passes crash/consistency tests, and `winapfs`-written test images (`basic`, `compressed`, `links`, `casesensitive`) are validated in CI with Apple's `fsck_apfs -n -l` and macOS read-only mounts; physical-disk writes remain opt-in and experimental |
| Writing to encrypted volumes, snapshots | ⏳ Planned |
| Fusion drives, Time Machine and macOS system volumes (writing) | ⛔ Refused (read-only) |

## Install

Download builds from [Releases](https://github.com/FOLKGG/winapfs/releases) (or the latest build artifacts from [GitHub Actions](https://github.com/FOLKGG/winapfs/actions/workflows/build.yml)):

* **All-in-one installer (recommended):** download **`winapfs-<version>-x64-setup.exe`** (`winapfs-setup-x64` artifact) and run it. It installs **WinFsp 2.2** and **winapfs** in one step (uninstalling winapfs later leaves WinFsp intact).
* **Standalone MSI** (if you already have **[WinFsp](https://winfsp.dev/rel/)** 2.x installed): download and run **`winapfs-<version>-x64.msi`** (`winapfs-msi-x64` artifact).

The installers are not code-signed yet, so SmartScreen may ask you to confirm ("More info" → "Run anyway").

The installer adds the `winapfs` service (starts with Windows), the tray app,
and the command-line tools (on `PATH`). Connect an APFS disk and it appears as a
drive letter, **read-only** by default.

To uninstall, use *Settings → Apps*. Your disks are never touched.

## Use

### Explorer, Dashboard and tray

APFS volumes show up under *This PC*. Left-click the **winapfs** tray icon (or
launch **winapfs** from the Start Menu) to open the Dashboard, or right-click
for the tray menu:

- **Open** a volume in Explorer, or monitor live **Read / Write MB/s** and capacity
- **Mount / Unmount**, or **Safely Eject** (flushes caches, unmounts every
  volume on the disk, ejects via Windows, and notifies when safe to unplug)
- **Unlock…** an encrypted volume (with optional Windows Credential Manager
  storage for automatic unlock on future connections)
- **Read-write / Read-Only**: switch access mode per volume (see below)
- **Letter (`D:`–`Z:`)**: pin a preferred drive letter remembered per volume UUID
- **Dotfiles / Auto-mount**: toggle hiding macOS dotfiles (`._*`, `.DS_Store`)
  and automatic mounting per volume
- **Verify Disk**: run `winapfs-fsck -c` against the APFS container from the UI
- **Updates**: check GitHub Releases for newer versions and run the installer

### Command line

```powershell
winapfs list                                   # APFS disks, partitions and volumes
winapfs mount disk.img *                       # an image at a free drive letter (no admin needed)
winapfs mount \\.\PhysicalDrive2 X: --volume "Macintosh HD - Data"   # admin
winapfs mount-all --ask                        # every volume; ask for passwords
winapfs ctl list                               # what the service has mounted
winapfs ctl set Backup DriveLetter M:          # pin a volume's drive letter
winapfs ctl set Backup HideDotfiles 0          # show macOS dotfiles in Explorer
winapfs ctl verify Backup                      # run winapfs-fsck -c through the service
winapfs ctl eject Backup                       # flush, unmount and eject a disk
winapfs-cli cat disk.img /path/in/volume       # read a file without mounting
winapfs-fsck \\.\PhysicalDrive2                # check a container (read-only)
```

`winapfs --help`, `winapfs service` and `winapfs ctl` describe every option.

## Writing (experimental)

Writing uses the same engine Linux uses, and every change goes through APFS's
copy-on-write transactions and checkpoints, so a crash or an unplugged cable
leaves the last checkpoint intact. Still, it is young code: **keep a backup.**

Writing is off by default. Turn it on per volume in the tray (*Read-write*), or
with `winapfs mount … --rw`. Physical disks also need *Allow writing to disks*
in the tray (`--allow-disk-writes` on the command line). winapfs refuses to
write when:

- the disk was **not ejected cleanly** from the Mac (crash, hibernation, cable
  pulled). Eject it properly on the Mac, or mount it read-only.
- the volume is **encrypted**, **sealed** (macOS system), a **Time Machine**
  backup, another **macOS-owned role**, or part of a **Fusion** drive.

Files the Mac stored compressed are rewritten uncompressed when you change them.

## How it works

```mermaid
flowchart LR
    A["Explorer and apps"] --> B["WinFsp<br/>(kernel driver)"]
    B --> C["winapfs<br/>file system front-end"]
    C --> D["linux-apfs-rw<br/>(Linux APFS driver)"]
    D --> E["kshim<br/>Linux kernel API in user mode"]
    E --> F[("APFS disk<br/>or image")]
    C -. "encrypted volumes" .-> G["ApfsLib<br/>(apfs-fuse)"]
    G -.-> F
```

- **Engine.** [linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw), the
  read-write APFS driver for Linux, is compiled unchanged in spirit (a few
  marked patches) for Windows on top of **kshim**, a small user-mode
  implementation of the Linux kernel APIs it needs: page cache, buffer heads,
  inode cache, work queues, block devices.
- **Front-end.** winapfs implements a [WinFsp](https://winfsp.dev) file system:
  it maps Windows calls (CreateFile, ReadFile, rename, streams, reparse points,
  security) onto the engine.
- **Encrypted volumes** are read with ApfsLib from
  [apfs-fuse](https://github.com/sgan81/apfs-fuse), which also serves as an
  independent second reader in the tests.
- **Checking.** `winapfs-fsck` is [apfsprogs](https://github.com/linux-apfs/apfsprogs)'
  `apfsck`, ported to Windows.

### Testing

The test suite (150 tests, `ctest`) runs against disk images made on macOS by a
generator script (`testdata/generator`), with a manifest of every file's
contents and metadata:

- every volume is walked through the engine, through ApfsLib, and through a
  mounted drive letter with plain Win32 calls, and compared field by field;
- write tests replay hundreds of operations (renames, hard links, xattrs,
  compressed files, odd names) through the engine and through Explorer-style
  Win32 calls, then verify the result with both readers and `fsck`;
- **crash tests** cut the power at evenly spread points during a write storm
  (including a simulated volatile disk cache) and require a clean `fsck` and
  the last synced state, intact;
- regression tests for long copies, disk-full, memory bounds and the
  installer's service;
- **cross-OS macOS `fsck_apfs` validation** (`.github/workflows/fsck-apfs.yml`):
  generates macOS APFS images (`basic`, `compressed`, `links`, `casesensitive`),
  runs `apfsrw-writetest` and `winapfs-fsck -c` on Windows, then sends the
  written images back to macOS to verify them with Apple's `fsck_apfs -n -l`
  and read-only `hdiutil` mounts.

## Build from source

Requirements: Visual Studio 2022 Build Tools (C++ workload), CMake ≥ 3.20,
WinFsp with its developer files (`ADDLOCAL=ALL`). The installers need WiX
Toolset 5 (`WixToolset.UI.wixext`, `WixToolset.Util.wixext`, and
`WixToolset.BootstrapperApplications.wixext` for the setup bundle).

```powershell
build.cmd Release                                     # configure and build (x64)
ctest --test-dir build -C Release --output-on-failure # needs the test images
powershell -File installer\build.ps1                  # dist\winapfs-<version>-x64.msi
powershell -File installer\build.ps1 -WinFspMsi path\to\winfsp.msi  # also builds dist\winapfs-<version>-x64-setup.exe
```

GitHub Actions (`.github/workflows/build.yml`) automates Release builds, unit
tests (`unit.names`), and both `.msi` and `-setup.exe` installers on
`windows-2022`.

The test images (2.7 GB) are not in the repository. Make them on a Mac with
`testdata/generator` (see its README); `testdata/SHA256SUMS` lists the
expected checksums.

## Credits

This repository is a maintained fork of
[christofilojohn/winapfs](https://github.com/christofilojohn/winapfs). winapfs
stands on the work of these projects. Thank you.

| Project | Used for | Licence |
|---|---|---|
| [linux-apfs-rw](https://github.com/linux-apfs/linux-apfs-rw) by Ernesto A. Fernández and contributors | The APFS engine (read and write) | GPL-2.0-only |
| [apfsprogs](https://github.com/linux-apfs/apfsprogs) by Ernesto A. Fernández and contributors | `apfsck`, as `winapfs-fsck` | GPL-2.0 |
| [apfs-fuse](https://github.com/sgan81/apfs-fuse) by Simon Gander | ApfsLib: encrypted volumes, test oracle | GPL-2.0-or-later |
| [WinFsp](https://github.com/winfsp/winfsp) by Bill Zissimopoulos | User-mode file systems on Windows (installed separately) | GPL-3.0 with FLOSS exception |
| [LZFSE](https://github.com/lzfse/lzfse) by Apple Inc. | LZVN / LZFSE decompression | BSD-3-Clause |
| [zlib](https://zlib.net) by Jean-loup Gailly and Mark Adler | zlib-compressed files | zlib |
| [bzip2](https://sourceware.org/bzip2/) by Julian Seward | ApfsLib's DMG reader | bzip2 (BSD-style) |
| [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann | Test manifests (tests only) | MIT |
| [WiX Toolset](https://wixtoolset.org) | Building the installer (build tool) | MS-RL |

The exact versions, local patches and full licence texts are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Apple's
[APFS Reference](https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf)
documents the on-disk format.

## No warranty

winapfs is experimental software provided **"as is", without warranty of any
kind**, express or implied, including fitness for a particular purpose. Writing
to APFS volumes is experimental (cross-OS CI validates written test images
against Apple's `fsck_apfs`, while real-world disk testing is ongoing); reading
has been tested extensively, but no file system driver is free of bugs. **You
use winapfs at your own risk**: the authors and contributors are not liable for any loss or corruption of data, or any other damage, arising
from its use (GPL-2.0, sections 11 and 12). Keep backups, eject disks properly
before unplugging them, and do not write to a disk you cannot afford to lose.

## Licence

winapfs is free software under the **GNU General Public License, version 2**
([LICENSE](LICENSE)). The program as a whole is GPL-2.0-only, because
linux-apfs-rw is; individual winapfs source files may say "GPL-2.0-or-later".
Bundled components keep their own licences (see above). The logo
([assets/logo.svg](assets/logo.svg)) is dedicated to the public domain (CC0 1.0).

winapfs uses WinFsp under its FLOSS exception:

> **WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**
> https://github.com/winfsp/winfsp

*APFS, Mac, macOS and FileVault are trademarks of Apple Inc. Windows is a
trademark of Microsoft Corporation. winapfs is an independent project, not
affiliated with or endorsed by either.*

*winapfs was developed with the help of Claude Opus 5.5, an AI model by
Anthropic, used for writing code, tests and documentation.*

## Contributing

Bug reports are very welcome, especially from real disks. Please include
`winapfs list` output, your Windows version, and for write problems the output
of `winapfs-fsck` on the disk. Never send disk images that contain private data.
