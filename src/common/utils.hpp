#pragma once

#include "common/types.hpp"

#include <atomic>
#include <cstdint>
#include <string>

extern std::atomic<bool> g_stop_requested;

void handle_signal(int);
int64_t monotonic_us();
Endpoint parse_endpoint(const std::string &text);
