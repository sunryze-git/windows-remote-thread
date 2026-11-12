// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);
		MessageBoxW(nullptr, L"The remote thread loaded this DLL.", L"Remote thread payload", MB_OK | MB_ICONINFORMATION);
	}
	return TRUE;
}
