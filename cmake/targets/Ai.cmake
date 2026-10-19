include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES
        src/ai/ContentUtil.cpp
        src/ai/InferenceFailure.cpp
        src/ai/Model.cpp
        src/ai/Models.cpp
        src/ai/MagpieCatalog.cpp
        src/ai/ModelsStream.cpp
        src/ai/ModelStream.cpp
        src/ai/SimpleOptions.cpp
        src/ai/Usage.cpp
        src/ai/api/CompletionsEvents.cpp
        src/ai/api/CompletionsPayload.cpp
        src/ai/api/MessageConversion.cpp
        src/ai/api/MessageNormalization.cpp
        src/ai/api/CompletionsPipeline.cpp
        src/ai/api/UsageNormalization.cpp
        src/ai/providers/RetryPolicy.cpp
    DEPENDS
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_ai src/ai/include)
