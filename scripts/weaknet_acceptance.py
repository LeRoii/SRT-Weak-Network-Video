#!/usr/bin/env python3
"""Automated weak-network acceptance runner.

The runner intentionally uses only the Python standard library so it can run on
the same machines that already build this C++ project.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


PROFILES = [
    (0, 2000, 30, 1280, 720),
    (1, 900, 24, 640, 360),
    (2, 700, 24, 640, 360),
    (3, 500, 20, 640, 360),
    (4, 350, 20, 426, 240),
    (5, 220, 15, 320, 180),
    (6, 150, 12, 320, 180),
    (7, 90, 12, 256, 144),
    (8, 60, 10, 160, 90),
]

PROFILE_RE = re.compile(
    r"profile=(?P<bitrate>\d+)kbps/(?P<fps>\d+)fps/"
    r"(?P<width>\d+)x(?P<height>\d+)"
)
PROFILE_CHANGE_RE = re.compile(
    r"profile_change=1 .* old=(?P<old_bitrate>\d+)kbps/"
    r"(?P<old_fps>\d+)fps/(?P<old_width>\d+)x(?P<old_height>\d+)"
    r" new=(?P<new_bitrate>\d+)kbps/(?P<new_fps>\d+)fps/"
    r"(?P<new_width>\d+)x(?P<new_height>\d+)"
)
ELAPSED_RE = re.compile(r"^\[elapsed=(?P<elapsed>[0-9.]+)\] ")
METRIC_RE = re.compile(r"(?P<name>[a-zA-Z0-9_]+)=(?P<value>[^ ]+)")
DOWNGRADE_WINDOW_SECONDS = 3.0
UPGRADE_WINDOW_SECONDS = 15.0


@dataclass(frozen=True)
class Step:
    loss_percent: int
    delay_ms: int
    duration_seconds: int


@dataclass(frozen=True)
class Scenario:
    name: str
    kind: str
    steps: tuple[Step, ...]
    extreme: bool = False


class RunningProcess:
    def __init__(self, name: str, process: subprocess.Popen[str],
                 log_path: Path, started_at: float) -> None:
        self.name = name
        self.process = process
        self.log_path = log_path
        self.started_at = started_at
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self) -> None:
        assert self.process.stdout is not None
        with self.log_path.open("w", encoding="utf-8") as output:
            for line in self.process.stdout:
                elapsed = time.monotonic() - self.started_at
                output.write(f"[elapsed={elapsed:.3f}] {line}")
                output.flush()

    def stop(self, grace_seconds: float = 8.0) -> int:
        if self.process.poll() is None:
            self._signal_group(signal.SIGINT)
            try:
                self.process.wait(timeout=grace_seconds)
            except subprocess.TimeoutExpired:
                self._signal_group(signal.SIGTERM)
                try:
                    self.process.wait(timeout=3.0)
                except subprocess.TimeoutExpired:
                    self._signal_group(signal.SIGKILL)
                    self.process.wait(timeout=3.0)
        self._thread.join(timeout=2.0)
        return self.process.returncode

    def _signal_group(self, sig: int) -> None:
        try:
            os.killpg(self.process.pid, sig)
        except ProcessLookupError:
            pass


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run_command(command: list[str], cwd: Path, *, check: bool = True,
                stdout_path: Path | None = None) -> subprocess.CompletedProcess[str]:
    if stdout_path:
        with stdout_path.open("w", encoding="utf-8") as output:
            result = subprocess.run(
                command,
                cwd=cwd,
                text=True,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=False,
            )
    else:
        result = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    if check and result.returncode != 0:
        detail = "" if stdout_path else f"\n{result.stdout}"
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}{detail}"
        )
    return result


def ns_command(root: Path, *args: str) -> list[str]:
    return ["sudo", str(root / "scripts" / "netem_loss.sh"), *args]


def write_runtime_config(root: Path, output_file: Path) -> Path:
    config_path = root / "build" / "runtime-config.yaml"
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(
        "\n".join(
            [
                "transport:",
                "  mode: udp",
                "  feedback_interval_ms: 200",
                "  feedback_redundancy: 10",
                "  feedback_timeout_ms: 1000",
                "",
                "sender:",
                "  input: file",
                "  connect: 10.88.0.2:9000",
                "  video_file: /home/u20/code/jetson-2k.mp4",
                "  camera_device: /dev/video0",
                "  camera_width: 640",
                "  camera_height: 480",
                "  camera_fps: 30",
                "  max_video_kbps: 2000",
                "",
                "receiver:",
                "  listen: 10.88.0.2:9000",
                "  display: true",
                "  minimum_output_width: 640",
                "  minimum_output_height: 360",
                "  minimum_output_fps: 20",
                "  upscale_mode: lanczos_sharpen",
                "  interpolation_mode: blend",
                "  write_h264: true",
                f"  output_file: {output_file}",
                "",
                "latency:",
                "  metric: encode_to_decode",
                "  window_seconds: 10",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return config_path


def preserve_runtime_config(root: Path) -> tuple[Path, bytes | None]:
    config_path = root / "build" / "runtime-config.yaml"
    return config_path, config_path.read_bytes() if config_path.exists() else None


def restore_runtime_config(config_path: Path, original: bytes | None) -> None:
    if original is None:
        try:
            config_path.unlink()
        except FileNotFoundError:
            pass
    else:
        config_path.write_bytes(original)


def start_process(root: Path, namespace: str, role: str,
                  log_path: Path, started_at: float) -> RunningProcess:
    executable = root / "build" / "srt_weak_video"
    command = [
        "sudo", "ip", "netns", "exec", namespace,
        "env", "SDL_VIDEODRIVER=dummy",
        str(executable), "--role", role,
    ]
    process = subprocess.Popen(
        command,
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,
        preexec_fn=os.setsid,
    )
    return RunningProcess(role, process, log_path, started_at)


def scenarios_for_suite(suite: str, duration_seconds: int | None,
                        step_duration_seconds: int | None) -> list[Scenario]:
    if suite == "quick":
        steady_duration = duration_seconds or 60
        step_duration = step_duration_seconds or 30
        losses = [0, 20, 45, 70, 80]
        staircase = [0, 10, 50, 70, 80, 50, 10, 0]
    elif suite == "full":
        steady_duration = duration_seconds or 600
        step_duration = step_duration_seconds or 60
        losses = [0, 5, 15, 20, 35, 45, 60, 70, 80, 85]
        staircase = [
            0, 5, 10, 15, 20, 35, 45, 60, 70, 80, 85,
            80, 70, 60, 45, 35, 20, 10, 5, 0,
        ]
    else:
        steady_duration = duration_seconds or 600
        step_duration = step_duration_seconds or steady_duration
        return [
            Scenario(
                name="extreme_loss_90",
                kind="steady",
                steps=(Step(90, 50, steady_duration),),
                extreme=True,
            )
        ]

    result = [
        Scenario(
            name=f"loss_{loss:02d}",
            kind="steady",
            steps=(Step(loss, 50, steady_duration),),
        )
        for loss in losses
    ]
    result.append(
        Scenario(
            name="staircase",
            kind="staircase",
            steps=tuple(Step(loss, 50, step_duration) for loss in staircase),
        )
    )
    return result


def parse_elapsed(line: str) -> float | None:
    match = ELAPSED_RE.search(line)
    return float(match.group("elapsed")) if match else None


def parse_value_ms(value: str) -> float | None:
    if value == "n/a":
        return None
    if value.endswith("ms"):
        value = value[:-2]
    try:
        return float(value)
    except ValueError:
        return None


def parse_log_metrics(path: Path) -> dict[str, object]:
    profiles: list[dict[str, object]] = []
    profile_changes: list[dict[str, object]] = []
    last_metrics: dict[str, str] = {}
    metric_samples: list[dict[str, object]] = []

    if not path.exists():
        return {
            "profiles": profiles,
            "profile_changes": profile_changes,
            "last_metrics": last_metrics,
        }

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        elapsed = parse_elapsed(line)
        profile_match = PROFILE_RE.search(line)
        if profile_match:
            profiles.append(
                {
                    "elapsed": elapsed,
                    "bitrate_kbps": int(profile_match.group("bitrate")),
                    "fps": int(profile_match.group("fps")),
                    "width": int(profile_match.group("width")),
                    "height": int(profile_match.group("height")),
                }
            )
        change_match = PROFILE_CHANGE_RE.search(line)
        if change_match:
            profile_changes.append(
                {
                    "elapsed": elapsed,
                    "old_bitrate_kbps": int(change_match.group("old_bitrate")),
                    "new_bitrate_kbps": int(change_match.group("new_bitrate")),
                    "old_size": (
                        f"{change_match.group('old_width')}x"
                        f"{change_match.group('old_height')}"
                    ),
                    "new_size": (
                        f"{change_match.group('new_width')}x"
                        f"{change_match.group('new_height')}"
                    ),
                }
            )
        if "frames=" in line or "decoder_errors=" in line:
            metrics: dict[str, object] = {
                match.group("name"): match.group("value")
                for match in METRIC_RE.finditer(line)
            }
            if elapsed is not None:
                metrics["elapsed"] = elapsed
            last_metrics = {
                str(key): str(value)
                for key, value in metrics.items()
                if key != "elapsed"
            }
            metric_samples.append(metrics)

    return {
        "profiles": profiles,
        "profile_changes": profile_changes,
        "last_metrics": last_metrics,
        "metric_samples": metric_samples,
    }


def metric_average(samples: list[dict[str, object]], name: str,
                   fallback: float) -> float:
    values: list[float] = []
    for sample in samples:
        value = sample.get(name)
        if value is None:
            continue
        try:
            values.append(float(str(value)))
        except ValueError:
            continue
    return sum(values) / len(values) if values else fallback


def metric_minimum(samples: list[dict[str, object]], name: str,
                   fallback: float) -> float:
    values: list[float] = []
    for sample in samples:
        value = sample.get(name)
        if value is None:
            continue
        try:
            values.append(float(str(value)))
        except ValueError:
            continue
    return min(values) if values else fallback


def samples_at_or_after(samples: list[dict[str, object]],
                        elapsed: float) -> list[dict[str, object]]:
    selected: list[dict[str, object]] = []
    for sample in samples:
        sample_elapsed = sample.get("elapsed")
        if sample_elapsed is None:
            continue
        if float(sample_elapsed) >= elapsed:
            selected.append(sample)
    return selected


def latest_metric(samples: list[dict[str, object]], name: str,
                  fallback: str) -> str:
    for sample in reversed(samples):
        value = sample.get(name)
        if value is not None:
            return str(value)
    return fallback


def format_report_value(value: object) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.2f}".rstrip("0").rstrip(".")
    return str(value)


def required_bitrate_for_loss(loss_percent: int) -> int:
    required_level = 0
    for threshold, level in [
        (5, 1), (15, 2), (20, 3), (30, 4),
        (50, 5), (65, 6), (72, 7), (77, 8),
    ]:
        if loss_percent > threshold:
            required_level = level
    return PROFILES[required_level][1]


def downgrade_after(
    profile_changes: list[dict[str, object]],
    applied_at: float,
    window_seconds: float,
) -> bool:
    return any(
        change["elapsed"] is not None and
        0 <= float(change["elapsed"]) - applied_at <= window_seconds and
        int(change["new_bitrate_kbps"]) < int(change["old_bitrate_kbps"])
        for change in profile_changes
    )


def upgrade_after(
    profile_changes: list[dict[str, object]],
    applied_at: float,
    window_seconds: float,
) -> bool:
    return any(
        change["elapsed"] is not None and
        0 <= float(change["elapsed"]) - applied_at <= window_seconds and
        int(change["new_bitrate_kbps"]) > int(change["old_bitrate_kbps"])
        for change in profile_changes
    )


def ffmpeg_check(root: Path, h264_path: Path, log_path: Path) -> dict[str, object]:
    if not h264_path.exists() or h264_path.stat().st_size == 0:
        log_path.write_text("missing or empty H.264 output\n", encoding="utf-8")
        return {"passed": False, "returncode": None, "bytes": 0}
    command = [
        "ffmpeg", "-v", "error", "-err_detect", "explode",
        "-i", str(h264_path), "-f", "null", "-",
    ]
    result = run_command(command, root, check=False, stdout_path=log_path)
    return {
        "passed": result.returncode == 0,
        "returncode": result.returncode,
        "bytes": h264_path.stat().st_size,
    }


def ffprobe_count(root: Path, h264_path: Path, log_path: Path) -> dict[str, object]:
    if not h264_path.exists() or h264_path.stat().st_size == 0:
        return {"available": False}
    command = [
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-count_frames", "-show_entries",
        "stream=width,height,nb_read_frames,bit_rate",
        "-of", "default=noprint_wrappers=1", str(h264_path),
    ]
    result = run_command(command, root, check=False, stdout_path=log_path)
    parsed: dict[str, object] = {
        "available": result.returncode == 0,
        "returncode": result.returncode,
    }
    if log_path.exists():
        for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                parsed[key] = value
    return parsed


def summarize_scenario(scenario: Scenario, sender_log: Path,
                       receiver_log: Path, h264_path: Path,
                       ffmpeg_result: dict[str, object],
                       ffprobe_result: dict[str, object],
                       step_events: list[dict[str, object]]) -> dict[str, object]:
    sender = parse_log_metrics(sender_log)
    receiver = parse_log_metrics(receiver_log)
    profiles = sender["profiles"]
    profile_changes = sender["profile_changes"]
    receiver_metrics = receiver["last_metrics"]
    receiver_samples = receiver["metric_samples"]

    bitrates = [int(profile["bitrate_kbps"]) for profile in profiles]
    max_bitrate = max(bitrates) if bitrates else None
    average_bitrate = sum(bitrates) / len(bitrates) if bitrates else None
    decoder_errors = int(receiver_metrics.get("decoder_errors", "999999"))
    frames = int(receiver_metrics.get("frames", "0"))
    first_loss = scenario.steps[0].loss_percent
    stable_samples = receiver_samples
    warmup_start_elapsed = None
    if scenario.kind == "steady":
        applied_at = float(step_events[0]["elapsed"]) if step_events else 0.0
        warmup_seconds = min(
            5.0,
            max(0.0, scenario.steps[0].duration_seconds * 0.25),
        )
        warmup_start_elapsed = applied_at + warmup_seconds
        post_warmup = samples_at_or_after(
            receiver_samples,
            warmup_start_elapsed,
        )
        if post_warmup:
            stable_samples = post_warmup

    last_decoded_fps = float(receiver_metrics.get("decoded_fps", "0"))
    last_output_fps = float(receiver_metrics.get("output_fps", "0"))
    last_synthetic_fps = float(receiver_metrics.get("synthetic_fps", "0"))
    decoded_fps = metric_average(
        stable_samples, "decoded_fps", last_decoded_fps)
    output_fps = metric_average(
        stable_samples, "output_fps", last_output_fps)
    synthetic_fps = metric_average(
        stable_samples, "synthetic_fps", last_synthetic_fps)
    decoded_fps_min = metric_minimum(
        stable_samples, "decoded_fps", last_decoded_fps)
    output_fps_min = metric_minimum(
        stable_samples, "output_fps", last_output_fps)
    output_resolution = latest_metric(
        stable_samples,
        "output_resolution",
        receiver_metrics.get("output_resolution", "0x0"),
    )
    max_frame_gap_ms = int(receiver_metrics.get("max_frame_gap_ms", "0"))
    latency_avg_ms = parse_value_ms(receiver_metrics.get("latency_avg", "n/a"))
    latency_p95_ms = parse_value_ms(receiver_metrics.get("latency_p95", "n/a"))
    latency_max_ms = parse_value_ms(receiver_metrics.get("latency_max", "n/a"))
    latency_samples = int(receiver_metrics.get("latency_samples", "0"))

    tc01 = True
    if scenario.kind == "steady" and first_loss == 0:
        tc01 = (
            max_bitrate is not None and max_bitrate <= 2000 and
            average_bitrate is not None and 1500 <= average_bitrate <= 2000
        )

    hard_loss_scenario = (
        scenario.kind == "steady" and
        5 <= first_loss <= 85 and
        not scenario.extreme
    )
    tc02_tc03 = True
    if hard_loss_scenario:
        tc02_tc03 = (
            frames > 0 and decoder_errors == 0 and bool(ffmpeg_result["passed"])
        )

    tc04 = True
    if scenario.kind == "steady" and first_loss >= 20 and not scenario.extreme:
        applied_at = float(step_events[0]["elapsed"])
        tc04 = downgrade_after(profile_changes, applied_at, 2.5)

    tc05 = True if scenario.extreme else bool(ffmpeg_result["passed"])
    tc08 = latency_avg_ms is not None and latency_avg_ms <= 500.0
    tc09 = True
    if scenario.kind == "steady" and first_loss == 80 and not scenario.extreme:
        tc09 = (
            decoded_fps >= 8 and
            output_fps >= 20 and
            output_resolution == "640x360"
        )

    tc07 = True
    if scenario.kind == "staircase":
        increasing_loss_step_indexes = [
            index
            for index in range(1, len(scenario.steps))
            if scenario.steps[index].loss_percent >
            scenario.steps[index - 1].loss_percent and
            scenario.steps[index].loss_percent >= 20
        ]
        decreasing_loss_step_indexes = [
            index
            for index in range(1, len(scenario.steps))
            if scenario.steps[index].loss_percent <
            scenario.steps[index - 1].loss_percent and
            scenario.steps[index - 1].loss_percent >= 20
        ]
        tc07 = (
            frames > 0 and decoder_errors == 0 and
            bool(ffmpeg_result["passed"]) and
            all(
                downgrade_after(
                    profile_changes,
                    float(step_events[index]["elapsed"]),
                    DOWNGRADE_WINDOW_SECONDS,
                )
                for index in increasing_loss_step_indexes
            ) and
            all(
                upgrade_after(
                    profile_changes,
                    float(step_events[index]["elapsed"]),
                    UPGRADE_WINDOW_SECONDS,
                )
                for index in decreasing_loss_step_indexes
            )
        )

    checks = {
        "TC-01": {"passed": tc01, "applies": scenario.kind == "steady" and first_loss == 0},
        "TC-02/TC-03": {"passed": tc02_tc03, "applies": hard_loss_scenario},
        "TC-04": {"passed": tc04, "applies": scenario.kind == "steady" and first_loss >= 20 and not scenario.extreme},
        "TC-05": {"passed": tc05, "applies": not scenario.extreme},
        "TC-07": {"passed": tc07, "applies": scenario.kind == "staircase"},
        "TC-08": {"passed": tc08, "applies": not scenario.extreme},
        "TC-09": {"passed": tc09, "applies": scenario.kind == "steady" and first_loss == 80 and not scenario.extreme},
    }
    passed = all(
        check["passed"] for check in checks.values() if check["applies"]
    )

    return {
        "name": scenario.name,
        "kind": scenario.kind,
        "extreme_observation": scenario.extreme,
        "passed": passed if not scenario.extreme else None,
        "steps": [step.__dict__ for step in scenario.steps],
        "step_events": step_events,
        "metrics": {
            "max_profile_bitrate_kbps": max_bitrate,
            "average_profile_bitrate_kbps": average_bitrate,
            "profile_count": len(profiles),
            "profile_changes": profile_changes,
            "decoder_errors": decoder_errors,
            "frames": frames,
            "decoded_fps": decoded_fps,
            "output_fps": output_fps,
            "synthetic_fps": synthetic_fps,
            "decoded_fps_min": decoded_fps_min,
            "output_fps_min": output_fps_min,
            "fps_warmup_start_elapsed": warmup_start_elapsed,
            "output_resolution": output_resolution,
            "max_frame_gap_ms": max_frame_gap_ms,
            "latency_avg_ms": latency_avg_ms,
            "latency_p95_ms": latency_p95_ms,
            "latency_max_ms": latency_max_ms,
            "latency_samples": latency_samples,
            "ffprobe": ffprobe_result,
        },
        "ffmpeg": ffmpeg_result,
        "checks": checks,
    }


def write_markdown_report(path: Path, summary: dict[str, object]) -> None:
    checks = summary["checks"]
    metrics = summary["metrics"]
    lines = [
        f"# {summary['name']}",
        "",
        f"- kind: `{summary['kind']}`",
        f"- passed: `{summary['passed']}`",
        f"- extreme observation: `{summary['extreme_observation']}`",
        "",
        "## Checks",
        "",
        "| Check | Applies | Passed |",
        "| --- | ---: | ---: |",
    ]
    for name, check in checks.items():
        lines.append(
            f"| {name} | {check['applies']} | {check['passed']} |"
        )
    lines.extend([
        "",
        "## Metrics",
        "",
        f"- max profile bitrate: `{metrics['max_profile_bitrate_kbps']}` kbps",
        f"- average profile bitrate: `{metrics['average_profile_bitrate_kbps']}` kbps",
        f"- frames: `{metrics['frames']}`",
        "- post-warmup decoded/output/synthetic fps: "
        f"`{format_report_value(metrics['decoded_fps'])}` / "
        f"`{format_report_value(metrics['output_fps'])}` / "
        f"`{format_report_value(metrics['synthetic_fps'])}`",
        "- post-warmup decoded/output fps minimum: "
        f"`{format_report_value(metrics['decoded_fps_min'])}` / "
        f"`{format_report_value(metrics['output_fps_min'])}`",
        "- fps warmup start elapsed: "
        f"`{format_report_value(metrics['fps_warmup_start_elapsed'])}`",
        f"- output resolution: `{metrics['output_resolution']}`",
        f"- decoder errors: `{metrics['decoder_errors']}`",
        f"- max frame gap: `{metrics['max_frame_gap_ms']}` ms",
        f"- latency avg/p95/max: `{metrics['latency_avg_ms']}` / `{metrics['latency_p95_ms']}` / `{metrics['latency_max_ms']}` ms",
        f"- latency samples: `{metrics['latency_samples']}`",
        f"- ffmpeg passed: `{summary['ffmpeg']['passed']}`",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def apply_step(root: Path, step: Step, qdisc_path: Path) -> None:
    run_command(
        ns_command(root, "ns-loss", str(step.loss_percent), str(step.delay_ms), "both"),
        root,
    )
    run_command(ns_command(root, "ns-show"), root, stdout_path=qdisc_path)


def run_scenario(root: Path, output_root: Path, scenario: Scenario,
                 startup_seconds: int) -> dict[str, object]:
    scenario_dir = output_root / scenario.name
    scenario_dir.mkdir(parents=True, exist_ok=True)
    h264_path = scenario_dir / "received.h264"
    write_runtime_config(root, h264_path)

    run_command(ns_command(root, "ns-clear", "both"), root, check=False)
    started_at = time.monotonic()
    receiver = start_process(root, "webrtc_rx", "receiver",
                             scenario_dir / "receiver.log", started_at)
    time.sleep(1.0)
    sender = start_process(root, "webrtc_tx", "sender",
                           scenario_dir / "sender.log", started_at)

    step_events: list[dict[str, object]] = []
    try:
        time.sleep(startup_seconds)
        for index, step in enumerate(scenario.steps):
            elapsed = time.monotonic() - started_at
            qdisc_path = scenario_dir / f"qdisc_step_{index}.txt"
            apply_step(root, step, qdisc_path)
            step_events.append({
                "index": index,
                "elapsed": elapsed,
                "loss_percent": step.loss_percent,
                "delay_ms": step.delay_ms,
                "duration_seconds": step.duration_seconds,
            })
            time.sleep(step.duration_seconds)
    finally:
        sender_returncode = sender.stop()
        receiver_returncode = receiver.stop()
        run_command(ns_command(root, "ns-clear", "both"), root, check=False)

    ffmpeg_result = ffmpeg_check(
        root, h264_path, scenario_dir / "ffmpeg_check.log"
    )
    ffprobe_result = ffprobe_count(
        root, h264_path, scenario_dir / "ffprobe.log"
    )
    summary = summarize_scenario(
        scenario,
        scenario_dir / "sender.log",
        scenario_dir / "receiver.log",
        h264_path,
        ffmpeg_result,
        ffprobe_result,
        step_events,
    )
    summary["processes"] = {
        "sender_returncode": sender_returncode,
        "receiver_returncode": receiver_returncode,
    }
    (scenario_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    write_markdown_report(scenario_dir / "report.md", summary)
    return summary


def write_suite_report(output_root: Path, summaries: list[dict[str, object]]) -> None:
    lines = [
        "# Weak-Network Acceptance Report",
        "",
        "| Scenario | Passed | Extreme | Frames | Decoded FPS | Output FPS | Output Resolution | Decoder Errors | Max Gap (ms) | FFmpeg |",
        "| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: |",
    ]
    for summary in summaries:
        metrics = summary["metrics"]
        lines.append(
            f"| {summary['name']} | {summary['passed']} | "
            f"{summary['extreme_observation']} | {metrics['frames']} | "
            f"{format_report_value(metrics['decoded_fps'])} | "
            f"{format_report_value(metrics['output_fps'])} | "
            f"{metrics['output_resolution']} | "
            f"{metrics['decoder_errors']} | {metrics['max_frame_gap_ms']} | "
            f"{summary['ffmpeg']['passed']} |"
        )
    output_root.joinpath("report.md").write_text(
        "\n".join(lines) + "\n",
        encoding="utf-8",
    )
    output_root.joinpath("summary.json").write_text(
        json.dumps({"scenarios": summaries}, indent=2, sort_keys=True),
        encoding="utf-8",
    )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run automated weak-network acceptance scenarios."
    )
    parser.add_argument("--suite", choices=["quick", "full", "extreme"],
                        default="quick")
    parser.add_argument("--duration-seconds", type=int)
    parser.add_argument("--step-duration-seconds", type=int)
    parser.add_argument("--startup-seconds", type=int, default=5)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    return parser


def main(argv: Iterable[str]) -> int:
    args = build_arg_parser().parse_args(list(argv))
    root = repo_root()
    executable = root / "build" / "srt_weak_video"
    if not executable.exists():
        raise RuntimeError("build/srt_weak_video does not exist; build first")
    if not args.skip_build:
        run_command(["cmake", "--build", "build", "-j2"], root)

    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_root = args.output_dir or Path("/tmp") / f"weaknet-acceptance-{timestamp}"
    output_root.mkdir(parents=True, exist_ok=True)

    config_path, original_config = preserve_runtime_config(root)
    summaries: list[dict[str, object]] = []
    try:
        run_command(ns_command(root, "ns-down"), root, check=False)
        run_command(ns_command(root, "ns-up"), root)
        for scenario in scenarios_for_suite(
            args.suite, args.duration_seconds, args.step_duration_seconds
        ):
            summaries.append(
                run_scenario(root, output_root, scenario, args.startup_seconds)
            )
    finally:
        run_command(ns_command(root, "ns-down"), root, check=False)
        restore_runtime_config(config_path, original_config)

    write_suite_report(output_root, summaries)
    print(f"acceptance_output={output_root}")
    failed = [
        summary["name"] for summary in summaries
        if summary["passed"] is False
    ]
    if failed:
        print("failed_scenarios=" + ",".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        raise SystemExit(130)
