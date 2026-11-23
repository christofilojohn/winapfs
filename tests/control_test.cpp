// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for Control protocol helpers (SplitTabs) and Updater version /
// security checks (CompareVersions, DownloadUpdateInstaller URL validation).

#include "Control.h"
#include "Updater.h"

#include <cstdio>
#include <string>
#include <vector>

using winapfs::CompareVersions;
using winapfs::DownloadUpdateInstaller;
using winapfs::SplitTabs;

static int g_fail = 0;

static void Check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_fail++;
	}
}

static bool VecEq(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
	return a == b;
}

int main()
{
	// ---- SplitTabs
	Check(VecEq(SplitTabs(""), { "" }), "SplitTabs empty string");
	Check(VecEq(SplitTabs("ok"), { "ok" }), "SplitTabs single token");
	Check(VecEq(SplitTabs("a\tb\tc"), { "a", "b", "c" }), "SplitTabs three tokens");
	Check(VecEq(SplitTabs("a\t\tc\t"), { "a", "", "c", "" }), "SplitTabs empty middle & trailing field");

	auto vol = SplitTabs("vol\tUUID-1\tMacintosh HD\tmounted\tM:\trw\t\\\\.\\PhysicalDrive1\t\t0\t12582912\t6291456");
	Check(vol.size() == 11, "SplitTabs 11-field vol status");
	Check(vol.size() == 11 && vol[0] == "vol" && vol[2] == "Macintosh HD" && vol[4] == "M:" && vol[5] == "rw" &&
		vol[7] == "" && vol[8] == "0" && vol[9] == "12582912" && vol[10] == "6291456",
		"SplitTabs parses vol fields accurately");

	// ---- CompareVersions
	Check(CompareVersions("0.7.5", "0.7.5") == 0, "CompareVersions equal");
	Check(CompareVersions("v0.7.5", "0.7.5") == 0, "CompareVersions leading v");
	Check(CompareVersions("0.7.4", "0.7.5") < 0, "CompareVersions 0.7.4 < 0.7.5");
	Check(CompareVersions("0.7.10", "0.7.9") > 0, "CompareVersions 0.7.10 > 0.7.9");
	Check(CompareVersions("1.0.0", "0.99.99") > 0, "CompareVersions 1.0.0 > 0.99.99");
	Check(CompareVersions("0.7.5-beta", "0.7.5") == 0, "CompareVersions pre-release tuple");

	// ---- DownloadUpdateInstaller offline security checks (must reject before network I/O)
	std::wstring out_path;
	std::string err;
	Check(!DownloadUpdateInstaller("", "winapfs-0.7.5-x64-setup.exe", out_path, err),
		"DownloadUpdateInstaller rejects empty URL");
	Check(!DownloadUpdateInstaller("http://github.com/FOLKGG/winapfs/releases/download/v0.7.5/winapfs-0.7.5-x64-setup.exe",
		"winapfs-0.7.5-x64-setup.exe", out_path, err),
		"DownloadUpdateInstaller rejects non-HTTPS URL");
	Check(!DownloadUpdateInstaller("https://evil.example.com/winapfs-0.7.5-x64-setup.exe",
		"winapfs-0.7.5-x64-setup.exe", out_path, err),
		"DownloadUpdateInstaller rejects untrusted HTTPS host");

	std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "control_test: OK", g_fail);
	return g_fail ? 1 : 0;
}
