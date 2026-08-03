#include "OauthPage.hpp"

#include <string>
#include <string_view>

namespace cch::ai::auth {
namespace {

constexpr std::string_view kLogoSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 800 800\" "
    "aria-hidden=\"true\"><path fill=\"#fff\" fill-rule=\"evenodd\" "
    "d=\"M165.29 165.29 H517.36 V400 H400 V517.36 H282.65 V634.72 H165.29 Z "
    "M282.65 282.65 V400 H400 V282.65 Z\"/><path fill=\"#fff\" "
    "d=\"M517.36 400 H634.72 V634.72 H517.36 Z\"/></svg>";

[[nodiscard]] std::string escape_html(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        case '\'':
            escaped += "&#39;";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

[[nodiscard]] std::string render_page(
    std::string_view title,
    std::string_view heading,
    std::string_view message,
    std::optional<std::string_view> details) {
    const std::string escaped_title = escape_html(title);
    const std::string escaped_heading = escape_html(heading);
    const std::string escaped_message = escape_html(message);
    const std::string escaped_details = details ? escape_html(*details) : std::string{};

    // Byte-identical to pi's renderPage template at baseline 83114817.
    std::string page = R"(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>)";
    page += escaped_title;
    page += R"(</title>
  <style>
    :root {
      --text: #fafafa;
      --text-dim: #a1a1aa;
      --page-bg: #09090b;
      --font-sans: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, "Noto Sans", sans-serif, "Apple Color Emoji", "Segoe UI Emoji", "Segoe UI Symbol", "Noto Color Emoji";
      --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    }
    * { box-sizing: border-box; }
    html { color-scheme: dark; }
    body {
      margin: 0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 24px;
      background: var(--page-bg);
      color: var(--text);
      font-family: var(--font-sans);
      text-align: center;
    }
    main {
      width: 100%;
      max-width: 560px;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
    }
    .logo {
      width: 72px;
      height: 72px;
      display: block;
      margin-bottom: 24px;
    }
    h1 {
      margin: 0 0 10px;
      font-size: 28px;
      line-height: 1.15;
      font-weight: 650;
      color: var(--text);
    }
    p {
      margin: 0;
      line-height: 1.7;
      color: var(--text-dim);
      font-size: 15px;
    }
    .details {
      margin-top: 16px;
      font-family: var(--font-mono);
      font-size: 13px;
      color: var(--text-dim);
      white-space: pre-wrap;
      word-break: break-word;
    }
  </style>
</head>
<body>
  <main>
    <div class="logo">)";
    page += std::string{kLogoSvg};
    page += R"(</div>
    <h1>)";
    page += escaped_heading;
    page += R"(</h1>
    <p>)";
    page += escaped_message;
    page += R"(</p>
    )";
    if (details) {
        page += "<div class=\"details\">" + escaped_details + "</div>";
    }
    page += R"(
  </main>
</body>
</html>)";
    return page;
}

} // namespace

std::string oauth_success_html(std::string_view message) {
    return render_page(
        "Authentication successful",
        "Authentication successful",
        message,
        std::nullopt);
}

std::string oauth_error_html(
    std::string_view message,
    std::optional<std::string_view> details) {
    return render_page(
        "Authentication failed",
        "Authentication failed",
        message,
        details);
}

} // namespace cch::ai::auth
