// Real-terminal (Kitty) drive of the frozen `audit-1035` placement-span scenario for the
// C++ owner (#1035). Mirrors `tests/tui/AltScreenPlacementSpanTest.cpp` (the VirtualTerminal
// byte replay) but drives a real `cch::tui::AltScreen` over the real
// `cch::tui::ProcessTerminal`, so the emulator performs the actual placement and repaint.
// Built ad hoc against the Debug preset libraries (see run-capture.sh); it is an evidence
// generator, not a shipped or tested target.
//
// Usage (inside a real Kitty window, stdout = PTY, 20x4 grid):
//   WEZTERM_PANE=1 ./capture_placement_span wezterm /tmp/placement-image.b64 /tmp/out.log
//   # omit WEZTERM_PANE for the plain-terminal control arm.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Component.hpp>
#include <cch/tui/Image.hpp>
#include <cch/tui/ProcessTerminal.hpp>

#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class SpanChild final : public cch::tui::Component {
public:
    explicit SpanChild(std::vector<cch::tui::InlineImageRenderRegion> images) : images_(std::move(images)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_, .images = images_};
    }

    void invalidate() override {}
    void set_covered(std::string value) { lines_[1] = std::move(value); }
    void set_tail(std::string value) { lines_[3] = std::move(value); }

private:
    std::vector<std::string> lines_{"", "", "", "after"};
    std::vector<cch::tui::InlineImageRenderRegion> images_;
};

[[nodiscard]] cch::tui::InlineImageRenderRegion placement_region(const std::string& base64_png) {
    cch::tui::InlineImageRenderRegion region;
    region.resource_id = 7301;
    region.revision = 1;
    region.encoded_data = base64_png;
    region.mime_type = "image/png";
    region.pixel_width = 100;
    region.pixel_height = 100;
    region.fallback_text = cch::tui::image_fallback(
            region.mime_type, cch::tui::ImagePixelSize{.width = region.pixel_width, .height = region.pixel_height});
    region.region = cch::tui::CellRegion{.column = 0, .row = 0, .columns = 2, .rows = 3};
    region.source_rows = region.region.rows;
    region.rows_reserved_in_lines = true;
    return region;
}

[[nodiscard]] std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string phase_dir() {
    if (const auto* dir = std::getenv("PHASE_DIR"); dir != nullptr) return dir;
    return {};
}

void mark_phase(const std::string& dir, const std::string& name) {
    if (dir.empty()) return;
    const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    std::ofstream marker(dir + "/" + std::to_string(now) + "-" + name + ".txt", std::ios::trunc);
    marker << name << '\n';
}

/// Set the winsize to 20x4 and wait until the real terminal grid adopts it (kitty resizes
/// its window to the child-set winsize; verified by probe on this host). Bounded so a stuck
/// resize fails loudly.
bool wait_for_grid(int fd) {
    const winsize target{.ws_row = 4, .ws_col = 20, .ws_xpixel = 0, .ws_ypixel = 0};
    for (int attempt = 0; attempt < 100; ++attempt) {
        (void)::ioctl(fd, TIOCSWINSZ, &target);
        winsize size{};
        if (::ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col == 20 && size.ws_row == 4) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: capture_placement_span <wezterm|control> <png.b64> <out.log>\n";
        return 2;
    }
    const std::string scenario = argv[1];
    if (scenario != "wezterm" && scenario != "control") return 2;

    // Mirror the replay's pinning: WezTerm trigger keys off WEZTERM_PANE/TERM_PROGRAM.
    if (scenario == "wezterm")
        (void)::setenv("WEZTERM_PANE", "1", 1);
    else
        (void)::unsetenv("WEZTERM_PANE");
    (void)::unsetenv("TERM_PROGRAM");

    std::ofstream log(argv[3], std::ios::binary | std::ios::trunc);
    if (!::wait_for_grid(STDOUT_FILENO)) {
        log << "ERROR: terminal did not reach 20x4\n";
        return 1;
    }

    cch::tui::ProcessTerminal terminal;
    cch::tui::AltScreen tui(terminal);
    std::vector<cch::tui::InlineImageRenderRegion> images;
    images.push_back(placement_region(read_file(argv[2])));
    auto child = std::make_unique<SpanChild>(std::move(images));
    auto* child_pointer = child.get();
    tui.set_layout_root(std::move(child));

    if (!tui.start()) {
        log << "ERROR: AltScreen start failed\n";
        return 1;
    }
    const auto phases = phase_dir();
    log << "phase initial\n";
    if (!tui.render()) return 1;
    mark_phase(phases, "initial");
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    child_pointer->set_covered("changed");
    log << "phase covered-text-change\n";
    if (!tui.render()) return 1;
    mark_phase(phases, "covered-text-change");
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    child_pointer->set_tail("done");
    log << "phase outside-span-change\n";
    if (!tui.render()) return 1;
    mark_phase(phases, "outside-span-change");
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    if (!tui.stop()) return 1;
    log << "phase stop\n";
    return 0;
}
