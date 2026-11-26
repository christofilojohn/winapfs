// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Updater.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <winhttp.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <string>

namespace winapfs {
namespace {

constexpr const wchar_t *kApiHost = L"api.github.com";
constexpr const wchar_t *kLatestPath = L"/repos/FOLKGG/winapfs/releases/latest";
constexpr size_t kMaxApiJsonBytes = 2u * 1024u * 1024u;
constexpr uint64_t kMinInstallerBytes = 64u * 1024u;
constexpr uint64_t kMaxInstallerBytes = 100u * 1024u * 1024u;

struct WinHttpHandle
{
	HINTERNET h = nullptr;
	~WinHttpHandle()
	{
		if (h)
			WinHttpCloseHandle(h);
	}
	WinHttpHandle() = default;
	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle &operator=(const WinHttpHandle &) = delete;
};

std::wstring ToWide(const std::string &s)
{
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

bool StartsWith(const std::string &s, const char *prefix)
{
	size_t n = strlen(prefix);
	return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool EndsWithNoCase(const std::string &s, const char *suffix)
{
	size_t n = strlen(suffix);
	if (s.size() < n)
		return false;
	return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}

std::string StripLeadingV(std::string s)
{
	if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
		s.erase(s.begin());
	return s;
}

std::array<unsigned long, 3> ParseVersionTuple(const std::string &raw)
{
	std::array<unsigned long, 3> out = { 0, 0, 0 };
	std::string s = StripLeadingV(raw);
	size_t pos = 0;
	for (size_t i = 0; i < 3 && pos < s.size(); i++)
	{
		if (!std::isdigit(static_cast<unsigned char>(s[pos])))
			break;
		char *end = nullptr;
		out[i] = std::strtoul(s.c_str() + pos, &end, 10);
		if (!end || *end != '.')
			break;
		pos = static_cast<size_t>(end - s.c_str()) + 1;
	}
	return out;
}

bool IsTrustedDownloadUrl(const std::string &url)
{
	return StartsWith(url, "https://github.com/") || StartsWith(url, "https://objects.githubusercontent.com/");
}

std::wstring SanitizeAssetName(const std::string &asset_name)
{
	std::wstring clean = L"winapfs-update-";
	for (unsigned char ch : asset_name)
	{
		if (std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')
			clean.push_back(static_cast<wchar_t>(ch));
		else
			clean.push_back(L'_');
	}
	if (asset_name.empty())
		clean += L"setup.exe";
	return clean;
}

std::string Win32Err(const char *what)
{
	return std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
}

} // namespace

int CompareVersions(const std::string &a, const std::string &b)
{
	auto va = ParseVersionTuple(a);
	auto vb = ParseVersionTuple(b);
	for (size_t i = 0; i < 3; i++)
	{
		if (va[i] < vb[i])
			return -1;
		if (va[i] > vb[i])
			return 1;
	}
	return 0;
}

UpdateInfo CheckForUpdates(const std::string &current_version)
{
	UpdateInfo info;
	std::wstring ua = L"winapfs/" + ToWide(current_version);
	WinHttpHandle session{ WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
	if (!session.h)
	{
		info.error = Win32Err("WinHttpOpen failed");
		return info;
	}
	WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 15000);

	WinHttpHandle conn{ WinHttpConnect(session.h, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0) };
	if (!conn.h)
	{
		info.error = Win32Err("Cannot connect to GitHub");
		return info;
	}

	WinHttpHandle req{ WinHttpOpenRequest(conn.h, L"GET", kLatestPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
	if (!req.h)
	{
		info.error = Win32Err("Cannot open update request");
		return info;
	}

	const wchar_t *headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
	if (!WinHttpSendRequest(req.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr))
	{
		info.error = Win32Err("Cannot reach GitHub Releases");
		return info;
	}

	DWORD status = 0;
	DWORD status_sz = sizeof(status);
	if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX))
	{
		info.error = Win32Err("Cannot read HTTP status");
		return info;
	}

	if (status == 404)
	{
		info.ok = true;
		info.update_available = false;
		info.latest_version = current_version;
		info.release_url = "https://github.com/FOLKGG/winapfs/releases";
		return info;
	}
	if (status != 200)
	{
		info.error = "GitHub returned HTTP " + std::to_string(status);
		return info;
	}

	std::string body;
	char buf[8192];
	for (;;)
	{
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req.h, &avail))
		{
			info.error = Win32Err("Failed reading update response");
			return info;
		}
		if (avail == 0)
			break;
		DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
		DWORD got = 0;
		if (!WinHttpReadData(req.h, buf, to_read, &got))
		{
			info.error = Win32Err("Failed reading update response");
			return info;
		}
		if (got == 0)
			break;
		if (body.size() + got > kMaxApiJsonBytes)
		{
			info.error = "Update response is too large";
			return info;
		}
		body.append(buf, got);
	}

	nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
	if (doc.is_discarded() || !doc.is_object() || !doc.contains("tag_name") || !doc["tag_name"].is_string())
	{
		info.error = "Invalid release metadata from GitHub";
		return info;
	}

	info.tag_name = doc["tag_name"].get<std::string>();
	info.latest_version = StripLeadingV(info.tag_name);
	if (doc.contains("html_url") && doc["html_url"].is_string())
		info.release_url = doc["html_url"].get<std::string>();
	else
		info.release_url = "https://github.com/FOLKGG/winapfs/releases";

	std::string setup_url, setup_name;
	std::string msi_url, msi_name;
	if (doc.contains("assets") && doc["assets"].is_array())
	{
		for (const auto &a : doc["assets"])
		{
			if (!a.is_object() || !a.contains("name") || !a["name"].is_string() ||
			    !a.contains("browser_download_url") || !a["browser_download_url"].is_string())
				continue;
			std::string aname = a["name"].get<std::string>();
			std::string aurl = a["browser_download_url"].get<std::string>();
			if (!IsTrustedDownloadUrl(aurl))
				continue;
			if (EndsWithNoCase(aname, "-x64-setup.exe") && setup_url.empty())
			{
				setup_url = aurl;
				setup_name = aname;
			}
			else if (EndsWithNoCase(aname, "-x64.msi") && msi_url.empty())
			{
				msi_url = aurl;
				msi_name = aname;
			}
		}
	}

	if (!setup_url.empty())
	{
		info.download_url = setup_url;
		info.asset_name = setup_name;
	}
	else if (!msi_url.empty())
	{
		info.download_url = msi_url;
		info.asset_name = msi_name;
	}

	info.ok = true;
	info.update_available = CompareVersions(info.latest_version, current_version) > 0;
	return info;
}

bool DownloadUpdateInstaller(const std::string &url, const std::string &asset_name, std::wstring &out_path, std::string &err)
{
	out_path.clear();
	if (!StartsWith(url, "https://"))
	{
		err = "Refusing non-HTTPS update URL";
		return false;
	}
	if (!IsTrustedDownloadUrl(url))
	{
		err = "Refusing untrusted update host";
		return false;
	}

	std::wstring wurl = ToWide(url);
	wchar_t host[256] = {};
	wchar_t path[4096] = {};
	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = _countof(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = _countof(path);
	if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
	{
		err = "Invalid update URL";
		return false;
	}

	WinHttpHandle session{ WinHttpOpen(L"winapfs/" WINAPFS_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
	if (!session.h)
	{
		err = Win32Err("WinHttpOpen failed");
		return false;
	}
	WinHttpSetTimeouts(session.h, 5000, 10000, 15000, 60000);

	WinHttpHandle conn{ WinHttpConnect(session.h, host, uc.nPort, 0) };
	if (!conn.h)
	{
		err = Win32Err("Cannot connect to download server");
		return false;
	}

	WinHttpHandle req{ WinHttpOpenRequest(conn.h, L"GET", path[0] ? path : L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) };
	if (!req.h)
	{
		err = Win32Err("Cannot open download request");
		return false;
	}

	DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

	if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(req.h, nullptr))
	{
		err = Win32Err("Installer download failed");
		return false;
	}

	DWORD status = 0;
	DWORD status_sz = sizeof(status);
	if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz, WINHTTP_NO_HEADER_INDEX) ||
	    status != 200)
	{
		err = "Download failed with HTTP " + std::to_string(status);
		return false;
	}

	wchar_t tmp_dir[MAX_PATH] = {};
	DWORD tn = GetTempPathW(_countof(tmp_dir), tmp_dir);
	if (!tn || tn >= _countof(tmp_dir))
	{
		err = "Cannot resolve temp folder";
		return false;
	}
	std::wstring dest = std::wstring(tmp_dir) + SanitizeAssetName(asset_name);

	HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		err = Win32Err("Cannot create temporary installer file");
		return false;
	}

	uint64_t total_written = 0;
	bool ok = true;
	char buf[32768];
	for (;;)
	{
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req.h, &avail))
		{
			err = Win32Err("Download interrupted");
			ok = false;
			break;
		}
		if (avail == 0)
			break;
		DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
		DWORD got = 0;
		if (!WinHttpReadData(req.h, buf, to_read, &got))
		{
			err = Win32Err("Download read error");
			ok = false;
			break;
		}
		if (got == 0)
			break;
		if (total_written + got > kMaxInstallerBytes)
		{
			err = "Installer exceeds 100 MiB size limit";
			ok = false;
			break;
		}
		DWORD written = 0;
		if (!WriteFile(file, buf, got, &written, nullptr) || written != got)
		{
			err = Win32Err("Cannot write temporary installer file");
			ok = false;
			break;
		}
		total_written += written;
	}

	CloseHandle(file);
	if (!ok || total_written < kMinInstallerBytes)
	{
		if (ok)
			err = "Downloaded installer is unexpectedly small";
		DeleteFileW(dest.c_str());
		return false;
	}

	out_path = dest;
	return true;
}

} // namespace winapfs
