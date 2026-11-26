// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The service's control channel (used by `winapfs ctl` and winapfs-tray):
// a message-mode named pipe, one request per connection.
//
//   request   one line, fields separated by tabs (UTF-8)
//   response  lines; the last is "OK" or "ERR <message>"
//
//   list                      "vol\t<uuid>\t<name>\t<state>\t<letter>\t<mode>\t<disk>\t<note>[\t<hide_dotfiles>\t<read_bytes>\t<write_bytes>]"
//                             for every volume seen, then "cfg\tAllowDiskWrites\t<0|1>"
//                             state: mounted | locked (encrypted) | disabled |
//                                    failed | unmounted; mode: rw | ro | -
//   mount\t<uuid>[\t<password>]
//   unmount\t<uuid>           stays unmounted until its disk comes back (or mount)
//   eject\t<uuid>             unmounts the disk's volumes, then asks Windows to eject it
//   verify\t<uuid>            runs winapfs-fsck -c on the partition, returns "exit\t<code>" + output lines
//   set\t<uuid>\t<ReadWrite|Disabled|HideDotfiles>\t<0|1>   stored, applied at once
//   set\t<uuid>\tDriveLetter\t<X:>                         pinned letter, applied at once
//   rescan

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace winapfs {

// \\.\pipe\winapfs (the service); `service debug` serves kDebugPipe.
// WINAPFS_PIPE overrides the client's default.
extern const wchar_t *const kServicePipe;
extern const wchar_t *const kDebugPipe;
std::wstring DefaultControlPipe();

struct VolumeStatus
{
	std::string uuid, name, state, letter, mode, disk, note;
	bool hide_dotfiles = true;
	uint64_t read_bytes = 0;
	uint64_t write_bytes = 0;
};

// Sends one request. Returns 0 (OK), 1 (ERR: err has the message) or 2 (no
// service / transport error: err says so). lines: the response without the
// final status line.
int ControlRequest(const std::wstring &pipe, const std::string &request, std::vector<std::string> &lines, std::string &err);

// `list`, parsed. allow_disk_writes: the service's global switch.
bool ControlList(const std::wstring &pipe, std::vector<VolumeStatus> &vols, bool &allow_disk_writes, std::string &err);

std::vector<std::string> SplitTabs(const std::string &line);

} // namespace winapfs
