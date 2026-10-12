# Usage

## Capture and playback

1. Choose photo, four-photo collage or video at the bottom left.
2. Press the red shutter or **Space**. Press again to stop a video.
3. Click a gallery thumbnail to view a photo or play a video. Playback has
   volume, play/pause and seek controls. **Esc** returns to the camera and
   cancels an active photo countdown.
4. Right-click a thumbnail to open it externally or move it to the system trash.

Photos are JPEG; recordings are MP4 with H.264 video and optional mono AAC
at 48 kHz. Mirror, rotation and effects apply to both preview and saved media.
Resolution and frame rate are chosen from the camera's supported modes;
requesting a higher setting does not add capabilities to the device.

## Options

- Select camera, microphone and audio output; the initial audio selection
  follows the system default.
- Set microphone gain, recording sound, capture quality, frame rate and timer.
- Mirror or rotate the image. Effects include normal, black and white, and sepia.
- Adjust window opacity. The default is 80% black and 20% transparency, with
  no requested blur. The donations panel has an opaque dark background.
- **Test microphone and sound** shows an input meter, plays a test tone, and
  records/replays a three-second sample.
- Refresh devices after plugging in a camera or microphone.
- Choose Automatic, English or Spanish. Automatic uses the KDE display
  language when available, otherwise the system locale; other languages use English.

Window decorations follow the desktop theme. Preview pauses while reviewing
media or when the window is hidden. Recording continues while minimized;
closing the app finalizes an active recording.

## Files and privacy

- Captures: `~/Pictures/Camera/` (created automatically).
- Settings: `~/.config/Samera/Samera.conf`.
- Thumbnail cache: the system cache directory under `Samera/Samera/`.

Captures remain local. Samera has no telemetry or upload service. The PayPal
button opens the donation link in your browser only when clicked. Audio tests
use temporary files. Photos are saved atomically; videos use `.part` until
finalized. Failed recordings may leave a `.part` file for manual recovery.

## Troubleshooting

**No camera:** close other camera apps, select the correct device and refresh
inputs. Check device permissions and your distribution's webcam/PipeWire setup.
An infrared input may produce an unusual picture: select your color camera.

**Slow preview:** lower the requested resolution or frame rate. USB bandwidth,
lighting and CPU capacity affect capture and software H.264 encoding.

**No recorded sound:** enable recording sound, select the intended microphone,
check the input meter and system mute/gain settings. Run the three-second audio
test. For playback, check both app volume and the selected system output.

**Missing codec:** install all runtime packages listed in the README. Recording
requires `x264enc`, `h264parse`, `mp4mux`, `avenc_aac` and `aacparse`.

**No transparency:** enable compositing in the desktop. The app does not modify
KDE effects or enable blur.

For a bug report, include your distribution, desktop/session (Wayland or X11),
Qt/GStreamer versions, camera/microphone model and reproduction steps. Review
logs before posting them, and avoid attaching private captures or configuration.
