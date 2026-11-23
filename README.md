<p align="center">
  <img src="gui/assets/ps5link/PS5Link.png" width="72" alt="PS5Link icon">
</p>

<h1 align="center">PS5Link</h1>

<p align="center">PlayStation Remote Play for Windows</p>

<p align="center">
  <a href="README.ru.md">Русский</a> ·
  <a href="https://github.com/Q1W1RTY/PS5Link/releases/latest">Download for Windows</a> ·
  <a href="docs/ps5link/BUILD.md">Build from source</a>
</p>

<p align="center">
  <img src="docs/ps5link/launcher.png" width="390" alt="PS5Link launcher with collapsed settings categories">
</p>

PS5Link streams your PS5 or PS4 to a Windows PC. The Avalonia launcher gives
you a single **Connect** button, graphics presets and collapsible settings
for video, network, controllers and appearance.

## Install

1. Download **PS5Link-1.10.1-win-x64-setup.exe** from
   [Releases](https://github.com/Q1W1RTY/PS5Link/releases/latest).
2. Run Setup in English or Russian. It installs for the current Windows user
   and adds PS5Link to the Start menu. A desktop shortcut is optional.
3. Open **PS5Link** from the Start menu.

The bundle includes the app runtimes. The installer is unsigned.

## Connect a console

1. On PlayStation, enable Remote Play in system settings.
2. In the launcher, open **Advanced settings → Consoles → Register New**.
   Use the pairing code shown by the console. For access outside your local
   network, sign in to PlayStation Network in the **Remote** tab.
3. Close the settings window and press **Connect** in the launcher.
   PS5Link uses the console marked for auto-connect or the only registered
   console. If a manual choice is needed, it opens the console list.

Press **Ctrl+O** during a stream for the in-game menu. The **RU / EN** button
changes the launcher language. The client reads it the next time it opens.

## Video and input

| | |
| --- | --- |
| Video | Direct3D 12 and standard renderer, hardware decoding, HDR output on compatible hardware |
| Scaling | Shader scaling, AMD FSR 1.0, optional NVIDIA RTX VSR and DLSS |
| Frame generation | Built-in interpolation, optional DLSS FG and AMD FSR 3.1 FG |
| Input | Keyboard mapping, DualSense adaptive triggers, haptics and lightbar controls |
| Network | Packet buffer presets and keyframe requests for video recovery |

The clean installer includes the standard renderer, Direct3D 12, FSR 1.0
and built-in interpolation. RTX VSR, DLSS and FSR 3.1 FG require separate
compatible runtime modules. The **Without RTX** preset works with the
installed components.

## Requirements and source

- Windows 10 (version 2004 or later) or Windows 11, x64.
- A PS5 or PS4 with Remote Play enabled and a PC connected to it over the
  network. Graphics features depend on the installed GPU and its driver.
- [Windows build and packaging instructions](docs/ps5link/BUILD.md).

PS5Link is based on [chiaki-ng](https://github.com/chiaki-ng/chiaki-ng) and
distributed under the [AGPL-3.0 with an OpenSSL exception](LICENSES/AGPL-3.0-only-OpenSSL.txt).
The installer includes third-party notices. PlayStation is a trademark of
Sony Interactive Entertainment. PS5Link is an independent project.
