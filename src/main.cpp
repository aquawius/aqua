#include <iostream>
#include <format>
#include <print>

#include <cxxopts.hpp>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include "config.h"

void wait_3_sec()
{
    spdlog::info("[TEST] Waiting for 3 sec.");
    std::this_thread::sleep_for(std::chrono::seconds(3));
}

int main(int argc, const char* argv[])
{
    spdlog::set_level(spdlog::level::trace);
    spdlog::info("{}\tversion: {}\tplatform: {}",
                 aqua_client_BINARY_NAME, aqua_client_VERSION, aqua_client_PLATFORM_NAME);
    spdlog::info("[TEST] Hello World!");
    return 0;
}
