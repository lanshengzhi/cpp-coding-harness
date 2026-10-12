#include <cch/tui/TerminalImage.hpp>

#include <iterator>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// Behavioral baseline: pi v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df,
// packages/tui/src/components/image.ts (`setImageTranscoder`/`toPng`). The
// module-level registry holds the injected capability so protocol selection
// stays terminal-owned; only the injected converter knows how to decode an
// image.
namespace cch::tui {
namespace {

constexpr std::size_t kPngCacheCapacity = 32;

struct PngCacheEntry {
    std::optional<std::string> png_data;
    std::list<std::string>::iterator recency;
};

std::mutex g_png_transcoder_mutex;
ImageTranscoder g_png_transcoder;
std::list<std::string> g_png_cache_recency;
std::unordered_map<std::string, PngCacheEntry> g_png_cache;

void clear_png_cache() {
    g_png_cache.clear();
    g_png_cache_recency.clear();
}

} // namespace

void set_image_transcoder(ImageTranscoder transcoder) {
    const std::lock_guard lock(g_png_transcoder_mutex);
    g_png_transcoder = std::move(transcoder);
    clear_png_cache();
}

void clear_image_transcoder() { set_image_transcoder({}); }

std::optional<std::string> to_png(std::string_view base64_data, std::string_view mime_type) {
    const std::lock_guard lock(g_png_transcoder_mutex);
    if (!g_png_transcoder) return std::nullopt;

    const auto key = std::string(base64_data);
    if (const auto found = g_png_cache.find(key); found != g_png_cache.end()) {
        g_png_cache_recency.splice(g_png_cache_recency.end(), g_png_cache_recency, found->second.recency);
        return found->second.png_data;
    }

    // Failures are cached like successes: a payload the host cannot convert
    // must not retry on every frame.
    auto png_data = g_png_transcoder(base64_data, mime_type);
    g_png_cache_recency.push_back(key);
    g_png_cache.emplace(key,
            PngCacheEntry{
                    .png_data = png_data,
                    .recency = std::prev(g_png_cache_recency.end()),
            });
    if (g_png_cache.size() > kPngCacheCapacity) {
        g_png_cache.erase(g_png_cache_recency.front());
        g_png_cache_recency.pop_front();
    }
    return png_data;
}

} // namespace cch::tui
