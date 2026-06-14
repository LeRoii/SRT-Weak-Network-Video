#include "common/runtime_config.hpp"

#include <cctype>
#include <fstream>
#include <limits.h>
#include <set>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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

SenderInput parse_sender_input(const std::string &value) {
    if (value == "camera") {
        return SenderInput::Camera;
    }
    if (value == "file") {
        return SenderInput::File;
    }
    throw std::runtime_error("sender.input must be camera or file");
}

int parse_integer(const std::string &value,
                  const std::string &name,
                  int minimum,
                  int maximum) {
    std::size_t consumed = 0;
    int result = 0;
    try {
        result = std::stoi(value, &consumed);
    } catch (const std::exception &) {
        throw std::runtime_error(name + " must be an integer");
    }
    if (consumed != value.size() || result < minimum || result > maximum) {
        throw std::runtime_error(
            name + " must be between " + std::to_string(minimum) +
            " and " + std::to_string(maximum));
    }
    return result;
}

bool parse_boolean(const std::string &value, const std::string &name) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error(name + " must be true or false");
}

enum class Section {
    None,
    Sender,
    Receiver,
    Latency,
};

Section parse_section(const std::string &content) {
    if (content == "sender:") {
        return Section::Sender;
    }
    if (content == "receiver:") {
        return Section::Receiver;
    }
    if (content == "latency:") {
        return Section::Latency;
    }
    throw std::runtime_error(
        "runtime config section must be sender, receiver, or latency");
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

const char *sender_input_name(SenderInput input) {
    switch (input) {
    case SenderInput::Camera:
        return "camera";
    case SenderInput::File:
        return "file";
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
    Section section = Section::None;
    bool saw_sender = false;
    bool saw_receiver = false;
    bool saw_latency = false;
    std::set<std::string> seen_keys;
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
            section = parse_section(content);
            bool *seen = nullptr;
            if (section == Section::Sender) {
                seen = &saw_sender;
            } else if (section == Section::Receiver) {
                seen = &saw_receiver;
            } else {
                seen = &saw_latency;
            }
            if (*seen) {
                throw std::runtime_error(
                    "runtime config line " + std::to_string(line_number) +
                    " repeats a section");
            }
            *seen = true;
            continue;
        }
        if (section == Section::None || indent != 2) {
            throw std::runtime_error(
                "runtime config line " + std::to_string(line_number) +
                " must use two-space indentation under a section");
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
        const std::string qualified_key =
            std::to_string(static_cast<int>(section)) + ":" + key;
        if (!seen_keys.insert(qualified_key).second) {
            throw std::runtime_error(
                "duplicate runtime config key '" + key + "'");
        }
        if (section == Section::Sender) {
            if (key == "input") {
                config.sender.input = parse_sender_input(value);
            } else if (key == "connect") {
                config.sender.connect = value;
            } else if (key == "video_file") {
                config.sender.video_file = value;
            } else if (key == "camera_device") {
                config.sender.camera_device = value;
            } else if (key == "camera_width") {
                config.sender.camera_width = parse_integer(
                    value, "sender.camera_width", 16, 8192);
            } else if (key == "camera_height") {
                config.sender.camera_height = parse_integer(
                    value, "sender.camera_height", 16, 8192);
            } else if (key == "camera_fps") {
                config.sender.camera_fps = parse_integer(
                    value, "sender.camera_fps", 1, 240);
            } else if (key == "max_video_kbps") {
                config.sender.max_video_kbps = parse_integer(
                    value, "sender.max_video_kbps", 8, 2000);
            } else {
                throw std::runtime_error(
                    "unknown sender config key '" + key + "'");
            }
        } else if (section == Section::Receiver) {
            if (key == "listen") {
                config.receiver.listen = value;
            } else if (key == "display") {
                config.receiver.display =
                    parse_boolean(value, "receiver.display");
            } else if (key == "write_h264") {
                config.receiver.write_h264 =
                    parse_boolean(value, "receiver.write_h264");
            } else if (key == "output_file") {
                config.receiver.output_file = value;
            } else {
                throw std::runtime_error(
                    "unknown receiver config key '" + key + "'");
            }
        } else {
            if (key == "metric") {
                config.latency.metric = parse_metric(value);
            } else if (key == "window_seconds") {
                config.latency.window_seconds = parse_integer(
                    value, "latency.window_seconds", 1, 3600);
            } else {
                throw std::runtime_error(
                    "unknown latency config key '" + key + "'");
            }
        }
    }

    if (!saw_sender || !saw_receiver || !saw_latency) {
        throw std::runtime_error(
            "runtime config must contain sender, receiver, and latency sections");
    }
    return config;
}

std::filesystem::path default_runtime_config_path() {
#ifdef _WIN32
    char executable[MAX_PATH + 1] = {};
    const DWORD size = GetModuleFileNameA(
        nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    if (size > 0 && size < sizeof(executable)) {
        return std::filesystem::path(executable).parent_path() /
               "runtime-config.yaml";
    }
#else
    char executable[PATH_MAX + 1] = {};
    const auto size =
        readlink("/proc/self/exe", executable, PATH_MAX);
    if (size > 0) {
        executable[size] = '\0';
        return std::filesystem::path(executable).parent_path() /
               "runtime-config.yaml";
    }
#endif
    return "runtime-config.yaml";
}
