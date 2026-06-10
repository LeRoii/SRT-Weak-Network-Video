#include "common/runtime_config.hpp"

#include <cctype>
#include <fstream>
#include <limits.h>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace {

std::string trim(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() &&
           std::isspace(static_cast<unsigned char>(text[first]))) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(text[last - 1]))) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

LatencyMetric parse_metric(const std::string &value) {
    if (value == "encode_to_assemble") {
        return LatencyMetric::EncodeToAssemble;
    }
    if (value == "encode_to_decode") {
        return LatencyMetric::EncodeToDecode;
    }
    if (value == "source_to_display") {
        return LatencyMetric::SourceToDisplay;
    }
    throw std::runtime_error(
        "latency.metric must be encode_to_assemble, encode_to_decode, "
        "or source_to_display");
}

int parse_window_seconds(const std::string &value) {
    std::size_t consumed = 0;
    int seconds = 0;
    try {
        seconds = std::stoi(value, &consumed);
    } catch (const std::exception &) {
        throw std::runtime_error(
            "latency.window_seconds must be an integer");
    }
    if (consumed != value.size() || seconds < 1 || seconds > 3600) {
        throw std::runtime_error(
            "latency.window_seconds must be between 1 and 3600");
    }
    return seconds;
}

} // namespace

const char *latency_metric_name(LatencyMetric metric) {
    switch (metric) {
    case LatencyMetric::EncodeToAssemble:
        return "encode_to_assemble";
    case LatencyMetric::EncodeToDecode:
        return "encode_to_decode";
    case LatencyMetric::SourceToDisplay:
        return "source_to_display";
    }
    return "unknown";
}

RuntimeConfig load_runtime_config(const std::filesystem::path &path,
                                  bool missing_is_default) {
    std::ifstream input(path);
    if (!input) {
        if (missing_is_default &&
            !std::filesystem::exists(path)) {
            return {};
        }
        throw std::runtime_error(
            "cannot open runtime config '" + path.string() + "'");
    }

    RuntimeConfig config;
    bool in_latency = false;
    bool saw_latency = false;
    bool saw_metric = false;
    bool saw_window = false;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        if (trim(line).empty()) {
            continue;
        }

        std::size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') {
            ++indent;
        }
        if (indent < line.size() && line[indent] == '\t') {
            throw std::runtime_error(
                "runtime config line " + std::to_string(line_number) +
                " uses tabs for indentation");
        }
        const std::string content = trim(line);
        if (indent == 0) {
            if (content != "latency:" || saw_latency) {
                throw std::runtime_error(
                    "runtime config line " + std::to_string(line_number) +
                    " must be the unique 'latency:' section");
            }
            saw_latency = true;
            in_latency = true;
            continue;
        }
        if (!in_latency || indent != 2) {
            throw std::runtime_error(
                "runtime config line " + std::to_string(line_number) +
                " must use two-space indentation under latency");
        }

        const auto separator = content.find(':');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "runtime config line " + std::to_string(line_number) +
                " must contain key: value");
        }
        const std::string key = trim(content.substr(0, separator));
        const std::string value = trim(content.substr(separator + 1));
        if (value.empty()) {
            throw std::runtime_error(
                "runtime config line " + std::to_string(line_number) +
                " has an empty value");
        }
        if (key == "metric") {
            if (saw_metric) {
                throw std::runtime_error("duplicate latency.metric");
            }
            config.latency.metric = parse_metric(value);
            saw_metric = true;
        } else if (key == "window_seconds") {
            if (saw_window) {
                throw std::runtime_error(
                    "duplicate latency.window_seconds");
            }
            config.latency.window_seconds = parse_window_seconds(value);
            saw_window = true;
        } else {
            throw std::runtime_error(
                "unknown latency config key '" + key + "'");
        }
    }

    if (!saw_latency) {
        throw std::runtime_error(
            "runtime config is missing the latency section");
    }
    return config;
}

std::filesystem::path default_runtime_config_path() {
    char executable[PATH_MAX + 1] = {};
    const auto size =
        readlink("/proc/self/exe", executable, PATH_MAX);
    if (size > 0) {
        executable[size] = '\0';
        return std::filesystem::path(executable).parent_path() /
               "runtime-config.yaml";
    }
    return "runtime-config.yaml";
}
