#include "FrontendSelection.hpp"

#include <unistd.h>

namespace cch::cli {
namespace {

[[nodiscard]] bool descriptor_is_terminal(int descriptor) {
    return ::isatty(descriptor) != 0;
}

} // namespace

FrontendEnvironment detect_frontend_environment() {
    constexpr int kInputDescriptor = 0;
    constexpr int kOutputDescriptor = 1;
    return FrontendEnvironment{
        .stdin_is_terminal = descriptor_is_terminal(kInputDescriptor),
        .stdout_is_terminal = descriptor_is_terminal(kOutputDescriptor),
    };
}

support::Expected<Frontend> select_frontend(
    const CliConfig& config,
    FrontendEnvironment environment) {
    if (config.print || !environment.stdin_is_terminal || !environment.stdout_is_terminal) {
        return Frontend::Print;
    }
    return Frontend::Interactive;
}

} // namespace cch::cli
