# Weak-Network Video

Native Linux sender/receiver for complete-frame video delivery over severe
packet loss. The transport combines:

- connectionless UDP as the default high-loss media transport
- optional SRT live transport for compatibility and comparison
- per-frame Reed-Solomon erasure coding through Intel ISA-L
- nine adaptive H.264 profiles from 2000 kbps down to 30 kbps
- CRC validation and strict complete-frame/keyframe gating
- CPU display upscaling and frame synthesis with a minimum output specification
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
  minimum_output_width: 640
  minimum_output_height: 360
  minimum_output_fps: 5
  upscale_mode: lanczos_sharpen
  interpolation_mode: blend
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
The display uses a minimum output specification rather than a fixed size.
Frames already at or above the configured minimum width and height keep their
decoded resolution. Smaller frames are scaled up without changing aspect
ratio. When complete decoded frames arrive slower than the minimum output
frame rate, the display generates CPU-processed frames at that cadence. The
decoded data and optional H.264 output file are not modified.

`upscale_mode` selects `bilinear` or `lanczos_sharpen`; the latter is the
default and applies light luma sharpening only after an upscale.
`interpolation_mode` selects `repeat` or `blend`; the default `blend` emits one
50/50 transition frame before displaying a newly received low-rate frame.
Both modes retain the last complete frame during a prolonged gap.

Receiver statistics keep `decoded_fps` for input-rate diagnosis. Generated
output rate, synthetic-frame rate, output resolution, and the configured
latency metric name are not repeated in every log line.

### Transport Mode

`transport.mode` selects the media transport:

- `udp` is the default. It has no handshake, connection state, or reconnect
  delay. The sender immediately transmits FEC-protected datagrams and receives
  redundant quality feedback over the same UDP socket.
- `srt` keeps the previous SRT caller/listener behavior for compatibility and
  comparison. SRT still uses its 350 ms latency budget and ARQ.

UDP starts with the normal Level 0 profile (`1280x720/30fps`) and switches to
Level 8 at `256x144/2fps/30kbps` only after stale or missing feedback, send
congestion, a receiver frame age above 1500 ms, or very high reported loss. A
recovery frame keeps Level 8 until it is acknowledged, then the sender
immediately selects the level implied by the fresh feedback. UDP and SRT use the
same nine-level ladder after recovery. Levels 1 and 2 use about a half-second
GOP to recover H.264 synchronization faster after a lost P-frame; the remaining
levels use about a one-second GOP. Feedback is emitted every 200 ms, with 10
copies spread across the interval.

| Level | H.264 profile | GOP frames | P-frame FEC | keyframe FEC |
|---:|---|---:|---:|---:|
| 0 | 1280x720 / 30 fps / 2000 kbps | 30 | 0.10 | 0.30 |
| 1 | 640x360 / 20 fps / 900 kbps | 10 | 0.50 | 0.75 |
| 2 | 640x360 / 15 fps / 700 kbps | 8 | 0.50 | 0.75 |
| 3 | 640x360 / 10 fps / 400 kbps | 10 | 0.40 | 1.00 |
| 4 | 426x240 / 5 fps / 220 kbps | 5 | 0.75 | 2.00 |
| 5 | 426x240 / 3 fps / 140 kbps | 3 | 1.25 | 3.00 |
| 6 | 320x180 / 3 fps / 80 kbps | 3 | 2.50 | 5.00 |
| 7 | 320x180 / 2 fps / 50 kbps | 2 | 4.00 | 7.00 |
| 8 | 256x144 / 2 fps / 30 kbps | 2 | 8.00 | 12.00 |

Loss thresholds above 5, 15, 20, 30, 50, 65, 72, and 77 percent select Levels
1 through 8. Degradation is immediate; recovery advances one level after five
fresh healthy or emergency-recovery feedback windows.

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
keeps 70%-85% netem tests focused on IP media and feedback loss; otherwise ARP
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

The current reference validation used `/home/u20/code/jetson-2k.mp4` and
50 ms delay with 70%, 80%, and 85% loss in both directions. Each scenario ran
for 600 seconds. Maximum complete-frame gaps were 1595, 2104, and 2103 ms;
the display produced 2998 frames in each run at `640x360`, decoder errors
remained zero, and every saved H.264 stream passed FFmpeg's
`-err_detect explode` check. Five repeated transitions from an unimpaired link
to 80% and 85% loss also stayed below three seconds, with worst gaps of 2189
and 2236 ms.
