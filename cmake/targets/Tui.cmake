include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

cch_parity_declare_target(
    TARGET cch_tui
    ROLE owner
    OWNER cch_tui
    SOURCES
        src/tui/Autocomplete.cpp
        src/tui/CancellableLoader.cpp
        src/tui/Container.cpp
        src/tui/Editor.cpp
        src/tui/EditorCompletionSession.cpp
        src/tui/Fuzzy.cpp
        src/tui/Image.cpp
        src/tui/Input.cpp
        src/tui/InputDecoder.cpp
        src/tui/Keybindings.cpp
        src/tui/Keys.cpp
        src/tui/Loader.cpp
        src/tui/Markdown.cpp
        src/tui/Overlay.cpp
        src/tui/OverlayCompositor.cpp
        src/tui/ProcessTerminal.cpp
        src/tui/SelectList.cpp
        src/tui/SettingsList.cpp
        src/tui/TerminalImage.cpp
        src/tui/Text.cpp
        src/tui/TextBuffer.cpp
        src/tui/TruncatedText.cpp
        src/tui/Tui.cpp
        src/tui/UnicodeWidth.cpp
        src/tui/Utils.cpp
        src/tui/VirtualTerminal.cpp
    DEPENDS
        cch_support
        md4c::md4c@md4c
        utf8proc::utf8proc@utf8proc
        Threads::Threads@threads
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_tui src/tui/include)
