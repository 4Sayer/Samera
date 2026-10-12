# Build and packaging

## Dependencies

Build tools: C++17 compiler, CMake 3.20+, pkg-config, Qt 6.4+ Widgets/Multimedia
headers, and GStreamer 1.20+ app/video headers.

Runtime: Qt Widgets/Multimedia and the appropriate platform plugin (Wayland or
X11), GStreamer base/good/bad/ugly/libav plugins, and a PulseAudio-compatible
server. `pactl` improves microphone enumeration. A real camera is required for
normal capture. Required codec elements include `jpegdec`, `x264enc`,
`h264parse`, `mp4mux`, `avenc_aac`, `aacparse` and `pulsesrc`.

### Arch / CachyOS

Install the runtime packages in the README, then:

```sh
sudo pacman -S --needed base-devel cmake pkgconf
```

### Ubuntu 24.04 / compatible Debian systems

```sh
sudo apt install build-essential cmake pkg-config qt6-base-dev \
  qt6-multimedia-dev qt6-wayland libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  gstreamer1.0-pulseaudio gstreamer1.0-tools pulseaudio-utils
```

Package names can vary by distribution. References:
[Qt QImage API](https://doc.qt.io/qt-6/qimage.html),
[GStreamer Linux installation](https://gstreamer.freedesktop.org/documentation/installing/on-linux.html),
[Arch codec packages](https://archlinux.org/packages/extra/x86_64/gst-plugins-ugly/).

## Compile and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix "$HOME/.local"
```

The install includes the executable, desktop launcher, icon, license and docs.
For a system installation use an appropriate prefix and administrator privileges.
Source builds can target other architectures supported by Qt and GStreamer;
these are not all tested. No dependencies are downloaded by the build.

## Verify

```sh
./scripts/check.sh build/samera
```

The check uses an offscreen synthetic camera and tone, isolates preferences and
captures in a temporary directory, and checks photos, audio/video recording,
rotation, collage, capture restart and translations. It does not use a real
camera/microphone, change your settings or open PayPal.

For real hardware, open the app and use its audio test. Close other instances
before running `build/samera --smoke-test`: this runs the selected real camera
for 30 seconds and reports frames, resolution and recent frame rate.

## Release

```sh
cmake --build build --target package
```

CPack produces `samera-1.0.0-linux-<architecture>.tar.gz`, containing only the
installed app, desktop integration, docs and a per-user installer. It uses
system libraries, which are not bundled. A binary built on Arch/CachyOS must
be used with compatible library versions; compile from source elsewhere.

Create the source archive from a reviewed commit:

```sh
git archive --format=tar.gz --prefix=samera-1.0.0/ \
  -o samera-1.0.0-source.tar.gz v1.0.0
sha256sum samera-1.0.0-*.tar.gz > SHA256SUMS
```

Publish archives and checksums as GitHub release assets. Keep binaries, build
folders, logs, settings and private media out of Git. The GitHub workflow
builds from source and runs synthetic checks; hardware compatibility still
requires real-device testing.
