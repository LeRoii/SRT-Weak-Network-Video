#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "weaknet_acceptance.py"
SPEC = importlib.util.spec_from_file_location(
    "weaknet_acceptance", MODULE_PATH
)
assert SPEC and SPEC.loader
weaknet = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = weaknet
SPEC.loader.exec_module(weaknet)


def test_summary_reports_display_metrics() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        sender_log = root / "sender.log"
        receiver_log = root / "receiver.log"
        sender_log.write_text(
            "[elapsed=1.000] profile=60kbps/10fps/160x90\n",
            encoding="utf-8",
        )
        receiver_log.write_text(
            "[elapsed=1.000] frames=9 dropped=0 decoder_errors=0 "
            "decoded_fps=9 output_fps=20 synthetic_fps=11 "
            "output_resolution=640x360 max_frame_gap_ms=500 "
            "latency_avg=100.0ms latency_p95=120.0ms "
            "latency_max=140.0ms latency_samples=9\n",
            encoding="utf-8",
        )

        summary = weaknet.summarize_scenario(
            weaknet.Scenario(
                name="loss_80",
                kind="steady",
                steps=(weaknet.Step(80, 50, 60),),
            ),
            sender_log,
            receiver_log,
            root / "received.h264",
            {"passed": True, "returncode": 0, "bytes": 100},
            {"available": True, "nb_read_frames": "9"},
            [{"elapsed": 0.5, "loss_percent": 80}],
        )

        metrics = summary["metrics"]
        assert metrics["decoded_fps"] == 9
        assert metrics["output_fps"] == 20
        assert metrics["synthetic_fps"] == 11
        assert metrics["output_resolution"] == "640x360"


def test_tc09_uses_post_warmup_average() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        sender_log = root / "sender.log"
        receiver_log = root / "receiver.log"
        sender_log.write_text(
            "[elapsed=1.000] profile=60kbps/10fps/160x90\n",
            encoding="utf-8",
        )
        receiver_log.write_text(
            "\n".join(
                [
                    "[elapsed=1.000] frames=1 decoder_errors=0 "
                    "decoded_fps=1 output_fps=5 synthetic_fps=4 "
                    "output_resolution=640x360 max_frame_gap_ms=500 "
                    "latency_avg=100.0ms latency_samples=1",
                    "[elapsed=6.000] frames=8 decoder_errors=0 "
                    "decoded_fps=8 output_fps=20 synthetic_fps=12 "
                    "output_resolution=640x360 max_frame_gap_ms=500 "
                    "latency_avg=100.0ms latency_samples=8",
                    "[elapsed=7.000] frames=18 decoder_errors=0 "
                    "decoded_fps=10 output_fps=20 synthetic_fps=10 "
                    "output_resolution=640x360 max_frame_gap_ms=500 "
                    "latency_avg=100.0ms latency_samples=18",
                    "[elapsed=8.000] frames=25 decoder_errors=0 "
                    "decoded_fps=7 output_fps=20 synthetic_fps=13 "
                    "output_resolution=640x360 max_frame_gap_ms=500 "
                    "latency_avg=100.0ms latency_samples=25",
                ]
            )
            + "\n",
            encoding="utf-8",
        )

        summary = weaknet.summarize_scenario(
            weaknet.Scenario(
                name="loss_80",
                kind="steady",
                steps=(weaknet.Step(80, 50, 20),),
            ),
            sender_log,
            receiver_log,
            root / "received.h264",
            {"passed": True, "returncode": 0, "bytes": 100},
            {"available": True, "nb_read_frames": "25"},
            [{"elapsed": 0.0, "loss_percent": 80}],
        )

        assert summary["checks"]["TC-09"]["passed"]
        assert summary["metrics"]["decoded_fps"] > 8


def test_runtime_config_enables_dummy_display_metrics() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        output_file = root / "received.h264"
        config_path = weaknet.write_runtime_config(root, output_file)
        text = config_path.read_text(encoding="utf-8")
        assert "  display: true\n" in text
        assert "  minimum_output_fps: 20\n" in text


def test_quick_suite_covers_revised_acceptance_losses() -> None:
    scenarios = weaknet.scenarios_for_suite("quick", 7, 5)
    steady_losses = [
        scenario.steps[0].loss_percent
        for scenario in scenarios
        if scenario.kind == "steady"
    ]
    assert steady_losses == [0, 20, 45, 70, 80]


def test_staircase_downgrade_allows_feedback_scheduling_margin() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        sender_log = root / "sender.log"
        receiver_log = root / "receiver.log"
        sender_log.write_text(
            "\n".join(
                [
                    "[elapsed=1.000] profile=2000kbps/30fps/1280x720",
                    "[elapsed=12.900] profile_change=1 reason=test old=2000kbps/30fps/1280x720 new=900kbps/24fps/640x360",
                    "[elapsed=23.900] profile_change=1 reason=test old=900kbps/24fps/640x360 new=350kbps/20fps/426x240",
                    "[elapsed=36.800] profile_change=1 reason=test old=350kbps/20fps/426x240 new=60kbps/10fps/160x90",
                    "[elapsed=50.000] profile_change=1 reason=test old=60kbps/10fps/160x90 new=350kbps/20fps/426x240",
                    "[elapsed=61.000] profile_change=1 reason=test old=350kbps/20fps/426x240 new=900kbps/24fps/640x360",
                ]
            )
            + "\n",
            encoding="utf-8",
        )
        receiver_log.write_text(
            "[elapsed=70.000] frames=100 decoder_errors=0 "
            "decoded_fps=10 output_fps=20 synthetic_fps=10 "
            "output_resolution=640x360 max_frame_gap_ms=500 "
            "latency_avg=100.0ms latency_samples=100\n",
            encoding="utf-8",
        )

        summary = weaknet.summarize_scenario(
            weaknet.Scenario(
                name="staircase",
                kind="staircase",
                steps=(
                    weaknet.Step(0, 50, 8),
                    weaknet.Step(10, 50, 8),
                    weaknet.Step(50, 50, 8),
                    weaknet.Step(80, 50, 8),
                    weaknet.Step(50, 50, 8),
                    weaknet.Step(10, 50, 8),
                ),
            ),
            sender_log,
            receiver_log,
            root / "received.h264",
            {"passed": True, "returncode": 0, "bytes": 100},
            {"available": True, "nb_read_frames": "100"},
            [
                {"elapsed": 4.0, "loss_percent": 0},
                {"elapsed": 12.0, "loss_percent": 10},
                {"elapsed": 21.1, "loss_percent": 50},
                {"elapsed": 34.0, "loss_percent": 80},
                {"elapsed": 45.5, "loss_percent": 50},
                {"elapsed": 53.7, "loss_percent": 10},
            ],
        )

        assert summary["checks"]["TC-07"]["passed"]


def main() -> int:
    test_summary_reports_display_metrics()
    test_tc09_uses_post_warmup_average()
    test_runtime_config_enables_dummy_display_metrics()
    test_quick_suite_covers_revised_acceptance_losses()
    test_staircase_downgrade_allows_feedback_scheduling_margin()
    print("weaknet_acceptance_tests=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
