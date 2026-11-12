# Releasing

These commands build `windows-remote-thread-x86_64.zip` and publish it as a GitHub release. Run them from the repository root, the directory that contains `CMakeLists.txt`.

Use Git Bash, MSYS2, or WSL. The `cp`, `mkdir`, `rm`, and `zip` commands are not available in `cmd.exe`.

## Build

On a Windows machine with CMake and a compiler.

Visual Studio:

```sh
cmake -B build -S . -A x64
cmake --build build --config Release
```

MinGW Makefiles, with an x86-64 MinGW-w64 toolchain on `PATH`:

```sh
cmake -B build -S . -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Visual Studio writes the binaries to `build/Release/`; MinGW Makefiles writes them to `build/`.

Remove old build outputs. The snippet below uses the Visual Studio path; change `bin=build/Release` to `bin=build` for MinGW. The repository root contains `CMakeLists.txt`, `README.md`, and `LICENSE`.

```sh
bin=build/Release
rm -rf dist windows-remote-thread-x86_64.zip
mkdir dist
cp "$bin/remote_thread.exe" dist/
cp "$bin/payload.dll" dist/
cp "$bin/audio_payload.dll" dist/
cp README.md LICENSE dist/
```

Create the archive:

```sh
cd dist
zip -r ../windows-remote-thread-x86_64.zip .
cd ..
```

## Publish

The GitHub CLI must be installed and authenticated. Run `gh auth login` if needed. The first time you publish, the GitHub repository must not already exist so `gh repo create` can create it. For later releases, commit your changes and run `git push origin HEAD` instead of running `gh repo create`.

The current directory must be a git repository with at least one commit. If it is not, run `git init` and commit the files first.

Create the private repository and push the source:

```sh
gh repo create sunryze-git/windows-remote-thread --private --source=. --remote=origin --push
```

The command creates a private repository, so the release page and assets are visible only to accounts with access.

Create a tag and release. Replace `v1.0.0` with the new version for later releases and update the `--notes` text. The archive is in the repository root after the `cd ..` above. If the tag or release already exists, use a new version.

```sh
git tag v1.0.0
git push origin v1.0.0
gh release create v1.0.0 windows-remote-thread-x86_64.zip \
  --repo sunryze-git/windows-remote-thread \
  --title "v1.0.0" \
  --notes "Prebuilt x86-64 Windows binaries. See README.md for usage."
```

Check the uploaded asset:

```sh
gh release view v1.0.0 --repo sunryze-git/windows-remote-thread
```
