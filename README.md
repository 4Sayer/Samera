# Samera

A lightweight, open-source camera app for Linux, created by **xSayer**.
A minimal Photo Booth-inspired interface, built with **C++17, Qt 6 and GStreamer**.

- Photos, four-photo collages and video with microphone audio.
- Camera, microphone and audio output selection; microphone gain and audio test.
- Mirror, rotation, timer, quality and frame rate controls.
- Built-in gallery with photo viewing and video playback.
- Dark interface, adjustable transparency without blur, and native window controls.
- English and Spanish, with automatic system language detection.

Captures are saved to **`~/Pictures/Camera/`**. No account, telemetry or background service.
Samera is an independent Linux project; no Apple artwork or code is included.

## Install

Download a package from [Releases](https://github.com/4Sayer/Samera/releases).
The prebuilt **Arch/CachyOS x86_64** package uses system libraries; install the
runtime dependencies below, extract it, then run:

```sh
./install-user.sh
```

Open **Samera** from your application menu, or run `./bin/samera` without installing.
Other distributions should build from source; this binary is not a universal Linux package.

## Requirements and build

Linux with a webcam, Qt **6.4+** Widgets/Multimedia, GStreamer **1.20+**,
and PulseAudio or PipeWire with PulseAudio compatibility. KDE Plasma is the
primary tested desktop. Transparency depends on the compositor.

Arch/CachyOS runtime dependencies:

```sh
sudo pacman -S --needed qt6-base qt6-multimedia qt6-wayland \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad \
  gst-plugins-ugly gst-libav libpulse
```

To build, also install `base-devel`, `cmake` and `pkgconf`:

```sh
git clone https://github.com/4Sayer/Samera.git
cd Samera
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix "$HOME/.local"
```

Ensure `~/.local/bin` is in your desktop session's `PATH`, or run
`~/.local/bin/samera` directly. See [Build and packaging](docs/BUILD.md) for
Ubuntu dependencies, checks and release instructions.

## Use and support

**Space** captures a photo or starts/stops recording. Choose a capture mode at
the bottom left. **Options** contains devices and settings; click a thumbnail
to view a capture, then **Esc** to return to the camera.

See [Usage and troubleshooting](docs/USAGE.md).
Report bugs through [Issues](https://github.com/4Sayer/Samera/issues), or contact
**@xsayer** on Discord. Hardware quality and available frame rates depend on your camera.

## Donations

Samera is free and open source. If you enjoy it, your donation would help me
a lot and support the project. Thank you!

**[Donate with PayPal](https://www.paypal.com/paypalme/xsayer)**

## License

[MIT](LICENSE) · Copyright © 2026 xSayer.
