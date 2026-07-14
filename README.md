# Weak-Network Video

Native Linux sender/receiver for complete-frame video delivery over severe
packet loss. The transport combines:

- connectionless UDP as the default high-loss media transport
- optional SRT live transport for compatibility and comparison
- per-frame Reed-Solomon erasure coding through Intel ISA-L
- nine adaptive H.264 profiles from 2000 kbps down to 60 kbps
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
  minimum_output_fps: 30
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
default and applies luma contrast stretch plus light luma sharpening only after
an upscale.
`interpolation_mode` selects `repeat` or `blend`; the default `blend` emits one
50/50 transition frame only when the cadence controller has accumulated enough
low-rate display debt. Both modes retain the last complete frame during a
prolonged gap. The 30 fps display default uses bounded supplementation so a
24 fps decoded stream is smoothed with a small number of synthetic frames rather
than replacing most real frames.

Receiver statistics include `decoded_fps`, `output_fps`, `synthetic_fps`, and
`output_resolution` so reports can separate true decoded throughput from the
30 fps display cadence.

### Transport Mode

`transport.mode` selects the media transport:

- `udp` is the default. It has no handshake, connection state, or reconnect
  delay. The sender immediately transmits FEC-protected datagrams and receives
  redundant quality feedback over the same UDP socket.
- `srt` keeps the previous SRT caller/listener behavior for compatibility and
  comparison. SRT still uses its 350 ms latency budget and ARQ.

UDP starts with the normal Level 0 profile (`1280x720/30fps`) and switches to
Level 8 at `320x180/24fps/220kbps` only after stale or missing feedback, send
congestion, a receiver frame age above 1500 ms, or very high reported loss. A
recovery frame keeps Level 8 until it is acknowledged, then the sender
immediately selects the level implied by the fresh feedback. UDP and SRT use the
same nine-level ladder after recovery. Low transport resolutions are allowed
below the display minimum; the receiver upscales complete decoded frames to at
least `640x360` and fills the display cadence to `30fps` by default. The sender
profiles are quality/fps balanced: low and mid levels stay at the current 30fps
source ceiling, while 70-80% loss levels keep `320x180` transport detail and
accept `24-28fps` true send cadence rather than dropping to very blurry
`160x90` transport.

| Level | H.264 profile | GOP frames | min data shards | P-frame FEC | keyframe FEC | all-intra |
|---:|---|---:|---:|---:|---:|---|
| 0 | 1280x720 / 30 fps / 2000 kbps | 30 | 4 | 0.10 | 0.30 | no |
| 1 | 640x360 / 30 fps / 900 kbps | 10 | 4 | 0.50 | 0.75 | no |
| 2 | 640x360 / 30 fps / 700 kbps | 8 | 4 | 0.75 | 1.00 | no |
| 3 | 512x288 / 30 fps / 500 kbps | 6 | 3 | 1.00 | 2.00 | no |
| 4 | 320x180 / 30 fps / 350 kbps | 1 | 1 | 3.00 | 4.00 | yes |
| 5 | 320x180 / 30 fps / 300 kbps | 1 | 1 | 5.00 | 6.00 | yes |
| 6 | 320x180 / 28 fps / 260 kbps | 1 | 1 | 7.00 | 8.00 | yes |
| 7 | 320x180 / 26 fps / 240 kbps | 1 | 1 | 10.00 | 10.00 | yes |
| 8 | 320x180 / 24 fps / 220 kbps | 1 | 1 | 12.00 | 12.00 | yes |

Raw loss thresholds above 5, 15, 20, 30, 50, 65, 72, and 77 percent map to
Levels 1 through 8. Effective selection adds hysteresis: Level 2 requires three
consecutive feedback windows above 15 percent unless loss exceeds 20 percent,
and Level 5 requires three consecutive windows above 55 percent unless loss
exceeds 60 percent. Recovery advances one level after five stable feedback
windows, with hysteresis below 12 percent for Level 2 to Level 1, below 3
percent for Level 1 to Level 0, and below 50 percent for Level 5 to Level 4.
RTT-only degradation starts above 160 ms and requires five consecutive high-RTT
feedback windows.

Levels 3 through 8 use stronger FEC, lower transport resolution, smaller
per-block data shard counts, and short GOP or all-intra coding to improve true
decoded-frame throughput under high loss. At 80% bidirectional loss the target
is no longer 8-12 true decoded fps; the acceptance floor is 20 true decoded fps,
with 30fps as the source-limited ceiling when the picture can be very blurry.

### Sender Input

`sender.input` selects the video source:

- `camera` uses a Linux V4L2 camera and is the compiled fallback when the
  configuration file is absent.
- `file` loops the local file configured by `sender.video_file`.

The checked-in namespace-test configuration above currently selects `file`.
File input is paced against the source frame deadline: decode, scale, x264,
FEC, and socket-send time are subtracted from the following sleep. If a frame
is already late, the sender does not burst-send catch-up frames; it restarts the
next deadline from the current time.

Camera mode currently requests YUYV 4:2:2 using `camera_device`,
`camera_width`, `camera_height`, and `camera_fps`. The driver may adjust the
requested camera mode; the negotiated values are printed as
`video_input=camera ...` when the sender starts. Camera pacing comes from the
V4L2 capture cadence; the sender does not add the file-mode post-send sleep.

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

The previous 5 fps display reference validation used
`/home/u20/code/jetson-2k.mp4` and 50 ms delay with 70%, 80%, and 85% loss in
both directions. Each scenario ran for 600 seconds. Maximum complete-frame gaps
were 1595, 2104, and 2103 ms; the display produced 2998 frames in each run at
`640x360`, decoder errors remained zero, and every saved H.264 stream passed
FFmpeg's `-err_detect explode` check. The current 20 fps display acceptance
path is `scripts/weaknet_acceptance.py`; it reports true decoded fps separately
from generated display cadence.

## Automated Acceptance

`scripts/weaknet_acceptance.py` runs the weak-network acceptance matrix and
collects the evidence needed for TC-01 through TC-09. It creates the namespace
topology, starts the receiver and sender inside `webrtc_rx` and `webrtc_tx`,
applies `tc netem` loss, records logs, validates the saved H.264 stream with
FFmpeg, and writes JSON plus Markdown reports.

Run the quick regression suite:

```bash
python3 scripts/weaknet_acceptance.py --suite quick
```

The quick suite runs 0%, 20%, 30%, 45%, 50%, 65%, 70%, and 80% bidirectional
loss with 50 ms delay for 60 seconds per steady scenario, followed by a short
staircase from 0% to 10%, 30%, 50%, 65%, 70%, 80%, then back down through 50%,
10%, and 0%. For a shorter smoke run, override the durations:

```bash
python3 scripts/weaknet_acceptance.py \
  --suite quick \
  --duration-seconds 15 \
  --step-duration-seconds 10
```

The full suite is intended for formal acceptance:

```bash
python3 scripts/weaknet_acceptance.py --suite full
```

It runs 0%, 5%, 15%, 20%, 30%, 35%, 45%, 50%, 60%, 65%, 70%, 80%, and 85%
bidirectional loss for 600 seconds per steady scenario, then runs the full
staircase up to 85% and gradually back down to 0%. The 90% case is separated as
an extreme observation and does not fail the main
acceptance suite:

```bash
python3 scripts/weaknet_acceptance.py --suite extreme
```

Results are written under `/tmp/weaknet-acceptance-YYYYmmdd-HHMMSS` unless
`--output-dir` is provided. Each scenario directory contains:

- `sender.log` and `receiver.log` with `elapsed=` timestamps added by the
  runner.
- `received.h264`, `ffmpeg_check.log`, and `ffprobe.log`.
- `qdisc_step_N.txt` snapshots for each applied impairment.
- `summary.json` and `report.md`.

The suite-level `summary.json` and `report.md` summarize pass/fail status. The
current acceptance scope treats TC-01 as a video-profile bitrate check only:
FEC and UDP overhead are not counted against the 2 Mbps ceiling. TC-06 is not
part of the automated acceptance result. TC-07 checks both fast downshift on
increasing loss and profile recovery on decreasing loss, using a 3 second
downshift window and a 15 second recovery window. TC-08 uses
`latency_avg <= 500 ms` as the hard criterion; values below 180 ms are reported
as better-than-target latency rather than failures. TC-09 checks the 80% loss
target after warmup: average `decoded_fps >= 20`, average `output_fps >= 28`,
average `synthetic_fps <= 10`, and `output_resolution == 640x360`.
The report also includes `synthetic_fps`,
post-warmup fps minimums, `latency_p95`, and `latency_max` for diagnosis.
