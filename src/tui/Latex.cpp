#include <cch/tui/Latex.hpp>

#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cch::tui {
namespace {

/// pi's LaTeX grammar tables, keyed by the command name after the backslash.
/// The maps are immutable; they hold the frozen command vocabulary and are
/// consulted, never mutated.
using SymbolTable = std::unordered_map<std::string_view, std::string_view>;
using CommandSet = std::unordered_set<std::string_view>;

[[nodiscard]] const SymbolTable& symbols() {
    static const SymbolTable table{
            {"alpha", "α"},
            {"beta", "β"},
            {"gamma", "γ"},
            {"delta", "δ"},
            {"epsilon", "ϵ"},
            {"varepsilon", "ε"},
            {"zeta", "ζ"},
            {"eta", "η"},
            {"theta", "θ"},
            {"vartheta", "ϑ"},
            {"iota", "ι"},
            {"kappa", "κ"},
            {"varkappa", "ϰ"},
            {"lambda", "λ"},
            {"mu", "μ"},
            {"nu", "ν"},
            {"xi", "ξ"},
            {"pi", "π"},
            {"varpi", "ϖ"},
            {"rho", "ρ"},
            {"varrho", "ϱ"},
            {"sigma", "σ"},
            {"varsigma", "ς"},
            {"tau", "τ"},
            {"upsilon", "υ"},
            {"phi", "ϕ"},
            {"varphi", "φ"},
            {"chi", "χ"},
            {"psi", "ψ"},
            {"omega", "ω"},
            {"Gamma", "Γ"},
            {"Delta", "Δ"},
            {"Theta", "Θ"},
            {"Lambda", "Λ"},
            {"Xi", "Ξ"},
            {"Pi", "Π"},
            {"Sigma", "Σ"},
            {"Upsilon", "Υ"},
            {"Phi", "Φ"},
            {"Psi", "Ψ"},
            {"Omega", "Ω"},
            {"pm", "±"},
            {"mp", "∓"},
            {"times", "×"},
            {"div", "÷"},
            {"cdot", "·"},
            {"ast", "∗"},
            {"star", "⋆"},
            {"circ", "∘"},
            {"bullet", "•"},
            {"oplus", "⊕"},
            {"ominus", "⊖"},
            {"otimes", "⊗"},
            {"oslash", "⊘"},
            {"odot", "⊙"},
            {"bigcirc", "○"},
            {"dagger", "†"},
            {"ddagger", "‡"},
            {"amalg", "⨿"},
            {"uplus", "⊎"},
            {"sqcap", "⊓"},
            {"sqcup", "⊔"},
            {"bowtie", "⋈"},
            {"Join", "⋈"},
            {"ltimes", "⋉"},
            {"rtimes", "⋊"},
            {"leftouterjoin", "⟕"},
            {"rightouterjoin", "⟖"},
            {"fullouterjoin", "⟗"},
            {"triangleleft", "◁"},
            {"triangleright", "▷"},
            {"wr", "≀"},
            {"cap", "∩"},
            {"cup", "∪"},
            {"bigcap", "⋂"},
            {"bigcup", "⋃"},
            {"bigwedge", "⋀"},
            {"bigvee", "⋁"},
            {"bigsqcup", "⨆"},
            {"biguplus", "⨄"},
            {"bigoplus", "⨁"},
            {"bigotimes", "⨂"},
            {"bigodot", "⨀"},
            {"setminus", "∖"},
            {"in", "∈"},
            {"notin", "∉"},
            {"ni", "∋"},
            {"subset", "⊂"},
            {"supset", "⊃"},
            {"subseteq", "⊆"},
            {"supseteq", "⊇"},
            {"sqsubset", "⊏"},
            {"sqsupset", "⊐"},
            {"sqsubseteq", "⊑"},
            {"sqsupseteq", "⊒"},
            {"prec", "≺"},
            {"preceq", "≼"},
            {"succ", "≻"},
            {"succeq", "≽"},
            {"ll", "≪"},
            {"gg", "≫"},
            {"le", "≤"},
            {"leq", "≤"},
            {"leqslant", "≤"},
            {"ge", "≥"},
            {"geq", "≥"},
            {"geqslant", "≥"},
            {"ne", "≠"},
            {"neq", "≠"},
            {"equiv", "≡"},
            {"approx", "≈"},
            {"sim", "∼"},
            {"simeq", "≃"},
            {"cong", "≅"},
            {"asymp", "≍"},
            {"doteq", "≐"},
            {"propto", "∝"},
            {"parallel", "∥"},
            {"perp", "⊥"},
            {"mid", "∣"},
            {"vdash", "⊢"},
            {"dashv", "⊣"},
            {"models", "⊨"},
            {"Vdash", "⊩"},
            {"Vvdash", "⊪"},
            {"nvdash", "⊬"},
            {"nvDash", "⊭"},
            {"forall", "∀"},
            {"exists", "∃"},
            {"nexists", "∄"},
            {"neg", "¬"},
            {"land", "∧"},
            {"wedge", "∧"},
            {"lor", "∨"},
            {"vee", "∨"},
            {"to", "→"},
            {"rightarrow", "→"},
            {"longrightarrow", "→"},
            {"leftarrow", "←"},
            {"longleftarrow", "←"},
            {"gets", "←"},
            {"leftrightarrow", "↔"},
            {"longleftrightarrow", "↔"},
            {"hookleftarrow", "↩"},
            {"hookrightarrow", "↪"},
            {"twoheadleftarrow", "↞"},
            {"twoheadrightarrow", "↠"},
            {"leftharpoonup", "↼"},
            {"leftharpoondown", "↽"},
            {"rightharpoonup", "⇀"},
            {"rightharpoondown", "⇁"},
            {"rightleftharpoons", "⇌"},
            {"leftrightharpoons", "⇋"},
            {"nearrow", "↗"},
            {"searrow", "↘"},
            {"swarrow", "↙"},
            {"nwarrow", "↖"},
            {"rightsquigarrow", "⇝"},
            {"leadsto", "⇝"},
            {"Rightarrow", "⇒"},
            {"Longrightarrow", "⇒"},
            {"Leftarrow", "⇐"},
            {"Longleftarrow", "⇐"},
            {"Leftrightarrow", "⇔"},
            {"Longleftrightarrow", "⇔"},
            {"implies", "⇒"},
            {"iff", "⇔"},
            {"mapsto", "↦"},
            {"longmapsto", "↦"},
            {"uparrow", "↑"},
            {"downarrow", "↓"},
            {"partial", "∂"},
            {"nabla", "∇"},
            {"int", "∫"},
            {"iint", "∬"},
            {"iiint", "∭"},
            {"oint", "∮"},
            {"sum", "∑"},
            {"prod", "∏"},
            {"coprod", "∐"},
            {"infty", "∞"},
            {"emptyset", "∅"},
            {"varnothing", "∅"},
            {"angle", "∠"},
            {"therefore", "∴"},
            {"because", "∵"},
            {"aleph", "ℵ"},
            {"beth", "ℶ"},
            {"gimel", "ℷ"},
            {"daleth", "ℸ"},
            {"top", "⊤"},
            {"bot", "⊥"},
            {"triangle", "△"},
            {"square", "□"},
            {"lozenge", "◊"},
            {"checkmark", "✓"},
            {"complement", "∁"},
            {"wp", "℘"},
            {"prime", "′"},
            {"ldots", "…"},
            {"dots", "…"},
            {"cdots", "⋯"},
            {"vdots", "⋮"},
            {"ddots", "⋱"},
            {"ell", "ℓ"},
            {"hbar", "ℏ"},
            {"Im", "ℑ"},
            {"Re", "ℜ"},
            {"langle", "⟨"},
            {"rangle", "⟩"},
            {"vert", "|"},
            {"lvert", "|"},
            {"rvert", "|"},
            {"Vert", "‖"},
            {"lVert", "‖"},
            {"rVert", "‖"},
            {"lbrace", "{"},
            {"rbrace", "}"},
            {"backslash", "\\"},
            {"lfloor", "⌊"},
            {"rfloor", "⌋"},
            {"lceil", "⌈"},
            {"rceil", "⌉"},
            {"colon", ":"},
    };
    return table;
}

[[nodiscard]] const CommandSet& named_operators() {
    static const CommandSet set{
            "arccos",
            "arcsin",
            "arctan",
            "arg",
            "cos",
            "cosh",
            "cot",
            "coth",
            "csc",
            "deg",
            "det",
            "dim",
            "exp",
            "gcd",
            "hom",
            "inf",
            "ker",
            "lg",
            "lim",
            "liminf",
            "limsup",
            "ln",
            "log",
            "max",
            "min",
            "Pr",
            "sec",
            "sin",
            "sinh",
            "sup",
            "tan",
            "tanh",
    };
    return set;
}

[[nodiscard]] const CommandSet& limit_operators() {
    static const CommandSet set{
            "argmax",
            "argmin",
            "inf",
            "injlim",
            "lim",
            "liminf",
            "limsup",
            "max",
            "min",
            "projlim",
            "sup",
    };
    return set;
}

[[nodiscard]] const CommandSet& display_limit_symbols() {
    static const CommandSet set{
            "bigcap",
            "bigcup",
            "bigodot",
            "bigoplus",
            "bigotimes",
            "bigsqcup",
            "biguplus",
            "bigvee",
            "bigwedge",
            "coprod",
            "int",
            "iint",
            "iiint",
            "oint",
            "prod",
            "sum",
    };
    return set;
}

[[nodiscard]] const CommandSet& relation_commands() {
    static const CommandSet set{
            "Leftarrow",
            "Leftrightarrow",
            "Longleftarrow",
            "Longleftrightarrow",
            "Longrightarrow",
            "Rightarrow",
            "Join",
            "Vdash",
            "Vvdash",
            "approx",
            "asymp",
            "bowtie",
            "cong",
            "dashv",
            "fullouterjoin",
            "doteq",
            "downarrow",
            "equiv",
            "ge",
            "geq",
            "geqslant",
            "gets",
            "gg",
            "hookleftarrow",
            "hookrightarrow",
            "iff",
            "implies",
            "in",
            "leadsto",
            "le",
            "leftarrow",
            "leftharpoondown",
            "leftharpoonup",
            "leftrightarrow",
            "leftrightharpoons",
            "leftouterjoin",
            "leq",
            "leqslant",
            "ll",
            "longleftarrow",
            "longleftrightarrow",
            "longmapsto",
            "longrightarrow",
            "ltimes",
            "mapsto",
            "mid",
            "models",
            "ne",
            "nearrow",
            "neq",
            "ni",
            "notin",
            "nvdash",
            "nvDash",
            "nwarrow",
            "parallel",
            "perp",
            "prec",
            "preceq",
            "propto",
            "rightharpoondown",
            "rightharpoonup",
            "rightleftharpoons",
            "rightouterjoin",
            "rightarrow",
            "rightsquigarrow",
            "rtimes",
            "searrow",
            "sim",
            "simeq",
            "sqsubset",
            "sqsubseteq",
            "sqsupset",
            "sqsupseteq",
            "subset",
            "subseteq",
            "succ",
            "succeq",
            "supset",
            "supseteq",
            "swarrow",
            "to",
            "triangleleft",
            "triangleright",
            "twoheadleftarrow",
            "twoheadrightarrow",
            "uparrow",
            "vdash",
    };
    return set;
}

[[nodiscard]] const SymbolTable& negated_symbols() {
    static const SymbolTable table{
            {"<", "≮"},
            {">", "≯"},
            {"=", "≠"},
            {"∈", "∉"},
            {"∋", "∌"},
            {"∣", "∤"},
            {"∥", "∦"},
            {"∼", "≁"},
            {"≃", "≄"},
            {"≅", "≇"},
            {"≈", "≉"},
            {"≡", "≢"},
            {"≤", "≰"},
            {"≥", "≱"},
            {"≺", "⊀"},
            {"≻", "⊁"},
            {"⊂", "⊄"},
            {"⊃", "⊅"},
            {"⊆", "⊈"},
            {"⊇", "⊉"},
            {"⊢", "⊬"},
            {"⊨", "⊭"},
            {"↔", "↮"},
            {"←", "↚"},
            {"→", "↛"},
            {"⇒", "⇏"},
            {"⇐", "⇍"},
            {"⇔", "⇎"},
            {"≼", "⋠"},
            {"≽", "⋡"},
    };
    return table;
}

[[nodiscard]] const SymbolTable& blackboard() {
    static const SymbolTable table{
            {"C", "ℂ"},
            {"H", "ℍ"},
            {"N", "ℕ"},
            {"P", "ℙ"},
            {"Q", "ℚ"},
            {"R", "ℝ"},
            {"Z", "ℤ"},
    };
    return table;
}

[[nodiscard]] const SymbolTable& superscripts() {
    static const SymbolTable table{
            {"0", "⁰"},
            {"1", "¹"},
            {"2", "²"},
            {"3", "³"},
            {"4", "⁴"},
            {"5", "⁵"},
            {"6", "⁶"},
            {"7", "⁷"},
            {"8", "⁸"},
            {"9", "⁹"},
            {"+", "⁺"},
            {"-", "⁻"},
            {"=", "⁼"},
            {"(", "⁽"},
            {")", "⁾"},
            {"a", "ᵃ"},
            {"b", "ᵇ"},
            {"c", "ᶜ"},
            {"d", "ᵈ"},
            {"e", "ᵉ"},
            {"f", "ᶠ"},
            {"g", "ᵍ"},
            {"h", "ʰ"},
            {"i", "ⁱ"},
            {"j", "ʲ"},
            {"k", "ᵏ"},
            {"l", "ˡ"},
            {"m", "ᵐ"},
            {"n", "ⁿ"},
            {"o", "ᵒ"},
            {"p", "ᵖ"},
            {"r", "ʳ"},
            {"s", "ˢ"},
            {"t", "ᵗ"},
            {"u", "ᵘ"},
            {"v", "ᵛ"},
            {"w", "ʷ"},
            {"x", "ˣ"},
            {"y", "ʸ"},
            {"z", "ᶻ"},
    };
    return table;
}

[[nodiscard]] const SymbolTable& subscripts() {
    static const SymbolTable table{
            {"0", "₀"},
            {"1", "₁"},
            {"2", "₂"},
            {"3", "₃"},
            {"4", "₄"},
            {"5", "₅"},
            {"6", "₆"},
            {"7", "₇"},
            {"8", "₈"},
            {"9", "₉"},
            {"+", "₊"},
            {"-", "₋"},
            {"=", "₌"},
            {"(", "₍"},
            {")", "₎"},
            {"a", "ₐ"},
            {"e", "ₑ"},
            {"h", "ₕ"},
            {"i", "ᵢ"},
            {"j", "ⱼ"},
            {"k", "ₖ"},
            {"l", "ₗ"},
            {"m", "ₘ"},
            {"n", "ₙ"},
            {"o", "ₒ"},
            {"p", "ₚ"},
            {"r", "ᵣ"},
            {"s", "ₛ"},
            {"t", "ₜ"},
            {"u", "ᵤ"},
            {"v", "ᵥ"},
            {"x", "ₓ"},
    };
    return table;
}

[[nodiscard]] const CommandSet& spacing_commands() {
    static const CommandSet set{
            ",",
            ":",
            ";",
            " ",
            ">",
            "enspace",
            "enskip",
            "medspace",
            "quad",
            "qquad",
            "thickspace",
            "thinspace",
    };
    return set;
}

[[nodiscard]] const CommandSet& negative_spacing_commands() {
    static const CommandSet set{"!", "negmedspace", "negthickspace", "negthinspace"};
    return set;
}

[[nodiscard]] const CommandSet& font_switch_commands() {
    static const CommandSet set{"bf", "cal", "it", "rm", "sf", "sl", "tt"};
    return set;
}

[[nodiscard]] const CommandSet& ignored_commands() {
    static const CommandSet set{
            "displaystyle",
            "limits",
            "nolimits",
            "scriptstyle",
            "scriptscriptstyle",
            "textstyle",
    };
    return set;
}

[[nodiscard]] const CommandSet& size_commands() {
    static const CommandSet set{
            "big",
            "Big",
            "bigg",
            "Bigg",
            "bigl",
            "Bigl",
            "biggl",
            "Biggl",
            "bigr",
            "Bigr",
            "biggr",
            "Biggr",
    };
    return set;
}

[[nodiscard]] const CommandSet& plain_wrappers() {
    static const CommandSet set{
            "emph",
            "mathcal",
            "mathbf",
            "mathfrak",
            "mathit",
            "mathrm",
            "mathnormal",
            "mathscr",
            "mathsf",
            "mathtt",
            "mathup",
            "mbox",
            "overbrace",
            "pmb",
            "smash",
            "substack",
            "text",
            "textbf",
            "textit",
            "textmd",
            "textnormal",
            "textrm",
            "textsc",
            "textsf",
            "textsl",
            "texttt",
            "textup",
            "underbrace",
            "bm",
            "boldsymbol",
    };
    return set;
}

[[nodiscard]] const SymbolTable& accents() {
    static const SymbolTable table{
            {"acute", "\xcc\x81"},
            {"bar", "\xcc\x85"},
            {"breve", "\xcc\x86"},
            {"check", "\xcc\x8c"},
            {"ddot", "\xcc\x88"},
            {"dot", "\xcc\x87"},
            {"grave", "\xcc\x80"},
            {"hat", "\xcc\x82"},
            {"mathring", "\xcc\x8a"},
            {"overleftarrow", "\xe2\x83\x96"},
            {"overleftrightarrow", "\xe2\x83\xa1"},
            {"overline", "\xcc\x85"},
            {"overrightarrow", "\xe2\x83\x97"},
            {"tilde", "\xcc\x83"},
            {"underline", "\xcc\xb2"},
            {"vec", "\xe2\x83\x97"},
            {"widehat", "\xcc\x82"},
            {"widetilde", "\xcc\x83"},
    };
    return table;
}

/// pi's private markers: U+F0004/U+F0005 bracket a named operator so that the
/// final normalization pass inserts the surrounding spaces in one place.
constexpr std::string_view kNamedOperatorStart{"\xef\x80\x84"};
constexpr std::string_view kNamedOperatorEnd{"\xef\x80\x85"};
/// The sentinel a negative spacing command returns instead of a space. The
/// length is explicit: a lone NUL literal would otherwise terminate the view.
constexpr std::string_view kNegativeSpace{"\x00", 1};

[[nodiscard]] bool is_ascii_space(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\v' || character == '\f' ||
           character == '\r';
}

[[nodiscard]] bool is_ascii_letter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool is_space_or_tab(char character) { return character == ' ' || character == '\t'; }

[[nodiscard]] std::string_view trim(std::string_view value) {
    std::size_t start = 0;
    std::size_t end = value.size();
    while (start < end && is_ascii_space(value[start]))
        ++start;
    while (end > start && is_ascii_space(value[end - 1]))
        --end;
    return value.substr(start, end - start);
}

/// One decoded code point and the byte length it occupies.
struct CodePoint {
    char32_t value{0};
    std::size_t length{0};
};

[[nodiscard]] CodePoint next_code_point(std::string_view text, std::size_t position) {
    const auto [codepoint, length] = detail::decode_utf8(text, position);
    return CodePoint{codepoint, length};
}

[[nodiscard]] bool is_unicode_letter(char32_t codepoint) {
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(codepoint))) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LL:
    case UTF8PROC_CATEGORY_LT:
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_unicode_number(char32_t codepoint) {
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(codepoint))) {
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
        return true;
    default:
        return false;
    }
}

/// Every code point is a letter, a number or a decimal point; empty is false.
/// `allow_letter` drops the letter class for pi's `/^[\p{N}.]+$/u`.
[[nodiscard]] bool is_char_class_word(std::string_view value, bool allow_letter) {
    if (value.empty()) return false;
    std::size_t position = 0;
    while (position < value.size()) {
        const CodePoint point = next_code_point(value, position);
        const bool accepted = point.value == '.' || is_unicode_number(point.value) ||
                              (allow_letter && is_unicode_letter(point.value));
        if (!accepted) return false;
        position += point.length;
    }
    return true;
}

/// pi's `/^[A-Za-z]+$/u`.
[[nodiscard]] bool is_ascii_word(std::string_view value) {
    if (value.empty()) return false;
    return std::ranges::all_of(value, is_ascii_letter);
}

[[nodiscard]] std::string_view take_code_point(std::string_view value, std::size_t& position) {
    const CodePoint point = next_code_point(value, position);
    const std::string_view taken = value.substr(position, point.length);
    position += point.length;
    return taken;
}

[[nodiscard]] std::size_t count_code_points(std::string_view value) {
    std::size_t position = 0;
    std::size_t count = 0;
    while (position < value.size()) {
        position += next_code_point(value, position).length;
        ++count;
    }
    return count;
}

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] std::string replace_all(std::string_view value, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string{value};
    std::string result;
    std::size_t position = 0;
    while (position < value.size()) {
        if (starts_with(value.substr(position), from)) {
            result += to;
            position += from.size();
            continue;
        }
        result += value[position];
        ++position;
    }
    return result;
}

/// pi's `String.prototype.trim` followed by `/\s*([=+-])\s*/g` -> `$1`.
[[nodiscard]] std::string normalize_script_value(std::string_view value) {
    static const std::regex pattern(R"(\s*([=+-])\s*)");
    return std::regex_replace(std::string{trim(value)}, pattern, "$1");
}

enum class ScriptKind {
    Sub,
    Sup,
};

[[nodiscard]] std::optional<std::string> format_unicode_script(std::string_view value, ScriptKind kind) {
    const SymbolTable& table = kind == ScriptKind::Sub ? subscripts() : superscripts();
    std::string result;
    std::size_t position = 0;
    while (position < value.size()) {
        const std::string_view taken = take_code_point(value, position);
        const auto found = table.find(taken);
        if (found == std::end(table)) return std::nullopt;
        result += found->second;
    }
    return result;
}

[[nodiscard]] std::string format_script(std::string_view value, ScriptKind kind) {
    const std::string normalized = normalize_script_value(value);
    if (const auto unicode = format_unicode_script(normalized, kind)) return *unicode;
    const std::string_view prefix = kind == ScriptKind::Sub ? "_" : "^";
    if (count_code_points(normalized) == 1 || (kind == ScriptKind::Sub && is_ascii_word(normalized))) {
        return std::string(prefix) + normalized;
    }
    return std::string(prefix) + "(" + normalized + ")";
}

[[nodiscard]] std::string format_fraction(std::string_view numerator, std::string_view denominator) {
    const std::string_view trimmed_numerator = trim(numerator);
    const std::string_view trimmed_denominator = trim(denominator);
    const std::string simple_numerator = is_char_class_word(trimmed_numerator, true)
                                                 ? std::string{trimmed_numerator}
                                                 : "(" + std::string{trimmed_numerator} + ")";
    const std::string simple_denominator =
            is_char_class_word(trimmed_denominator, false) || count_code_points(trimmed_denominator) == 1
                    ? std::string{trimmed_denominator}
                    : "(" + std::string{trimmed_denominator} + ")";
    return simple_numerator + "/" + simple_denominator;
}

[[nodiscard]] std::string format_root(std::string_view value, std::string_view symbol) {
    const std::string_view trimmed = trim(value);
    return is_char_class_word(trimmed, true) ? std::string{symbol} + std::string{trimmed}
                                             : std::string{symbol} + "(" + std::string{trimmed} + ")";
}

/// Collapse every run of spaces and tabs to one, then trim the line.
[[nodiscard]] std::string collapse_spaces(const std::string& line) {
    static const std::regex pattern(R"([ \t]+)");
    const std::string collapsed = std::regex_replace(line, pattern, " ");
    return std::string{trim(collapsed)};
}

[[nodiscard]] bool is_named_operator_spacing_predecessor(char32_t codepoint) {
    if (is_unicode_letter(codepoint) || is_unicode_number(codepoint)) return true;
    return codepoint == U')' || codepoint == U']' || codepoint == U'}';
}

[[nodiscard]] bool is_named_operator_spacing_successor(char32_t codepoint) {
    if (is_unicode_letter(codepoint) || is_unicode_number(codepoint)) return true;
    return codepoint == U'√';
}

/// pi's `normalizeOutput`: drop the private markers while inserting the spaces
/// they stand for, then collapse and trim every line.
[[nodiscard]] std::string normalize_output(std::string_view value) {
    std::string marker_free;
    std::size_t position = 0;
    std::optional<char32_t> previous;
    while (position < value.size()) {
        const CodePoint point = next_code_point(value, position);
        position += point.length;
        const std::string_view encoded = value.substr(position - point.length, point.length);
        // pi decides both spacings against the unmodified string, so the look
        // behind always sees the marker that preceded the position.
        if (encoded == kNamedOperatorStart) {
            if (previous && is_named_operator_spacing_predecessor(*previous)) marker_free += ' ';
            previous = point.value;
            continue;
        }
        if (encoded == kNamedOperatorEnd) {
            // pi removes the start markers before deciding this spacing, so the
            // lookahead skips them and stops at the first surviving code point.
            std::size_t lookahead = position;
            CodePoint following{0, 0};
            bool have_following = false;
            while (lookahead < value.size()) {
                following = next_code_point(value, lookahead);
                if (value.substr(lookahead, following.length) != kNamedOperatorStart) {
                    have_following = true;
                    break;
                }
                lookahead += following.length;
            }
            if (have_following && is_named_operator_spacing_successor(following.value)) {
                marker_free += ' ';
            }
            previous = point.value;
            continue;
        }
        marker_free += encoded;
        previous = point.value;
    }
    std::vector<std::string> lines;
    std::size_t line_start = 0;
    while (line_start <= marker_free.size()) {
        const std::size_t line_end = marker_free.find('\n', line_start);
        const std::string line = line_end == std::string::npos ? marker_free.substr(line_start)
                                                               : marker_free.substr(line_start, line_end - line_start);
        lines.push_back(collapse_spaces(line));
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
    }
    std::string joined;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].empty() && !(index > 0 && index + 1 < lines.size())) continue;
        if (!joined.empty()) joined += '\n';
        joined += lines[index];
    }
    return std::string{trim(joined)};
}

enum class InlineLowerStyle {
    Bracket,
    Script,
};

/// pi's `LatexParser`. Per-instance parse state; the immutable grammar tables
/// above are the only shared data.
class LatexParser final {
public:
    explicit LatexParser(std::string_view source) : source_(source) {}

    [[nodiscard]] std::optional<std::string> render() {
        std::string rendered = parse_sequence('\0');
        if (!supported_ || position_ != source_.size()) return std::nullopt;
        return normalize_output(rendered);
    }

private:
    [[nodiscard]] std::string parse_sequence(char end_character) {
        std::string result;
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (end_character != '\0' && character == end_character) {
                ++position_;
                return result;
            }
            if (character == '}') {
                supported_ = false;
                return result;
            }
            if (character == '{') {
                ++position_;
                result += parse_sequence('}');
                continue;
            }
            if (character == '\\') {
                const std::string command = parse_command();
                if (command == kNegativeSpace) {
                    result = std::string{trim_end(result)};
                    if (ends_with(result, kNamedOperatorEnd)) result.resize(result.size() - kNamedOperatorEnd.size());
                } else {
                    result += command;
                }
                continue;
            }
            if (character == '^' || character == '_') {
                ++position_;
                result = std::string{trim_end(result)};
                const std::string script = parse_scripts(character);
                if (ends_with(result, kNamedOperatorEnd)) {
                    result.resize(result.size() - kNamedOperatorEnd.size());
                    result += script;
                    result += kNamedOperatorEnd;
                } else {
                    result += script;
                }
                continue;
            }
            if (is_ascii_space(character)) {
                result += parse_whitespace();
                continue;
            }
            if (character == '=' || character == '<' || character == '>') {
                result = std::string{trim_end(result)};
                result += ' ';
                result += character;
                result += ' ';
                ++position_;
                continue;
            }
            if (character == '&') {
                ++position_;
                continue;
            }
            if (character == '~') {
                ++position_;
                result += ' ';
                continue;
            }
            result += take_code_point(source_, position_);
        }
        if (end_character != '\0') supported_ = false;
        return result;
    }

    [[nodiscard]] std::string parse_scripts(char initial_marker) {
        std::optional<std::string> sub;
        std::optional<std::string> sup;
        std::vector<char> order;
        const auto parse = [&](char marker) {
            (marker == '_' ? sub : sup) = parse_required_argument();
            order.push_back(marker);
        };
        parse(initial_marker);
        std::size_t next_position = position_;
        while (next_position < source_.size() && is_ascii_space(source_[next_position]))
            ++next_position;
        const char next_marker = next_position < source_.size() ? source_[next_position] : '\0';
        if ((next_marker == '^' || next_marker == '_') && next_marker != initial_marker) {
            position_ = next_position + 1;
            parse(next_marker);
        }
        const auto sub_unicode = sub ? format_unicode_script(*sub, ScriptKind::Sub) : std::nullopt;
        const auto sup_unicode = sup ? format_unicode_script(*sup, ScriptKind::Sup) : std::nullopt;
        std::string result;
        for (const char kind : order) {
            if (kind == '_') {
                result += sub_unicode ? *sub_unicode : format_script(sub.value_or(std::string{}), ScriptKind::Sub);
            } else {
                result += sup_unicode ? *sup_unicode : format_script(sup.value_or(std::string{}), ScriptKind::Sup);
            }
        }
        return result;
    }

    [[nodiscard]] std::string parse_whitespace() {
        while (position_ < source_.size() && is_ascii_space(source_[position_]))
            ++position_;
        return " ";
    }

    [[nodiscard]] std::string parse_command() {
        ++position_;
        if (position_ >= source_.size()) {
            supported_ = false;
            return {};
        }
        std::string command;
        const char first = source_[position_];
        if (first == '\n' || first == '\r') {
            ++position_;
            if (first == '\r' && position_ < source_.size() && source_[position_] == '\n') ++position_;
            return " ";
        }
        if (is_ascii_letter(first)) {
            const std::size_t start = position_;
            while (position_ < source_.size() && is_ascii_letter(source_[position_]))
                ++position_;
            command = std::string{source_.substr(start, position_ - start)};
        } else {
            command = std::string{take_code_point(source_, position_)};
        }
        if (command == "\\") return "\n";
        if (contains(spacing_commands(), command)) return " ";
        if (contains(negative_spacing_commands(), command)) return std::string{kNegativeSpace};
        if (contains(font_switch_commands(), command)) {
            while (position_ < source_.size() && is_ascii_space(source_[position_]))
                ++position_;
            return {};
        }
        if (contains(ignored_commands(), command)) return {};
        if (command == "{" || command == "}" || command == "$" || command == "%" || command == "#" || command == "_" ||
                command == "&") {
            return command;
        }
        if (command == "|") return "‖";
        if (command == "not") return parse_negation();
        if (contains(limit_operators(), command)) {
            return parse_operator(command, InlineLowerStyle::Bracket, true);
        }
        if (const auto found = symbols().find(command); found != std::end(symbols())) {
            if (contains(display_limit_symbols(), command)) {
                return parse_operator(std::string{found->second}, InlineLowerStyle::Script, false);
            }
            const bool spaced = command == "cdot" || command == "times" || contains(relation_commands(), command);
            return spaced ? " " + std::string{found->second} + " " : std::string{found->second};
        }
        if (contains(named_operators(), command)) {
            return std::string{kNamedOperatorStart} + command + std::string{kNamedOperatorEnd};
        }
        if (contains(size_commands(), command)) return {};
        if (command == "left" || command == "middle" || command == "right") {
            if (position_ < source_.size() && source_[position_] == '.') ++position_;
            return {};
        }
        if (command == "frac" || command == "dfrac" || command == "tfrac") {
            const std::string numerator = parse_required_argument();
            const std::string denominator = parse_required_argument();
            return format_fraction(numerator, denominator);
        }
        if (command == "sqrt") return parse_sqrt();
        if (command == "boxed" || command == "fbox") {
            return "[" + std::string{trim(parse_required_argument())} + "]";
        }
        if (command == "binom" || command == "dbinom" || command == "tbinom") {
            const std::string upper = parse_required_argument();
            const std::string lower = parse_required_argument();
            return "(" + upper + " choose " + lower + ")";
        }
        if (const auto accent = accents().find(command); accent != std::end(accents())) {
            const std::string value = parse_required_argument();
            return count_code_points(value) == 1 ? value + std::string{accent->second} : command + "(" + value + ")";
        }
        if (command == "mathbb") return parse_blackboard();
        if (command == "operatorname") return parse_operatorname();
        if (command == "mod" || command == "bmod") return " mod ";
        if (command == "pmod" || command == "pod") {
            const std::string value{trim(parse_required_argument())};
            return command == "pmod" ? " (mod " + value + ")" : " (" + value + ")";
        }
        if (command == "overset" || command == "stackrel") {
            const std::string upper = parse_required_argument();
            const std::string base{trim(parse_required_argument())};
            return base + format_script(upper, ScriptKind::Sup);
        }
        if (command == "underset") {
            const std::string lower = parse_required_argument();
            const std::string base{trim(parse_required_argument())};
            return base + format_script(lower, ScriptKind::Sub);
        }
        if (contains(plain_wrappers(), command)) {
            const std::string value = parse_required_argument();
            return starts_with(command, "text") || command == "mbox" ? value : std::string{trim(value)};
        }
        if (command == "begin") return parse_environment();
        if (command == "end") {
            supported_ = false;
            return {};
        }
        supported_ = false;
        return "\\" + command;
    }

    [[nodiscard]] std::string parse_negation() {
        const std::string value{trim(parse_required_argument())};
        if (const auto negated = negated_symbols().find(value); negated != std::end(negated_symbols())) {
            return " " + std::string{negated->second} + " ";
        }
        if (value.empty()) {
            supported_ = false;
            return {};
        }
        std::size_t position = 0;
        const std::string first{take_code_point(value, position)};
        return " " + first + "\xcc\xb8" + value.substr(position) + " ";
    }

    [[nodiscard]] std::string parse_blackboard() {
        const std::string value = parse_required_argument();
        std::string result;
        std::size_t position = 0;
        while (position < value.size()) {
            const std::string_view taken = take_code_point(value, position);
            const auto found = blackboard().find(taken);
            result += found != std::end(blackboard()) ? std::string{found->second} : std::string{taken};
        }
        return result;
    }

    [[nodiscard]] std::string parse_operatorname() {
        const bool starred = position_ < source_.size() && source_[position_] == '*';
        if (starred) ++position_;
        const std::string operator_name{trim(normalize_output(parse_required_argument()))};
        return parse_operator(operator_name, InlineLowerStyle::Bracket, true);
    }

    [[nodiscard]] std::string parse_sqrt() {
        const auto degree = parse_optional_argument();
        const std::string value = parse_required_argument();
        const std::string_view trimmed_degree = degree ? trim(*degree) : std::string_view{};
        if (!degree || trimmed_degree == "2") return format_root(value, "√");
        if (trimmed_degree == "3") return format_root(value, "∛");
        if (trimmed_degree == "4") return format_root(value, "∜");
        return format_script(trimmed_degree, ScriptKind::Sup) + format_root(value, "√");
    }

    [[nodiscard]] std::string parse_operator(std::string operator_name, InlineLowerStyle lower_style, bool spaced) {
        consume_limits_modifier();
        std::optional<std::string> lower;
        std::optional<std::string> upper;
        while (true) {
            std::size_t script_position = position_;
            while (script_position < source_.size() && is_space_or_tab(source_[script_position]))
                ++script_position;
            const char kind = script_position < source_.size() ? source_[script_position] : '\0';
            if (kind != '_' && kind != '^') break;
            position_ = script_position + 1;
            const std::string rendered = normalize_output(parse_required_argument());
            const std::string value{replace_all(rendered, " ", "")};
            if (kind == '_') {
                if (lower) supported_ = false;
                lower = value;
            } else {
                if (upper) supported_ = false;
                upper = value;
            }
        }
        std::string rendered = std::move(operator_name);
        if (lower) {
            rendered += lower_style == InlineLowerStyle::Bracket ? "[" + *lower + "]"
                                                                 : format_script(*lower, ScriptKind::Sub);
        }
        if (upper) rendered += format_script(*upper, ScriptKind::Sup);
        return spaced ? " " + rendered + " " : rendered;
    }

    /// pi consumes a `\limits`/`\nolimits` modifier where it stands; inline
    /// rendering never stacks limits, so only the consumed span is observable.
    void consume_limits_modifier() {
        std::size_t modifier_position = position_;
        while (modifier_position < source_.size() && is_space_or_tab(source_[modifier_position])) {
            ++modifier_position;
        }
        const std::string_view tail = source_.substr(modifier_position);
        std::string_view modifier;
        for (const std::string_view candidate : {std::string_view{"\\limits"}, std::string_view{"\\nolimits"}}) {
            if (!starts_with(tail, candidate)) continue;
            const char following = tail.size() > candidate.size() ? tail[candidate.size()] : '\0';
            if (is_ascii_letter(following)) continue;
            modifier = candidate;
            break;
        }
        if (!modifier.empty()) position_ = modifier_position + modifier.size();
    }

    [[nodiscard]] std::string parse_required_argument() {
        while (position_ < source_.size() && is_ascii_space(source_[position_]))
            ++position_;
        if (position_ >= source_.size()) {
            supported_ = false;
            return {};
        }
        if (source_[position_] == '{') {
            ++position_;
            return parse_sequence('}');
        }
        if (source_[position_] == '\\') return parse_command();
        return std::string{take_code_point(source_, position_)};
    }

    [[nodiscard]] std::optional<std::string> parse_optional_argument() {
        while (position_ < source_.size() && is_space_or_tab(source_[position_]))
            ++position_;
        if (position_ >= source_.size() || source_[position_] != '[') return std::nullopt;
        const std::size_t end = source_.find(']', position_ + 1);
        if (end == std::string_view::npos) {
            supported_ = false;
            return std::nullopt;
        }
        const std::string value{std::string{source_.substr(position_ + 1, end - position_ - 1)}};
        position_ = end + 1;
        return render_nested(value);
    }

    [[nodiscard]] std::optional<std::string> read_raw_group() {
        while (position_ < source_.size() && is_space_or_tab(source_[position_]))
            ++position_;
        if (position_ >= source_.size() || source_[position_] != '{') {
            supported_ = false;
            return std::nullopt;
        }
        const std::size_t start = ++position_;
        int depth = 1;
        while (position_ < source_.size()) {
            const char character = source_[position_];
            if (character == '\\') {
                position_ += 2;
                continue;
            }
            if (character == '{') ++depth;
            if (character == '}') --depth;
            if (depth == 0) {
                const std::string value{std::string{source_.substr(start, position_ - start)}};
                ++position_;
                return value;
            }
            ++position_;
        }
        supported_ = false;
        return std::nullopt;
    }

    [[nodiscard]] std::string parse_environment() {
        const auto environment = read_raw_group();
        if (!environment) return {};
        const std::string end_marker = "\\end{" + *environment + "}";
        const std::size_t end = source_.find(end_marker, position_);
        if (end == std::string_view::npos) {
            supported_ = false;
            return {};
        }
        const std::string body{std::string{source_.substr(position_, end - position_)}};
        position_ = end + end_marker.size();
        if (*environment == "equation" || *environment == "equation*" || *environment == "displaymath") {
            return std::string{trim(render_nested(body))};
        }
        // Every remaining pi environment (aligned families, cases and the matrix
        // families) renders through the display layout stage owned by #975.
        supported_ = false;
        return body;
    }

    [[nodiscard]] std::string render_nested(std::string_view source) {
        auto rendered = LatexParser(source).render();
        if (!rendered) {
            supported_ = false;
            return std::string{source};
        }
        return *rendered;
    }

    static bool contains(const CommandSet& set, std::string_view command) { return set.find(command) != std::end(set); }

    static bool ends_with(const std::string& value, std::string_view suffix) {
        return value.size() >= suffix.size() && std::string_view{value}.substr(value.size() - suffix.size()) == suffix;
    }

    static std::string_view trim_end(const std::string& value) {
        std::size_t end = value.size();
        while (end > 0 && is_ascii_space(value[end - 1]))
            --end;
        return std::string_view{value}.substr(0, end);
    }

    std::string_view source_;
    std::size_t position_{0};
    bool supported_{true};
};

} // namespace

std::optional<std::string> render_latex(std::string_view source, const LatexOptions& options) {
    // The vertical display layout is #974 and the display environments are #975;
    // until both stages exist the option reports the frozen failure value rather
    // than a formula that silently lost its layout.
    if (options.display) return std::nullopt;
    return LatexParser(source).render();
}

} // namespace cch::tui