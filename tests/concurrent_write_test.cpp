// winapfs - APFS for Windows
// Copyright (C) 2026 winapfs contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Multi-threaded Win32 concurrent write stress test through a live --rw
// WinFsp mount (parallel file churn, concurrent compressed-file overwrites,
// concurrent ADS/xattrs, and directory queries).

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_log_mutex;
std::atomic<int> g_failures{ 0 };

void Fail(const char *msg, DWORD err = 0)
{
	std::lock_guard<std::mutex> lk(g_log_mutex);
	if (err)
		fprintf(stderr, "FAIL: %s (win32 %lu)\n", msg, static_cast<unsigned long>(err));
	else
		fprintf(stderr, "FAIL: %s\n", msg);
	g_failures.fetch_add(1, std::memory_order_relaxed);
}

std::vector<uint8_t> MakePattern(uint32_t seed, size_t size)
{
	std::vector<uint8_t> v(size);
	uint32_t x = seed * 2654435761u | 1u;
	for (size_t i = 0; i < size; i++)
	{
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		v[i] = static_cast<uint8_t>(x >> 16);
	}
	return v;
}

bool WriteAllChunks(HANDLE h, const uint8_t *data, size_t len, size_t chunk)
{
	size_t off = 0;
	while (off < len)
	{
		DWORD n = static_cast<DWORD>((std::min)(chunk, len - off));
		DWORD got = 0;
		if (!WriteFile(h, data + off, n, &got, nullptr) || got != n)
			return false;
		off += got;
	}
	return true;
}

bool ReadAllExact(const std::wstring &path, std::vector<uint8_t> &out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER li = {};
	if (!GetFileSizeEx(h, &li))
	{
		CloseHandle(h);
		return false;
	}
	out.resize(static_cast<size_t>(li.QuadPart));
	size_t off = 0;
	while (off < out.size())
	{
		DWORD want = static_cast<DWORD>((std::min<size_t>)(65536, out.size() - off));
		DWORD got = 0;
		if (!ReadFile(h, out.data() + off, want, &got, nullptr) || got == 0)
		{
			CloseHandle(h);
			return false;
		}
		off += got;
	}
	CloseHandle(h);
	return true;
}

struct KeptFile
{
	std::wstring path;
	std::vector<uint8_t> expected;
};

void WorkerFileChurn(int tid, const std::wstring &base_dir, const std::wstring &shared_dir,
	const std::atomic<bool> &go, std::vector<KeptFile> &kept)
{
	while (!go.load(std::memory_order_acquire))
		std::this_thread::yield();

	constexpr int kIters = 25;
	for (int it = 0; it < kIters; it++)
	{
		size_t full_sz = 16384u + static_cast<size_t>(((tid * 7 + it * 13) % 16) * 16384u);
		size_t chunk_sz = (it % 2 == 0) ? 8192u : 32768u;
		uint32_t seed = static_cast<uint32_t>(tid * 1000 + it + 1);
		std::vector<uint8_t> data = MakePattern(seed, full_sz);

		wchar_t name[64];
		_snwprintf_s(name, _TRUNCATE, L"w%d_f%02d.bin", tid, it);
		std::wstring p = base_dir + L"\\" + name;

		HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			Fail("WorkerFileChurn CreateFileW failed", GetLastError());
			continue;
		}
		if (!WriteAllChunks(h, data.data(), data.size(), chunk_sz))
		{
			Fail("WorkerFileChurn WriteFile failed", GetLastError());
			CloseHandle(h);
			continue;
		}
		if (!FlushFileBuffers(h))
		{
			Fail("WorkerFileChurn FlushFileBuffers failed", GetLastError());
			CloseHandle(h);
			continue;
		}

		// Truncate slightly on every 3rd iteration to stress extent shrinking
		if (it % 3 == 0)
		{
			size_t trunc_sz = full_sz - 137;
			LARGE_INTEGER pos = {};
			pos.QuadPart = static_cast<LONGLONG>(trunc_sz);
			if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) || !SetEndOfFile(h))
			{
				Fail("WorkerFileChurn SetEndOfFile failed", GetLastError());
				CloseHandle(h);
				continue;
			}
			data.resize(trunc_sz);
		}
		CloseHandle(h);

		std::vector<uint8_t> got;
		if (!ReadAllExact(p, got) || got != data)
		{
			Fail("WorkerFileChurn immediate readback mismatch");
			continue;
		}

		if (it % 2 == 0)
		{
			if (!DeleteFileW(p.c_str()))
				Fail("WorkerFileChurn DeleteFileW failed", GetLastError());
		}
		else
		{
			std::wstring dst = shared_dir + L"\\" + name;
			if (!MoveFileExW(p.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
			{
				Fail("WorkerFileChurn MoveFileExW failed", GetLastError());
				continue;
			}
			kept.push_back(KeptFile{ std::move(dst), std::move(data) });
		}
	}
}

void WorkerCompressedOverwrite(int tid, const std::wstring &mount_root, const std::atomic<bool> &go)
{
	while (!go.load(std::memory_order_acquire))
		std::this_thread::yield();

	const std::wstring targets[] = {
		mount_root + L"\\compressed\\text_64KiB.txt",
		mount_root + L"\\inline\\zlib_16KiB_As.txt",
	};

	for (int round = 0; round < 10; round++)
	{
		for (size_t t = 0; t < _countof(targets); t++)
		{
			std::vector<uint8_t> payload = MakePattern(static_cast<uint32_t>(9000 + tid * 100 + round), 4096u + (round * 512u));
			HANDLE h = CreateFileW(targets[t].c_str(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
			{
				DWORD err = GetLastError();
				Fail("WorkerCompressedOverwrite open failed", err);
				continue;
			}
			DWORD written = 0;
			if (!WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) || written != payload.size())
				Fail("WorkerCompressedOverwrite WriteFile failed", GetLastError());
			if (!FlushFileBuffers(h))
				Fail("WorkerCompressedOverwrite FlushFileBuffers failed", GetLastError());
			CloseHandle(h);
		}
	}
}

void WorkerAdsAndQueries(int tid, const std::wstring &base_dir, const std::wstring &shared_dir, const std::atomic<bool> &go)
{
	while (!go.load(std::memory_order_acquire))
		std::this_thread::yield();

	wchar_t fname[64];
	_snwprintf_s(fname, _TRUNCATE, L"ads_%d.txt", tid);
	std::wstring file = base_dir + L"\\" + fname;
	HANDLE hf = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hf == INVALID_HANDLE_VALUE)
	{
		Fail("WorkerAdsAndQueries create base file failed", GetLastError());
		return;
	}
	const char init_data[] = "ads-host\n";
	DWORD w = 0;
	WriteFile(hf, init_data, sizeof(init_data) - 1, &w, nullptr);
	CloseHandle(hf);

	std::wstring stream = file + L":winapfs_test";
	for (int i = 0; i < 30; i++)
	{
		std::vector<uint8_t> sdata = MakePattern(static_cast<uint32_t>(5000 + tid * 50 + i), 128u + static_cast<size_t>(i * 32));
		HANDLE hs = CreateFileW(stream.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hs == INVALID_HANDLE_VALUE)
		{
			Fail("WorkerAdsAndQueries create stream failed", GetLastError());
			continue;
		}
		DWORD sw = 0;
		if (!WriteFile(hs, sdata.data(), static_cast<DWORD>(sdata.size()), &sw, nullptr) || sw != sdata.size())
			Fail("WorkerAdsAndQueries write stream failed", GetLastError());
		CloseHandle(hs);

		std::vector<uint8_t> got;
		if (!ReadAllExact(stream, got) || got != sdata)
			Fail("WorkerAdsAndQueries readback stream mismatch");

		if (i % 3 == 0 && !DeleteFileW(stream.c_str()))
			Fail("WorkerAdsAndQueries delete stream failed", GetLastError());

		// Concurrent directory enumeration and single-name attribute queries
		WIN32_FIND_DATAW fd = {};
		HANDLE ff = FindFirstFileW((shared_dir + L"\\*").c_str(), &fd);
		if (ff != INVALID_HANDLE_VALUE)
		{
			while (FindNextFileW(ff, &fd))
			{
			}
			FindClose(ff);
		}
		GetFileAttributesW(file.c_str());
	}
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: winapfs-concurrent-test <mount_root>\n");
		return 2;
	}

	std::wstring root = argv[1];
	while (!root.empty() && root.back() == L'\\')
		root.pop_back();

	std::wstring base_dir = root + L"\\concurrent_test";
	std::wstring shared_dir = base_dir + L"\\shared";
	if (!CreateDirectoryW(base_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		fprintf(stderr, "cannot create %ls (win32 %lu)\n", base_dir.c_str(), GetLastError());
		return 1;
	}
	if (!CreateDirectoryW(shared_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
	{
		fprintf(stderr, "cannot create %ls (win32 %lu)\n", shared_dir.c_str(), GetLastError());
		return 1;
	}

	std::atomic<bool> go{ false };
	std::vector<KeptFile> kept[4];
	std::vector<std::thread> threads;

	for (int i = 0; i < 4; i++)
		threads.emplace_back(WorkerFileChurn, i, std::cref(base_dir), std::cref(shared_dir), std::cref(go), std::ref(kept[i]));
	for (int i = 0; i < 2; i++)
		threads.emplace_back(WorkerCompressedOverwrite, i, std::cref(root), std::cref(go));
	for (int i = 0; i < 2; i++)
		threads.emplace_back(WorkerAdsAndQueries, i, std::cref(base_dir), std::cref(shared_dir), std::cref(go));

	go.store(true, std::memory_order_release);
	for (auto &t : threads)
		t.join();

	size_t verified = 0;
	for (int i = 0; i < 4; i++)
	{
		for (const auto &k : kept[i])
		{
			std::vector<uint8_t> got;
			if (!ReadAllExact(k.path, got) || got != k.expected)
			{
				Fail("post-join verification failed");
				continue;
			}
			verified++;
		}
	}

	const wchar_t *compressed_checks[] = {
		L"\\compressed\\text_64KiB.txt",
		L"\\inline\\zlib_16KiB_As.txt",
	};
	for (const wchar_t *rel : compressed_checks)
	{
		std::vector<uint8_t> got;
		if (!ReadAllExact(root + rel, got) || got.empty())
			Fail("post-join compressed file read failed");
	}

	int fails = g_failures.load(std::memory_order_relaxed);
	printf("concurrent_write_test: 8 threads finished, verified %zu kept files, failures=%d\n", verified, fails);
	return fails == 0 ? 0 : 1;
}
