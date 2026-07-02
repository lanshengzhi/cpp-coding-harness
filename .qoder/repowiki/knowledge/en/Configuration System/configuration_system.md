The C++ Coding Agent Harness uses a layered configuration system that combines **CLI arguments**, **JSON configuration files**, and **environment variables** to manage runtime settings, provider credentials, and project-specific resources.

### 1. Configuration Layers & Priority
The system resolves settings using a strict priority order:
1.  **CLI Arguments**: Explicit flags (e.g., `--model`, `--base-url`) provided at runtime take the highest precedence.
2.  **Session State**: For resumed sessions (`--resume`), stored metadata (provider/model) from the JSONL session file is used if CLI overrides are absent.
3.  **User Configuration File**: A global JSON file located at `$HOME/.cpp-harness/config.json` provides default values for provider settings and API key environment variable chains.
4.  **Built-in Defaults**: Hardcoded defaults (e.g., `gpt-4.1-mini` model, `https://api.openai.com` base URL, `OPENAI_API_KEY` env var) are used as a fallback.

### 2. Key Components
-   **`ConfigLoader`** (`src/coding_agent/ConfigLoader.cpp`): Responsible for loading and parsing the user's `config.json`. It supports forward-compatible JSON parsing, ignoring unknown keys.
-   **`ProviderConfigResolution`** (`src/coding_agent/ProviderConfigResolution.cpp`): Implements the resolution logic for provider settings (model, base URL, API key env vars) by merging CLI overrides, config file data, and defaults.
-   **`CliParse`** (`src/cli/CliParse.cpp`): Uses the **CLI11** library to parse command-line arguments into a `CliConfig` struct, which includes `CliProviderOverrides`.
-   **`SessionFactory`** (`src/coding_agent/runtime/SessionFactory.cpp`): Orchestrates the creation of agent sessions, invoking the resolution logic and validating environment variables (e.g., ensuring API keys are present).

### 3. Environment Variable Management
-   **API Keys**: The system does not store secrets in files. Instead, it uses an "env var chain" mechanism. The `api_key_env` field in `config.json` can be a string or an array of strings (e.g., `["OPENAI_API_KEY", "ALT_KEY"]`). The resolver checks these variables in order using `std::getenv` and uses the first non-empty value.
-   **Home Directory**: The `HOME` environment variable is used to locate the global config file (`$HOME/.cpp-harness/config.json`) and the project trust store (`$HOME/.cpp-harness/trust.json`).

### 4. Project-Specific Configuration
-   **Trust Store**: Project trust decisions (allowing/denying project-local skills and prompts) are managed via a `trust.json` file in the user's home directory and/or project-local `.cpp-harness/trust.json`.
-   **Resource Discovery**: The harness automatically discovers project-local resources (skills, prompt templates) in `.cpp-harness/skills` and `.cpp-harness/prompts` within the workspace, subject to trust resolution.

### 5. Build & Dependency Configuration
-   **CMake**: The project uses CMake (3.25+) with C++23 standard. 
-   **vcpkg**: Dependencies (Glaze, Boost, CLI11, OpenSSL, Catch2) are managed via `vcpkg.json` in manifest mode. `CMakePresets.json` provides standardized build configurations for Debug/Release using vcpkg or system packages.