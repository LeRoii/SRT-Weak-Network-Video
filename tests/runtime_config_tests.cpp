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
        "sender:\n"
        "  connect: 127.0.0.1:9000\n"
        "  video_file: /tmp/input.mp4\n"
        "  max_video_kbps: 500\n"
        "receiver:\n"
        "  listen: 0.0.0.0:9000\n"
        "  display: false\n"
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

} // namespace

int main() {
    const auto missing =
        std::filesystem::temp_directory_path() /
        "srt-runtime-config-missing.yaml";
    std::filesystem::remove(missing);
    const auto defaults = load_runtime_config(missing, true);
    assert(defaults.sender.connect == "127.0.0.1:9000");
    assert(defaults.sender.max_video_kbps == 2000);
    assert(defaults.receiver.listen == "0.0.0.0:9000");
    assert(defaults.receiver.display);
    assert(defaults.receiver.write_h264);
    assert(defaults.receiver.output_file == "received.h264");
    assert(defaults.latency.metric == LatencyMetric::EncodeToDecode);
    assert(defaults.latency.window_seconds == 10);
    assert(load_fails(missing));

    const auto valid = write_config(
        "srt-runtime-config-valid.yaml",
        complete_config("encode_to_assemble", "5", "false"));
    const auto config = load_runtime_config(valid, false);
    assert(config.sender.video_file == "/tmp/input.mp4");
    assert(config.sender.max_video_kbps == 500);
    assert(!config.receiver.display);
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
        complete_config() +
        "  max_video_kbps: 3000\n");
    assert(load_fails(invalid_bitrate));

    const auto invalid_boolean = write_config(
        "srt-runtime-config-invalid-boolean.yaml",
        complete_config("encode_to_decode", "10", "yes"));
    assert(load_fails(invalid_boolean));

    const auto missing_section = write_config(
        "srt-runtime-config-missing-section.yaml",
        "latency:\n"
        "  metric: encode_to_decode\n");
    assert(load_fails(missing_section));

    std::filesystem::remove(valid);
    std::filesystem::remove(display);
    std::filesystem::remove(invalid_metric);
    std::filesystem::remove(invalid_window);
    std::filesystem::remove(invalid_bitrate);
    std::filesystem::remove(invalid_boolean);
    std::filesystem::remove(missing_section);

    std::cout << "runtime_config_tests=passed" << std::endl;
    return 0;
}
