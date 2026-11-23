# Windows build and packaging

Use the MSYS2 **MINGW64** environment for the native client, .NET SDK 10
for the Avalonia launcher and Inno Setup 6.6 or later for the installer.

Clone with submodules:

```powershell
git clone --recurse-submodules https://github.com/Q1W1RTY/PS5Link.git
cd PS5Link
```

Install the native build tools and libraries from the **MINGW64** shell:

```bash
pacman -S --needed git mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,python,python-protobuf,protobuf,ffmpeg,libplacebo,qt6-base,qt6-declarative,qt6-svg,sdl2-compat,sdl3,openssl,opus,speexdsp,json-c,miniupnpc,libevent,libpsl,libssh2,nghttp2,vulkan-loader,vulkan-headers}
```

Run the packaging script from PowerShell:

```powershell
./scripts/build-installer.ps1
```

The script configures and builds the native client, runs CTest, publishes the
self-contained launcher, stages the MSYS2 dependencies and compiles the
installer. The installer and SHA-256 checksum are written to
`build-offline/release`. Use `-MsysRoot` and `-Iscc` for non-default tool
locations. The staged payload includes the exact MSYS2 dependency versions and
links to their source recipes and patches under `LICENSES/MSYS2`.

Optional GPU modules can be built for local development using
[`scripts/build-bridges.ps1`](../../scripts/build-bridges.ps1) with
compatible vendor SDKs and Visual Studio x64 build tools. Those modules,
game clips and local console data are outside the standard installer payload.

The installer adds a per-user Start menu entry and can add a desktop shortcut.
It supports English and Russian. Windows Settings can uninstall the app.
Profiles and pairing data live in the Windows user's settings and are
independent of the installation directory.
