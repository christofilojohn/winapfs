// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Control.h"

#include <Windows.h>

#include <cstdlib>
#include <utility>

namespace winapfs {

static void SecureClear(std::string &s)
{
	if (!s.empty())
	{
		SecureZeroMemory(&s[0], s.size());
		s.clear();
	}
}

struct SecretStringGuard
{
	std::string &s;
	~SecretStringGuard() { SecureClear(s); }
};

const wchar_t *const kServicePipe = L"\\\\.\\pipe\\winapfs";
const wchar_t *const kDebugPipe = L"\\\\.\\pipe\\winapfs-debug";

std::wstring DefaultControlPipe()
{
	wchar_t buf[256];
	DWORD n = GetEnvironmentVariableW(L"WINAPFS_PIPE", buf, 256);
	return (n && n < 256) ? std::wstring(buf) : std::wstring(kServicePipe);
}

std::vector<std::string> SplitTabs(const std::string &line)
{
	std::vector<std::string> f;
	size_t pos = 0;
	for (;;)
	{
		size_t t = line.find('\t', pos);
		f.push_back(line.substr(pos, t == std::string::npos ? std::string::npos : t - pos));
		if (t == std::string::npos)
			break;
		pos = t + 1;
	}
	return f;
}

int ControlRequest(const std::wstring &pipe, const std::string &request, std::vector<std::string> &lines, std::string &err)
{
	lines.clear();
	HANDLE h = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 20 && h == INVALID_HANDLE_VALUE; attempt++)
	{
		h = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			break;
		DWORD e = GetLastError();
		if (e == ERROR_PIPE_BUSY)
			WaitNamedPipeW(pipe.c_str(), 1000);
		else if (e == ERROR_FILE_NOT_FOUND)
		{
			if (attempt >= 5)
			{
				err = "the winapfs service is not running";
				return 2;
			}
			Sleep(100);   // an older service between two pipe instances
		}
		else
		{
			err = "cannot connect to the winapfs service (error " + std::to_string(e) + ")";
			return 2;
		}
	}
	if (h == INVALID_HANDLE_VALUE)
	{
		err = "the winapfs service is busy";
		return 2;
	}
	DWORD mode = PIPE_READMODE_MESSAGE;
	SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
	DWORD done = 0;
	std::string req = request + "\n";
	SecretStringGuard req_guard{ req };
	if (!WriteFile(h, req.data(), static_cast<DWORD>(req.size()), &done, nullptr))
	{
		err = "cannot send the request (error " + std::to_string(GetLastError()) + ")";
		CloseHandle(h);
		return 2;
	}
	std::string resp;
	char buf[4096];
	for (;;)
	{
		BOOL ok = ReadFile(h, buf, sizeof(buf), &done, nullptr);
		resp.append(buf, done);
		if (ok)
			break;
		if (GetLastError() != ERROR_MORE_DATA)
		{
			err = "no reply from the winapfs service (error " + std::to_string(GetLastError()) + ")";
			CloseHandle(h);
			return 2;
		}
	}
	CloseHandle(h);

	size_t pos = 0;
	while (pos < resp.size())
	{
		size_t nl = resp.find('\n', pos);
		if (nl == std::string::npos)
			nl = resp.size();
		lines.push_back(resp.substr(pos, nl - pos));
		pos = nl + 1;
	}
	if (lines.empty())
	{
		err = "empty reply from the winapfs service";
		return 2;
	}
	std::string last = lines.back();
	lines.pop_back();
	if (last == "OK")
		return 0;
	err = last.compare(0, 4, "ERR ") == 0 ? last.substr(4) : last;
	return 1;
}

bool ControlList(const std::wstring &pipe, std::vector<VolumeStatus> &vols, bool &allow_disk_writes, std::string &err)
{
	std::vector<std::string> lines;
	vols.clear();
	allow_disk_writes = false;
	if (ControlRequest(pipe, "list", lines, err) != 0)
		return false;
	for (const auto &l : lines)
	{
		std::vector<std::string> f = SplitTabs(l);
		if (f.size() >= 8 && f[0] == "vol")
		{
			VolumeStatus vs;
			vs.uuid = f[1];
			vs.name = f[2];
			vs.state = f[3];
			vs.letter = f[4];
			vs.mode = f[5];
			vs.disk = f[6];
			vs.note = f[7];
			if (f.size() >= 9)
				vs.hide_dotfiles = f[8] != "0";
			if (f.size() >= 10)
				vs.read_bytes = std::strtoull(f[9].c_str(), nullptr, 10);
			if (f.size() >= 11)
				vs.write_bytes = std::strtoull(f[10].c_str(), nullptr, 10);
			vols.push_back(std::move(vs));
		}
		else if (f.size() >= 3 && f[0] == "cfg" && f[1] == "AllowDiskWrites")
			allow_disk_writes = f[2] == "1";
	}
	return true;
}

} // namespace winapfs
