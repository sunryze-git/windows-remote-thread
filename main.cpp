// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>

namespace
{

std::wstring format_error(DWORD error)
{
	wchar_t * buffer = nullptr;
	DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                            nullptr,
	                            error,
	                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	                            reinterpret_cast<wchar_t *>(&buffer),
	                            0,
	                            nullptr);

	std::wstring message;
	if (size != 0 && buffer != nullptr)
	{
		message.assign(buffer, size);
		while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
			message.pop_back();
	}

	LocalFree(buffer);
	return message;
}

void report_error(const wchar_t * operation)
{
	DWORD error = GetLastError();
	std::fwprintf(stderr, L"%ls failed (%lu): %ls\n", operation, error, format_error(error).c_str());
}

bool enable_debug_privilege()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return false;

	TOKEN_PRIVILEGES privileges{};
	privileges.PrivilegeCount = 1;
	if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid))
	{
		CloseHandle(token);
		return false;
	}

	privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	BOOL ok = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
	DWORD error = GetLastError();
	CloseHandle(token);
	return ok != FALSE && error == ERROR_SUCCESS;
}

bool same_architecture(HANDLE process)
{
	BOOL target_wow64 = FALSE;
	BOOL self_wow64 = FALSE;
	if (!IsWow64Process(process, &target_wow64) || !IsWow64Process(GetCurrentProcess(), &self_wow64))
		return false;
	return target_wow64 == self_wow64;
}

bool find_module_base(DWORD pid, const wchar_t * name, std::uintptr_t & base)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (snapshot == INVALID_HANDLE_VALUE)
		return false;

	MODULEENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	bool found = false;
	if (Module32FirstW(snapshot, &entry))
	{
		do
		{
			if (CompareStringOrdinal(entry.szModule, -1, name, -1, TRUE) == CSTR_EQUAL)
			{
				base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
				found = true;
				break;
			}
		} while (Module32NextW(snapshot, &entry));
	}

	CloseHandle(snapshot);
	return found;
}

bool resolve_remote_proc(DWORD pid, const wchar_t * module, const char * name, std::uintptr_t & address)
{
	auto local_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(module));
	if (local_base == 0)
		return false;

	FARPROC local_proc = GetProcAddress(reinterpret_cast<HMODULE>(local_base), name);
	if (local_proc == nullptr)
		return false;

	std::uintptr_t remote_base = 0;
	if (!find_module_base(pid, module, remote_base))
		return false;

	address = remote_base + (reinterpret_cast<std::uintptr_t>(local_proc) - local_base);
	return true;
}

std::string to_utf8(const std::wstring & value)
{
	if (value.empty())
		return {};

	int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	std::string result(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
	return result;
}

bool run_remote_thread(HANDLE process, std::uintptr_t start, void * argument, DWORD & exit_code)
{
	HANDLE thread = CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), argument, 0, nullptr);
	if (thread == nullptr)
	{
		report_error(L"CreateRemoteThread");
		return false;
	}

	bool success = WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0;
	if (!success)
		report_error(L"WaitForSingleObject");
	else if (!GetExitCodeThread(thread, &exit_code))
	{
		report_error(L"GetExitCodeThread");
		success = false;
	}

	CloseHandle(thread);
	return success;
}

bool load_dll(HANDLE process, DWORD pid, const std::filesystem::path & dll_path)
{
	std::wstring path = dll_path.wstring();
	const SIZE_T size = (path.size() + 1) * sizeof(wchar_t);
	void * remote_path = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (remote_path == nullptr)
	{
		report_error(L"VirtualAllocEx");
		return false;
	}

	bool success = false;
	if (!WriteProcessMemory(process, remote_path, path.c_str(), size, nullptr))
	{
		report_error(L"WriteProcessMemory");
	}
	else
	{
		std::uintptr_t load_library = 0;
		if (!resolve_remote_proc(pid, L"kernel32.dll", "LoadLibraryW", load_library))
		{
			std::fwprintf(stderr, L"could not resolve LoadLibraryW in the target process\n");
		}
		else
		{
			DWORD exit_code = 0;
			if (run_remote_thread(process, load_library, remote_path, exit_code))
			{
				std::uintptr_t module_base = 0;
				success = find_module_base(pid, dll_path.filename().c_str(), module_base);
				if (!success)
					std::fwprintf(stderr, L"the target process did not load the DLL\n");
			}
		}
	}

	VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
	return success;
}

FARPROC find_local_export(HMODULE module, const std::string & name)
{
	FARPROC proc = GetProcAddress(module, name.c_str());
	if (proc != nullptr)
		return proc;

	std::string decorated = "_" + name + "@4";
	return GetProcAddress(module, decorated.c_str());
}

bool call_export(HANDLE process, DWORD pid, const std::filesystem::path & dll_path, const std::string & export_name, const std::wstring & argument)
{
	std::uintptr_t module_base = 0;
	if (!find_module_base(pid, dll_path.filename().c_str(), module_base))
	{
		std::fwprintf(stderr, L"injected module not found\n");
		return false;
	}

	HMODULE local_module = LoadLibraryExW(dll_path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (local_module == nullptr)
	{
		report_error(L"LoadLibraryExW");
		return false;
	}

	FARPROC local_proc = find_local_export(local_module, export_name);
	std::uintptr_t rva = 0;
	if (local_proc != nullptr)
		rva = reinterpret_cast<std::uintptr_t>(local_proc) - reinterpret_cast<std::uintptr_t>(local_module);

	FreeLibrary(local_module);

	if (local_proc == nullptr)
	{
		std::fwprintf(stderr, L"export not found: %hs\n", export_name.c_str());
		return false;
	}

	void * remote_argument = nullptr;
	SIZE_T argument_size = 0;
	if (!argument.empty())
	{
		argument_size = (argument.size() + 1) * sizeof(wchar_t);
		remote_argument = VirtualAllocEx(process, nullptr, argument_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (remote_argument == nullptr)
		{
			report_error(L"VirtualAllocEx");
			return false;
		}

		if (!WriteProcessMemory(process, remote_argument, argument.c_str(), argument_size, nullptr))
		{
			report_error(L"WriteProcessMemory");
			VirtualFreeEx(process, remote_argument, 0, MEM_RELEASE);
			return false;
		}
	}

	DWORD exit_code = 0;
	bool success = run_remote_thread(process, module_base + rva, remote_argument, exit_code);
	if (success && FAILED(static_cast<HRESULT>(exit_code)))
	{
		std::fwprintf(stderr, L"remote function failed: 0x%08lX\n", exit_code);
		success = false;
	}

	if (remote_argument != nullptr)
		VirtualFreeEx(process, remote_argument, 0, MEM_RELEASE);

	return success;
}

bool inject_dll(DWORD pid, const std::filesystem::path & dll_path, const std::string & export_name, const std::wstring & argument)
{
	HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
	if (process == nullptr)
	{
		report_error(L"OpenProcess");
		return false;
	}

	bool success = false;
	if (same_architecture(process))
	{
		success = load_dll(process, pid, dll_path);
		if (success && !export_name.empty())
			success = call_export(process, pid, dll_path, export_name, argument);
	}
	else
	{
		std::fwprintf(stderr, L"target process architecture does not match this application\n");
	}

	CloseHandle(process);
	return success;
}

}

int wmain(int argc, wchar_t ** argv)
{
	if (argc < 3 || argc > 5)
	{
		std::fwprintf(stderr, L"usage: %ls <pid> <dll-path> [export] [argument]\n", argv[0]);
		return 1;
	}

	wchar_t * end = nullptr;
	unsigned long pid = std::wcstoul(argv[1], &end, 10);
	if (end == argv[1] || *end != L'\0' || pid == 0 || pid > MAXDWORD)
	{
		std::fwprintf(stderr, L"invalid pid: %ls\n", argv[1]);
		return 1;
	}

	std::error_code ec;
	std::filesystem::path dll_path = std::filesystem::absolute(argv[2], ec);
	if (ec || !std::filesystem::is_regular_file(dll_path, ec))
	{
		std::fwprintf(stderr, L"invalid DLL path: %ls\n", argv[2]);
		return 1;
	}

	std::string export_name;
	std::wstring argument;
	if (argc >= 4)
		export_name = to_utf8(argv[3]);
	if (argc >= 5)
		argument = argv[4];

	if (!enable_debug_privilege())
		std::fwprintf(stderr, L"warning: SeDebugPrivilege is not available\n");

	if (!inject_dll(static_cast<DWORD>(pid), dll_path, export_name, argument))
		return 1;

	std::wprintf(L"remote thread completed in process %lu\n", pid);
	return 0;
}
