# Windows remote thread example

This example creates a thread in another Windows process, loads a DLL into that process, and can call an exported function from that DLL with one wide-string argument (a null-terminated `wchar_t` string). A payload here is the DLL that the injector loads. The source tree includes two payload DLLs: `payload.dll` as a minimal test payload and `audio_payload.dll` for Media Foundation audio playback.

The process you inject into is the target. Use this project only on processes and systems you own or have permission to test.

## Requirements

- Windows 10 or later.
- CMake 3.20 or later.
- Visual Studio 2022 with the Desktop development with C++ workload, which includes the Windows SDK, or MinGW-w64. Use either one; you do not need both.
- Media Foundation, the Windows media pipeline used by `audio_payload.dll`. No separate install is needed on Windows 10 or later, except on N editions. Windows N editions are sold without some media components; install the Media Feature Pack from Microsoft.
- `remote_thread.exe`, the target, and both payload DLLs must use the same architecture: either all x86-64 or all 32-bit x86.

## Build

From the repository root, the directory that contains `CMakeLists.txt`:

```sh
cmake -B build -S .
cmake --build build --config Release
```

The first command uses CMake's default Visual Studio generator on Windows with Visual Studio installed. For MinGW Makefiles, make sure the MinGW-w64 toolchain from Requirements is on the `PATH` environment variable, then add `-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release` to the configure command.

Visual Studio keeps separate Debug and Release directories, so the outputs are in `build/Release/`:

- `remote_thread.exe`
- `payload.dll`
- `audio_payload.dll`

MinGW Makefiles writes directly to `build/`. `CMakeLists.txt` adds `-static` for MinGW, so the executables and DLLs run without the MinGW C++ runtime DLLs (`libstdc++-6.dll`, `libgcc_s_*.dll`).

## Usage

`remote_thread.exe` is the injector. A remote thread is a thread created in another process with `CreateRemoteThread`. Run the commands from the directory that contains the DLLs; for a source build, that is `build/Release/` or `build/`.

Find the target's process ID in Task Manager's Details tab, or run `tasklist` in a command prompt. Replace `1234` in the examples with that process ID. Then run:

```sh
remote_thread.exe <pid> <dll-path> [export] [argument]
```

The injector always creates a thread whose entry point is `LoadLibraryW` (the Windows API that loads a DLL), with the DLL path as its argument. Without an export name it stops there. With one, it resolves the export (finds the function address with `GetProcAddress`) in the target and calls it on a second remote thread. The second thread passes the argument to the export. The export argument is copied into the target as a null-terminated wide string. If you give an export name but no argument, the export receives a null pointer. The fourth argument is only used when an export name is given.

The injector converts the DLL path to an absolute path. Use an absolute path for the audio file as well; the target resolves relative paths against its own working directory.

The demo payload, `payload.dll`, shows a message box:

```sh
remote_thread.exe 1234 payload.dll
```

The target must run in a user desktop session, not a Windows service session, for the message box to appear.

The audio payload plays a file through the default audio device:

```sh
remote_thread.exe 1234 audio_payload.dll PlayAudioFileW C:\media\example.mp3
```

The audio payload plays formats supported by Media Foundation, such as MP3, AAC, WAV, and WMA.

Playback runs on the second remote thread, and the injector waits for that thread, so `remote_thread.exe` returns after the file finishes.

The injector requests the `SeDebugPrivilege` privilege, which lets an administrator open processes owned by other users. If the privilege is not available, it continues without it. If the injector cannot open the target because the target is running as administrator, run `remote_thread.exe` from an elevated command prompt (Run as administrator).

On success the injector prints `remote thread completed in process <pid>` and exits with status 0. On failure it prints the failed Windows API call and its error code to stderr and exits with status 1.

## Implementation

`remote_thread.exe`:

1. Opens the target with `PROCESS_CREATE_THREAD`, `PROCESS_QUERY_INFORMATION`, `PROCESS_VM_OPERATION`, and `PROCESS_VM_WRITE`.
2. Writes the DLL path into the target with `VirtualAllocEx` and `WriteProcessMemory`.
3. Creates a thread at `LoadLibraryW` in the target process, waits for it, and checks that the loaded DLL appears in the target's module list.
4. If an export name is given, finds the loaded module base by name in a Toolhelp32 module snapshot (the Windows API that lists loaded modules), resolves the export address, writes the argument, and creates a second thread at that address. It waits for the second thread to finish.

`payload.dll` shows a message box from `DllMain`, the DLL entry point, when it is loaded. Running code under the loader lock is not recommended for a real payload.

`audio_payload.dll` exports `PlayAudioFileW`. It starts Media Foundation, opens the file with `IMFSourceResolver`, connects the audio source to the default audio renderer, starts an `IMFMediaSession`, and waits for `MESessionEnded`.

## Release

The prebuilt GitHub release asset is `windows-remote-thread-x86_64.zip` on the [releases page](https://github.com/sunryze-git/windows-remote-thread/releases). It contains `remote_thread.exe`, `payload.dll`, `audio_payload.dll`, the README, and the GPL-3.0-or-later license (`LICENSE`). The release page and assets are visible only to accounts with access to the private repository. The prebuilt release is x86-64 only; for x86, use a 32-bit Visual Studio platform or a 32-bit MinGW-w64 toolchain. Extract the archive and run the commands in Usage from the extracted directory.

See [RELEASING.md](RELEASING.md) for the build and publish procedure.
