#include "common/runtime_config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::filesystem::path write_config(const std::string &name,
                                   const std::string &contents) {
    const auto path =
        std::filesystem::temp_directory_path() / name;
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    output.close();
    return path;
}

std::string complete_config(const std::string &metric = "encode_to_decode",
                            const std::string &window = "10",
                            const std::string &write_h264 = "true") {
    return
        "transport:\n"
        "  mode: udp\n"
        "  feedback_interval_ms: 200\n"
        "  feedback_redundancy: 10\n"
        "  feedback_timeout_ms: 1000\n"
        "sender:\n"
        "  input: camera\n"
        "  connect: 127.0.0.1:9000\n"
        "  video_file: /tmp/input.mp4\n"
        "  camera_device: /dev/video9\n"
        "  camera_width: 640\n"
        "  camera_height: 480\n"
        "  camera_fps: 30\n"
        "  max_video_kbps: 500\n"
        "receiver:\n"
        "  listen: 0.0.0.0:9000\n"
        "  display: false\n"
        "  minimum_output_width: 800\n"
        "  minimum_output_height: 450\n"
        "  minimum_output_fps: 30\n"
        "  upscale_mode: lanczos_sharpen\n"
        "  interpolation_mode: blend\n"
        "  write_h264: " + write_h264 + "\n"
        "  output_file: /tmp/output.h264\n"
        "latency:\n"
        "  metric: " + metric + "\n"
        "  window_seconds: " + window + "\n";
}

bool load_fails(const std::filesystem::path &path) {
    try {
        (void)load_runtime_config(path, false);
        return false;
    } catch (const std::runtime_error &) {
        return true;
    }
}

std::string replace_once(std::string text,
                         const std::string &from,
                         const std::string &to) {
    const auto position = text.find(from);
    assert(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

} // namespace

int main() {
    const auto missing =
        std::filesystem::temp_directory_path() /
        "srt-runtime-config-missing.yaml";
    std::filesystem::remove(missing);
    const auto defaults = load_runtime_config(missing, true);
    assert(defaults.transport.mode == TransportMode::Udp);
    assert(defaults.transport.feedback_interval_ms == 200);
    assert(defaults.transport.feedback_redundancy == 10);
    assert(defaults.transport.feedback_timeout_ms == 1000);
    assert(defaults.sender.connect == "10.88.0.2:9000");
    assert(defaults.sender.input == SenderInput::File);
    assert(defaults.sender.video_file == "/home/u20/code/jetson-2k.mp4");
    assert(defaults.sender.camera_device == "/dev/video0");
    assert(defaults.sender.camera_width == 640);
    assert(defaults.sender.camera_height == 480);
    assert(defaults.sender.camera_fps == 30);
    assert(defaults.sender.max_video_kbps == 2000);
    assert(defaults.receiver.listen == "10.88.0.2:9000");
    assert(defaults.receiver.display);
    assert(defaults.receiver.minimum_output_width == 640);
    assert(defaults.receiver.minimum_output_height == 360);
    assert(defaults.receiver.minimum_output_fps == 30);
    assert(defaults.receiver.upscale_mode ==
           UpscaleMode::LanczosSharpen);
    assert(defaults.receiver.interpolation_mode ==
           InterpolationMode::Blend);
    assert(defaults.receiver.write_h264);
    assert(defaults.receiver.output_file == "received.h264");
    assert(defaults.latency.metric == LatencyMetric::EncodeToDecode);
    assert(defaults.latency.window_seconds == 10);
    assert(load_fails(missing));

    const auto empty = write_config(
        "srt-runtime-config-empty.yaml",
        "");
    const auto empty_defaults = load_runtime_config(empty, false);
    assert(empty_defaults.sender.input == SenderInput::File);
    assert(empty_defaults.sender.connect == "10.88.0.2:9000");
    assert(empty_defaults.receiver.listen == "10.88.0.2:9000");
    assert(empty_defaults.receiver.minimum_output_fps == 30);

    const auto partial = write_config(
        "srt-runtime-config-partial.yaml",
        "sender:\n"
        "  connect: 127.0.0.1:9100\n"
        "receiver:\n"
        "  display: false\n");
    const auto partial_config = load_runtime_config(partial, false);
    assert(partial_config.transport.mode == TransportMode::Udp);
    assert(partial_config.sender.input == SenderInput::File);
    assert(partial_config.sender.connect == "127.0.0.1:9100");
    assert(partial_config.receiver.listen == "10.88.0.2:9000");
    assert(!partial_config.receiver.display);
    assert(partial_config.latency.metric == LatencyMetric::EncodeToDecode);

    const auto valid = write_config(
        "srt-runtime-config-valid.yaml",
        complete_config("encode_to_assemble", "5", "false"));
    const auto config = load_runtime_config(valid, false);
    assert(config.transport.mode == TransportMode::Udp);
    assert(config.sender.video_file == "/tmp/input.mp4");
    assert(config.sender.input == SenderInput::Camera);
    assert(config.sender.camera_device == "/dev/video9");
    assert(config.sender.max_video_kbps == 500);
    assert(!config.receiver.display);
    assert(config.receiver.minimum_output_width == 800);
    assert(config.receiver.minimum_output_height == 450);
    assert(config.receiver.minimum_output_fps == 30);
    assert(!config.receiver.write_h264);
    assert(config.receiver.output_file == "/tmp/output.h264");
    assert(config.latency.metric == LatencyMetric::EncodeToAssemble);
    assert(config.latency.window_seconds == 5);

    const auto display = write_config(
        "srt-runtime-config-display.yaml",
        complete_config("source_to_display", "30"));
    assert(load_runtime_config(display, false).latency.metric ==
           LatencyMetric::SourceToDisplay);

    const auto invalid_metric = write_config(
        "srt-runtime-config-invalid-metric.yaml",
        complete_config("round_trip"));
    assert(load_fails(invalid_metric));

    const auto invalid_window = write_config(
        "srt-runtime-config-invalid-window.yaml",
        complete_config("encode_to_decode", "0"));
    assert(load_fails(invalid_window));

    const auto invalid_bitrate = write_config(
        "srt-runtime-config-invalid-bitrate.yaml",
        replace_once(complete_config(),
                     "max_video_kbps: 500",
                     "max_video_kbps: 3000"));
    assert(load_fails(invalid_bitrate));

    const auto bitrate_below_ladder = write_config(
        "srt-runtime-config-bitrate-below-ladder.yaml",
        replace_once(complete_config(),
                     "max_video_kbps: 500",
                     "max_video_kbps: 59"));
    assert(load_fails(bitrate_below_ladder));

    const auto invalid_input = write_config(
        "srt-runtime-config-invalid-input.yaml",
        replace_once(complete_config(),
                     "input: camera",
                     "input: stream"));
    assert(load_fails(invalid_input));

    const auto invalid_camera_fps = write_config(
        "srt-runtime-config-invalid-camera-fps.yaml",
        replace_once(complete_config(),
                     "camera_fps: 30",
                     "camera_fps: 0"));
    assert(load_fails(invalid_camera_fps));

    const auto invalid_boolean = write_config(
        "srt-runtime-config-invalid-boolean.yaml",
        complete_config("encode_to_decode", "10", "yes"));
    assert(load_fails(invalid_boolean));

    const auto invalid_transport = write_config(
        "srt-runtime-config-invalid-transport.yaml",
        replace_once(complete_config(),
                     "mode: udp",
                     "mode: tcp"));
    assert(load_fails(invalid_transport));

    const auto invalid_feedback_redundancy = write_config(
        "srt-runtime-config-invalid-feedback-redundancy.yaml",
        replace_once(complete_config(),
                     "feedback_redundancy: 10",
                     "feedback_redundancy: 0"));
    assert(load_fails(invalid_feedback_redundancy));

    const auto invalid_output_width = write_config(
        "srt-runtime-config-invalid-output-width.yaml",
        replace_once(complete_config(),
                     "minimum_output_width: 800",
                     "minimum_output_width: 639"));
    assert(load_fails(invalid_output_width));

    const auto invalid_output_height = write_config(
        "srt-runtime-config-invalid-output-height.yaml",
        replace_once(complete_config(),
                     "minimum_output_height: 450",
                     "minimum_output_height: 8"));
    assert(load_fails(invalid_output_height));

    const auto invalid_output_fps = write_config(
        "srt-runtime-config-invalid-output-fps.yaml",
        replace_once(complete_config(),
                     "minimum_output_fps: 30",
                     "minimum_output_fps: 0"));
    assert(load_fails(invalid_output_fps));

    const auto bilinear_repeat = write_config(
        "srt-runtime-config-bilinear-repeat.yaml",
        replace_once(
            replace_once(complete_config(),
                         "upscale_mode: lanczos_sharpen",
                         "upscale_mode: bilinear"),
            "interpolation_mode: blend",
            "interpolation_mode: repeat"));
    const auto alternate =
        load_runtime_config(bilinear_repeat, false);
    assert(alternate.receiver.upscale_mode ==
           UpscaleMode::Bilinear);
    assert(alternate.receiver.interpolation_mode ==
           InterpolationMode::Repeat);

    const auto invalid_upscale = write_config(
        "srt-runtime-config-invalid-upscale.yaml",
        replace_once(complete_config(),
                     "upscale_mode: lanczos_sharpen",
                     "upscale_mode: nearest"));
    assert(load_fails(invalid_upscale));

    const auto invalid_interpolation = write_config(
        "srt-runtime-config-invalid-interpolation.yaml",
        replace_once(complete_config(),
                     "interpolation_mode: blend",
                     "interpolation_mode: optical_flow"));
    assert(load_fails(invalid_interpolation));

    const auto latency_only = write_config(
        "srt-runtime-config-latency-only.yaml",
        "latency:\n"
        "  metric: encode_to_assemble\n");
    const auto latency_only_config =
        load_runtime_config(latency_only, false);
    assert(latency_only_config.sender.input == SenderInput::File);
    assert(latency_only_config.latency.metric ==
           LatencyMetric::EncodeToAssemble);

    std::filesystem::remove(empty);
    std::filesystem::remove(partial);
    std::filesystem::remove(valid);
    std::filesystem::remove(display);
    std::filesystem::remove(invalid_metric);
    std::filesystem::remove(invalid_window);
    std::filesystem::remove(invalid_bitrate);
    std::filesystem::remove(bitrate_below_ladder);
    std::filesystem::remove(invalid_input);
    std::filesystem::remove(invalid_camera_fps);
    std::filesystem::remove(invalid_boolean);
    std::filesystem::remove(invalid_transport);
    std::filesystem::remove(invalid_feedback_redundancy);
    std::filesystem::remove(invalid_output_width);
    std::filesystem::remove(invalid_output_height);
    std::filesystem::remove(invalid_output_fps);
    std::filesystem::remove(bilinear_repeat);
    std::filesystem::remove(invalid_upscale);
    std::filesystem::remove(invalid_interpolation);
    std::filesystem::remove(latency_only);

    std::cout << "runtime_config_tests=passed" << std::endl;
    return 0;
}
