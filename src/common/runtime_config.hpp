#pragma once

#include <filesystem>
#include <string>

enum class LatencyMetric {
    EncodeToAssemble,
    EncodeToDecode,
    SourceToDisplay,
};

struct LatencyConfig {
    LatencyMetric metric = LatencyMetric::EncodeToDecode;
    int window_seconds = 10;
};

struct RuntimeConfig {
    LatencyConfig latency;
};

const char *latency_metric_name(LatencyMetric metric);
RuntimeConfig load_runtime_config(const std::filesystem::path &path,
                                  bool missing_is_default);
std::filesystem::path default_runtime_config_path();
