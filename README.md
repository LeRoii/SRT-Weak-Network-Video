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
topology as the WebRTC demo. A process started on the host does not use this
link: binding the receiver to `0.0.0.0` only covers interfaces in the
receiver's current network namespace.

Create the topology, then start the receiver in `webrtc_rx`:

```bash
sudo ./scripts/netem_loss.sh ns-up
sudo ip netns exec webrtc_rx ./build/srt_weak_video \
  --role receiver \
  --listen 10.88.0.2:9000 \
  --output-file /tmp/srt-received.h264 \
  --no-display
```

Start the sender in another terminal in `webrtc_tx`:

```bash
sudo ip netns exec webrtc_tx ./build/srt_weak_video \
  --role sender \
  --connect 10.88.0.2:9000 \
  --video-file /path/to/input.mp4 \
  --max-video-kbps 2000
```

After the connection and media flow are established, apply loss in a third
terminal:

```bash
sudo ./scripts/netem_loss.sh ns-loss 50 50 both
sudo ./scripts/netem_loss.sh ns-show
```

Use `sudo ./scripts/netem_loss.sh ns-clear both` to remove the impairment and
`sudo ./scripts/netem_loss.sh ns-down` after both processes have stopped.

Validate the recovered elementary stream:

```bash
ffmpeg -v error -err_detect explode \
  -i /tmp/srt-received.h264 -f null -
```

At extreme loss the receiver deliberately freezes until a complete keyframe is
recovered. It never forwards incomplete or CRC-invalid frames to the decoder.
The SDL window and its last decoded frame stay alive while SRT reconnects.

The reference validation used `/home/u20/code/jetson-2k.mp4`, 30% loss in both
directions, and 50 ms delay. Complete frames continued throughout the loss
period at a reduced profile, the receiver reported zero decoder errors, and
the saved H.264 stream passed FFmpeg's `-err_detect explode` check.
