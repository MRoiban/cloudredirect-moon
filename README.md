# cloudredirect-moon

A fork of [CloudRedirect](https://github.com/Selectively11/CloudRedirect). This
fork adds cross-distro attach fixes, legacy save-layout healing, and
worker-thread crash containment on the Linux 32-bit hook.

The result is a 32-bit `cloud_redirect.so`, loaded into Steam via `LD_PRELOAD`.

## Windows build

On x86_64 Linux, run `./build-windows.sh` with Podman or Docker installed.
It builds the container image if missing, then cross-compiles Release x64
`build/win/cloud_redirect.dll` and `build/win/cloud_redirect_cli.exe` and prints
their SHA-256 hashes. Keep both files together; the CLI loads the adjacent DLL.
The Windows x64 Visual C++ runtime must be installed on the target machine.

The image uses LLVM 19 and checksum-verified xwin 0.6.5 with MSVC CRT
14.44.17.14 and Windows SDK 10.0.26100. Building the image accepts Microsoft's
SDK license. Reuse the same image and source commit for repeatable binaries;
`/Brepro` removes build timestamps. To refresh the image after a Dockerfile
change, run `podman build -t cloudredirect-windows-builder -f docker/windows/Dockerfile docker/windows`
(or replace `podman` with `docker`). The Linux `./build.sh` is unchanged.

Cabinet pairing works through `cloud_redirect_cli.exe cabinet-connect <code>`
(optional `--server <origin>`, default `https://cabinet.mc2.onl`). Config and
DPAPI-encrypted credentials live in `%APPDATA%\CloudRedirect\`. Set
`"auto_update_dll": false` in `config.json` when another tool manages the DLL.
Managed Windows games use the redirect provider even if owned; games outside
the managed set keep Valve's Steam Cloud.

## Credits

Upstream:

- [Selectively11](https://github.com/Selectively11) and contributors —
  the CloudRedirect hook this fork builds on.

## Support

Open an issue: https://github.com/swwayps/cloudredirect-moon/issues
