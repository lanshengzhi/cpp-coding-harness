include_guard(GLOBAL)

# Orchestration include: top-level CMakeLists.txt only (relies on CMAKE_CURRENT_SOURCE_DIR = repo root).

cch_parity_declare_target(
    TARGET cch_ai
    ROLE owner
    OWNER cch_ai
    SOURCES
        src/ai/BuiltinProviders.cpp
        src/ai/ContentUtil.cpp
        src/ai/Model.cpp
        src/ai/Models.cpp
        src/ai/ModelStream.cpp
        src/ai/SimpleOptions.cpp
        src/ai/Usage.cpp
        src/ai/api/AnthropicMessagesAdapter.cpp
        src/ai/api/MessageConversion.cpp
        src/ai/api/OpenAICodexResponsesAdapter.cpp
        src/ai/api/OpenAIResponsesAdapter.cpp
        src/ai/api/ResponsesEventProcessor.cpp
        src/ai/api/Termination.cpp
        src/ai/api/UsageNormalization.cpp
        src/ai/auth/OAuthCallbackServer.cpp
        src/ai/auth/OAuthHttpClient.cpp
        src/ai/auth/OpenAICodexOAuth.cpp
        src/ai/auth/KimiCodingOAuth.cpp
        src/ai/auth/OauthPage.cpp
        src/ai/auth/Pkce.cpp
        src/ai/providers/BoostBeastStreamTransport.cpp
        src/ai/providers/BoostBeastWebSocketTransport.cpp
        src/ai/providers/CodexCatalog.cpp
        src/ai/providers/ComposedProvider.cpp
        src/ai/providers/EnvApiKeyAuth.cpp
        src/ai/providers/FakeProvider.cpp
        src/ai/providers/KimiCatalog.cpp
        src/ai/providers/RetryPolicy.cpp
        src/ai/providers/SseParser.cpp
        src/ai/providers/StreamExecutionEngine.cpp
        src/ai/utils/RetryClassifier.cpp
    DEPENDS
        cch_support
        Boost::headers@boost
        Threads::Threads@threads
        glaze::glaze@glaze
        OpenSSL::SSL@openssl
        OpenSSL::Crypto@openssl
    INTERFACE_DEPENDS
        cch_support
)
cch_owner_include_roots(cch_ai src/ai/include)
