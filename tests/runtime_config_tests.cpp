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
    assert(defaults.latency.metric == LatencyMetric::EncodeToDecode);
    assert(defaults.latency.window_seconds == 10);
    assert(load_fails(missing));

    const auto assemble = write_config(
        "srt-runtime-config-assemble.yaml",
        "latency:\n"
        "  metric: encode_to_assemble\n"
        "  window_seconds: 5\n");
    const auto assemble_config =
        load_runtime_config(assemble, false);
    assert(assemble_config.latency.metric ==
           LatencyMetric::EncodeToAssemble);
    assert(assemble_config.latency.window_seconds == 5);

    const auto display = write_config(
        "srt-runtime-config-display.yaml",
        "latency:\n"
        "  metric: source_to_display\n"
        "  window_seconds: 30\n");
    assert(load_runtime_config(display, false).latency.metric ==
           LatencyMetric::SourceToDisplay);

    const auto decode = write_config(
        "srt-runtime-config-decode.yaml",
        "latency:\n"
        "  metric: encode_to_decode\n");
    assert(load_runtime_config(decode, false).latency.metric ==
           LatencyMetric::EncodeToDecode);

    const auto invalid_metric = write_config(
        "srt-runtime-config-invalid-metric.yaml",
        "latency:\n"
        "  metric: round_trip\n");
    assert(load_fails(invalid_metric));

    const auto invalid_window = write_config(
        "srt-runtime-config-invalid-window.yaml",
        "latency:\n"
        "  window_seconds: 0\n");
    assert(load_fails(invalid_window));

    const auto unknown_key = write_config(
        "srt-runtime-config-unknown-key.yaml",
        "latency:\n"
        "  enabled: true\n");
    assert(load_fails(unknown_key));

    std::filesystem::remove(assemble);
    std::filesystem::remove(display);
    std::filesystem::remove(decode);
    std::filesystem::remove(invalid_metric);
    std::filesystem::remove(invalid_window);
    std::filesystem::remove(unknown_key);

    std::cout << "runtime_config_tests=passed" << std::endl;
    return 0;
}
