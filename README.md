# SRT Weak-Network Video

Native Linux sender/receiver for complete-frame video delivery over severe
packet loss. The transport combines:

- SRT live message transport with a 350 ms latency budget and deadline-aware ARQ
- per-frame Reed-Solomon erasure coding through Intel ISA-L
- seven adaptive H.264 profiles from 2000 kbps down to 8 kbps
- CRC validation and strict complete-frame/keyframe gating
- a receiver display that keeps the last good frame across disconnects

The project is independent from the libdatachannel WebRTC demo.

## Dependencies

Ubuntu 22.04:

```bash
sudo apt install cmake g++ git pkg-config nasm libisal-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libsdl2-dev
```

CMake downloads and builds SRT v1.5.5 inside `build/_deps`; it does not replace
the system SRT package. The build applies a local nonblocking-lock patch to
SRT's send and close paths. Without it, extreme bidirectional loss can leave
the public nonblocking API waiting indefinitely on an internal mutex.

## Build

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Run

Start the receiver:

```bash
./build/srt_weak_video \
  --role receiver \
  --listen 0.0.0.0:9000 \
  --output-file /tmp/srt-received.h264
```

Start the sender:

```bash
./build/srt_weak_video \
  --role sender \
  --connect 127.0.0.1:9000 \
  --video-file /path/to/input.mp4 \
  --max-video-kbps 2000
```

Use `--no-display` on the receiver for headless tests.

## Weak-Network Test

The copied `scripts/netem_loss.sh` creates the same sender/receiver namespace
topology as the WebRTC demo. Start the peers using port 9000, then apply loss:

```bash
sudo ./scripts/netem_loss.sh ns-up
sudo ./scripts/netem_loss.sh ns-loss 50 50 both
sudo ./scripts/netem_loss.sh ns-show
```

Validate the recovered elementary stream:

```bash
ffmpeg -v error -err_detect explode \
  -i /tmp/srt-received.h264 -f null -
```

At extreme loss the receiver deliberately freezes until a complete keyframe is
recovered. It never forwards incomplete or CRC-invalid frames to the decoder.
The SDL window and its last decoded frame stay alive while SRT reconnects.

The reference validation used `/home/u20/code/jetson-2k.mp4`, 50% loss in both
directions, and 50 ms delay. The receiver continued from 67 to 68 complete
frames after loss, reported zero decoder errors, and the saved H.264 stream
passed FFmpeg's `-err_detect explode` check. Persistent 50% loss may still
produce long freezes and repeated handshakes; that is intentional degradation
instead of displaying damaged frames.
