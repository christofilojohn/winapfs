// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace winapfs {

struct UpdateInfo
{
	bool ok = false;
	bool update_available = false;
	std::string latest_version;
	std::string tag_name;
	std::string release_url;
	std::string download_url;
	std::string asset_name;
	std::string error;
};

int CompareVersions(const std::string &a, const std::string &b);
UpdateInfo CheckForUpdates(const std::string &current_version);
bool DownloadUpdateInstaller(const std::string &url, const std::string &asset_name, std::wstring &out_path, std::string &err);

} // namespace winapfs
