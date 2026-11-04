#pragma once

#include <cch/durable/Environment.hpp>

#include <stop_token>
#include <string>

namespace cch::durable {

/// The bounded tree walk behind `LocalEnvironment::list_files` (issue #1158;
/// the ignore-rule engine it walks with is #1198, the discovery #1199). The
/// root is the resolved absolute path; the frozen payload ceiling clamps
/// `options.max_bytes` at listing time.
[[nodiscard]] support::Expected<TreeListing> walk_tree_listing(
        std::string root, TreeListingOptions options, std::stop_token stop);

} // namespace cch::durable
