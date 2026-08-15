#include "FrontendSelection.hpp"

#if defined(_WIN32)
#include <cstdio>
#include <io.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace cch::cli {
namespace {

[[nodiscard]] bool descriptor_is_terminal(int descriptor) {
#if defined(_WIN32)
    return ::_isatty(descriptor) != 0;
#elif defined(__unix__) || defined(__APPLE__)
    return ::isatty(descriptor) != 0;
#else
    (void)descriptor;
    return false;
#endif
}

} // namespace

FrontendEnvironment detect_frontend_environment() {
#if defined(_WIN32)
    const int input_descriptor = ::_fileno(stdin);
    const int output_descriptor = ::_fileno(stdout);
#else
    constexpr int kInputDescriptor = 0;
    constexpr int kOutputDescriptor = 1;
#endif
    return FrontendEnvironment{
#if defined(_WIN32)
        .stdin_is_terminal = descriptor_is_terminal(input_descriptor),
        .stdout_is_terminal = descriptor_is_terminal(output_descriptor),
#else
        .stdin_is_terminal = descriptor_is_terminal(kInputDescriptor),
        .stdout_is_terminal = descriptor_is_terminal(kOutputDescriptor),
#endif
#if defined(__linux__) || defined(__APPLE__)
        .interactive_supported = true,
#else
        .interactive_supported = false,
#endif
    };
}

support::Expected<Frontend> select_frontend(
    const CliConfig& config,
    FrontendEnvironment environment) {
    if (config.print || !environment.stdin_is_terminal || !environment.stdout_is_terminal) {
        return Frontend::Print;
    }
    if (!environment.interactive_supported) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Native TUI interactive mode is not supported on this platform",
            "use --print instead"));
    }
    return Frontend::Interactive;
}

} // namespace cch::cli
