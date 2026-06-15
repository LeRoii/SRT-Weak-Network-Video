# Weak-Network Video

Native Linux sender/receiver for complete-frame video delivery over severe
packet loss. The transport combines:

- connectionless UDP as the default high-loss media transport
- optional SRT live transport for compatibility and comparison
- per-frame Reed-Solomon erasure coding through Intel ISA-L
- nine adaptive H.264 profiles from 2000 kbps down to 8 kbps
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

At runtime, the program loads `runtime-config.yaml` from the directory that
contains the executable. For `./build/srt_weak_video`, the effective file is
therefore `build/runtime-config.yaml`.

The repository-root `runtime-config.yaml` is the source template copied into
the build directory when CMake configures the project. Edit the build copy for
an immediate runtime change, or edit the root template and rerun CMake to keep
future build directories consistent. The checked-in configuration is:

```yaml
transport:
  mode: udp
  feedback_interval_ms: 200
  feedback_redundancy: 10
  feedback_timeout_ms: 1000

sender:
  input: file
  connect: 10.88.0.2:9000
  video_file: /home/u20/code/jetson-2k.mp4
  camera_device: /dev/video0
  camera_width: 640
  camera_height: 480
  camera_fps: 30
  max_video_kbps: 2000

receiver:
  listen: 10.88.0.2:9000
  display: true
  output_width: 640
  output_height: 360
  write_h264: true
  output_file: received.h264

latency:
  metric: encode_to_decode
  window_seconds: 10
```

Start the receiver and sender:

```bash
./build/srt_weak_video --role receiver
./build/srt_weak_video --role sender
```

The command line accepts only `--role`. Set `receiver.display: false` for
headless operation. Set `receiver.write_h264: false` to disable creation and
writing of the H.264 output file; writing is enabled by default.
The display window and rendered frames use the fixed size selected by
`receiver.output_width` and `receiver.output_height` (default `640x360`).
Decoded frames at adaptive resolutions are scaled to this size for display;
the decoded data and optional H.264 output file are not modified.

### Transport Mode

`transport.mode` selects the media transport:

- `udp` is the default. It has no handshake, connection state, or reconnect
  delay. The sender immediately transmits FEC-protected datagrams and receives
  redundant quality feedback over the same UDP socket.
- `srt` keeps the previous SRT caller/listener behavior for compatibility and
  comparison. SRT still uses its 350 ms latency budget and ARQ.

UDP cold start and feedback loss enter a recovery profile at
`128x72/1fps/8kbps` with 20x parity. After complete-frame confirmation, 70%
loss uses `320x180/1fps/50kbps` with 8x parity and 80% loss uses
`160x90/1fps/20kbps` with 15x parity. Feedback is emitted every 200 ms, with
10 copies spread across the interval.

### Sender Input

`sender.input` selects the video source:

- `camera` uses a Linux V4L2 camera and is the compiled fallback when the
  configuration file is absent.
- `file` loops the local file configured by `sender.video_file`.

The checked-in namespace-test configuration above currently selects `file`.

Camera mode currently requests YUYV 4:2:2 using `camera_device`,
`camera_width`, `camera_height`, and `camera_fps`. The driver may adjust the
requested camera mode; the negotiated values are printed as
`video_input=camera ...` when the sender starts.

To use the local file instead:

```yaml
sender:
  input: file
```

The remaining sender fields stay unchanged. The sender process must have read
and write permission for the configured `/dev/video*` device.

## Latency Metrics

The executable automatically loads `runtime-config.yaml` from the directory
that contains the executable. If it is missing, the compiled defaults shown
above are used.

Only one latency metric is sampled and printed at a time:

- `encode_to_assemble`: encoder output to complete-frame FEC recovery
- `encode_to_decode`: encoder output to a valid decoded frame
- `source_to_display`: selected source frame to `SDL_RenderPresent`

The receiver prints the average and P95 over the configured rolling window.
`source_to_display` reports `n/a` when `receiver.display` is false. Sender and receiver
system clocks must be synchronized with NTP or PTP because these are one-way
latency measurements. The display metric stops at the SDL software-present
call and does not include monitor scanout or panel response.

## Weak-Network Test

The copied `scripts/netem_loss.sh` creates the same sender/receiver namespace
topology as the WebRTC demo. A process started on the host does not use this
link: binding the receiver to `0.0.0.0` only covers interfaces in the
receiver's current network namespace.

`ns-up` installs permanent neighbor entries between the veth endpoints. This
keeps 70%-80% netem tests focused on IP media and feedback loss; otherwise ARP
requests and replies are also dropped and cold-start timing mostly measures
neighbor discovery luck.

The values below are **namespace-test overrides**, not the default runtime
configuration. Before starting a namespace test, temporarily change these
fields in `build/runtime-config.yaml`:

```yaml
sender:
  connect: 10.88.0.2:9000

receiver:
  listen: 10.88.0.2:9000
  display: false
  output_file: /tmp/srt-received.h264
```

All fields not shown above retain their default values from the earlier
configuration block. Restore `sender.connect`, `receiver.listen`,
`receiver.display`, and `receiver.output_file` after the namespace test if the
next run will use the local host topology.

Create the topology, then start the receiver in `webrtc_rx`:

```bash
sudo ./scripts/netem_loss.sh ns-up
sudo ip netns exec webrtc_rx ./build/srt_weak_video \
  --role receiver
```

Start the sender in another terminal in `webrtc_tx`:

```bash
sudo ip netns exec webrtc_tx ./build/srt_weak_video \
  --role sender
```

After the connection and media flow are established, apply loss in a third
terminal:

```bash
sudo ./scripts/netem_loss.sh ns-loss 50 50 both
sudo ./scripts/netem_loss.sh ns-show
```

Limit the sender and receiver egress bandwidth independently:

```bash
sudo ./scripts/netem_loss.sh ns-bandwidth 500kbit 200kbit
```

To combine bandwidth limits with loss and delay, configure the complete link
in one command:

```bash
sudo ./scripts/netem_loss.sh ns-link 30 50 500kbit 200kbit
```

Here `tx_rate` limits traffic leaving `webrtc_tx`, while `rx_rate` limits
traffic leaving `webrtc_rx`. Rate values use `tc` units such as `kbit`,
`mbit`, or `gbit`.

Use `sudo ./scripts/netem_loss.sh ns-clear both` to remove the impairment and
`sudo ./scripts/netem_loss.sh ns-down` after both processes have stopped.

Validate the recovered elementary stream:

```bash
ffmpeg -v error -err_detect explode \
  -i /tmp/srt-received.h264 -f null -
```

At extreme loss the receiver deliberately freezes until a complete keyframe is
recovered. It never forwards incomplete or CRC-invalid frames to the decoder.
The SDL window and its last decoded frame stay alive. In UDP mode there is no
SRT connection or reconnect state.

The reference validation used `/home/u20/code/jetson-2k.mp4`, 30% loss in both
directions, and 50 ms delay. Complete frames continued throughout the loss
period at a reduced profile, the receiver reported zero decoder errors, and
the saved H.264 stream passed FFmpeg's `-err_detect explode` check.
