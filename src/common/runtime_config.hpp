#pragma once

#include <filesystem>
#include <string>

enum class LatencyMetric {
    EncodeToAssemble,
    EncodeToDecode,
    SourceToDisplay,
};

enum class SenderInput {
    Camera,
    File,
};

enum class TransportMode {
    Udp,
    Srt,
};

struct LatencyConfig {
    LatencyMetric metric = LatencyMetric::EncodeToDecode;
    int window_seconds = 10;
};

struct TransportConfig {
    TransportMode mode = TransportMode::Udp;
    int feedback_interval_ms = 200;
    int feedback_redundancy = 10;
    int feedback_timeout_ms = 1000;
};

struct SenderConfig {
    SenderInput input = SenderInput::Camera;
    std::string connect = "127.0.0.1:9000";
    std::string video_file = "/home/u20/code/jetson-2k.mp4";
    std::string camera_device = "/dev/video0";
    int camera_width = 640;
    int camera_height = 480;
    int camera_fps = 30;
    int max_video_kbps = 2000;
};

struct ReceiverConfig {
    std::string listen = "0.0.0.0:9000";
    bool display = true;
    int output_width = 640;
    int output_height = 360;
    bool write_h264 = true;
    std::string output_file = "received.h264";
};

struct RuntimeConfig {
    TransportConfig transport;
    SenderConfig sender;
    ReceiverConfig receiver;
    LatencyConfig latency;
};

const char *latency_metric_name(LatencyMetric metric);
const char *sender_input_name(SenderInput input);
const char *transport_mode_name(TransportMode mode);
RuntimeConfig load_runtime_config(const std::filesystem::path &path,
                                  bool missing_is_default);
std::filesystem::path default_runtime_config_path();
