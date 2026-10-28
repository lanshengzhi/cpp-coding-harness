#pragma once

#include <functional>

namespace cch::durable::testing {

/// Where one view-dispatch test hook runs. `AfterClaim` runs on the
/// publication thread after a watcher's delivery is admitted and before its
/// listener is invoked; `AfterStopMarked` runs on the stopping thread after
/// `stop()` records the stop. The stop path does not wait.
enum class ViewDispatchHookPoint {
    AfterClaim,
    AfterStopMarked,
};

/// Private test-only observation hook; it is not a public contract. It is
/// null-safe when unset, runs on the publication or stopping thread, and must
/// not call back into a watch or Session. Production callers never install
/// one. A hook is copied before it runs, so clearing it does not wait for a
/// callback already in progress.
using ViewDispatchHook = std::function<void(ViewDispatchHookPoint)>;

void set_view_dispatch_hook(ViewDispatchHook hook);
void clear_view_dispatch_hook();

} // namespace cch::durable::testing
