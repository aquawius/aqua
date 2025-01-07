#include "config.h"

#include <iostream>
#include <format>
#include <print>

int main()
{
    std::print( "aqua_core_VERSION: {}\n", aqua_core_VERSION);
    std::print( "aqua_core_PLATFORM_NAME: {}\n", aqua_core_PLATFORM_NAME);
    std::print( "aqua_core_BINARY_NAME: {}\n", aqua_core_BINARY_NAME);

    return 0;
}