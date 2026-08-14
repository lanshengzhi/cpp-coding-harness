#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Style.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace cch::tui {

struct ImageContent {
    std::string encoded_data{};
    std::string mime_type{};
    std::optional<std::string> filename{std::nullopt};
};

struct ImageCellConstraints {
    std::optional<std::size_t> max_width{std::nullopt};
    std::optional<std::size_t> max_height{std::nullopt};
};

struct ImageOptions {
    ImageCellConstraints constraints{};
    TextStyleHook fallback_style{};
};

/// A reusable encoded-image Component with a bounded textual fallback.
class Image final : public Component {
public:
    explicit Image(ImageContent content, ImageOptions options = {});
    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    ~Image() override;

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    void set_content(ImageContent content);
    void clear();
    [[nodiscard]] bool has_content() const;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui
