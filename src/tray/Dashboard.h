// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Control.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace winapfs {

struct VolumeCardData : VolumeStatus
{
	bool has_space = false;
	uint64_t total_bytes = 0;
	uint64_t free_bytes = 0;
};

struct DashboardState
{
	bool service_up = false;
	bool service_installed = false;
	bool allow_writes = false;
	bool update_checking = false;
	bool update_downloading = false;
	bool update_available = false;
	std::string update_version;
	std::string update_status;
	std::vector<VolumeCardData> vols;
};

struct DashboardCallbacks
{
	std::function<void(const VolumeStatus &)> on_open;
	std::function<void(const VolumeStatus &)> on_mount;
	std::function<void(const VolumeStatus &)> on_unmount;
	std::function<void(const VolumeStatus &)> on_unlock;
	std::function<void(const VolumeStatus &)> on_eject;
	std::function<void(const VolumeStatus &)> on_toggle_rw;
	std::function<void(const VolumeStatus &)> on_toggle_auto;
	std::function<void(bool current_allow)> on_toggle_allow_writes;
	std::function<void()> on_rescan;
	std::function<void()> on_start_service;
	std::function<void()> on_install_service;
	std::function<void()> on_about;
	std::function<void()> on_check_updates;
	std::function<void()> on_install_update;
	std::function<void()> on_open_release_page;
};

void DashboardInit(HINSTANCE inst, HWND owner_tray_wnd, DashboardCallbacks cbs);
void DashboardShow();
void DashboardUpdate(const DashboardState &state);
void DashboardDestroy();
HWND DashboardHwnd();

} // namespace winapfs
