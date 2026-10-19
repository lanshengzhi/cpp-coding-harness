#pragma once

#include "ai/ModelsImpl.hpp"
#include "support/StreamTransport.hpp"

#include <memory>
#include <utility>

namespace cch::ai::detail {

struct ModelsTestAccess final {
    [[nodiscard]] static support::ExpectedVoid replace_models(Models& models, std::vector<Model> catalog) {
        return models.replace_models(std::move(catalog));
    }

    static void set_transport(Models& models, std::shared_ptr<support::StreamTransport> transport) {
        models.impl_->transport = std::move(transport);
    }

    [[nodiscard]] static ModelStreamFactory take_stream(Models& models) { return std::move(models.impl_->test_stream); }

    static void set_stream(Models& models, ModelStreamFactory stream) { models.impl_->test_stream = std::move(stream); }
};

} // namespace cch::ai::detail
