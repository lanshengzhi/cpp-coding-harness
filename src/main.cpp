#include "coding_agent/runtime/AsyncCliRuntime.hpp"

#include <iostream>

int main(int argc, char** argv) {
    return cch::cli::run_cli_entry(
        argc,
        argv,
        cch::cli::CliStreams{std::cin, std::cout, std::cerr});
}
