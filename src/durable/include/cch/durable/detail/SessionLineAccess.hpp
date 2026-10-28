#pragma once

#include <cch/durable/Session.hpp>
#include <cch/durable/Storage.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <functional>

namespace cch::durable::detail {

/// Internal, non-public mutation-line access for the Harness (issue #1165).
///
/// A job admitted here joins the same FIFO line as `Session::read_committed`
/// and commits: it runs after every job admitted before it has settled, and
/// no job admitted after it starts before its completion callback returns.
/// Unlike the public `read_committed`, whose callback contract is committed
/// Storage state only, this seam exists so the Harness can sample its own
/// runtime state (invocation identities, scheduling flags) at the same
/// instant as the storage scan; that is why it is not public API.
///
/// Admission rejects a failed Session with the frozen failure message and a
/// closed Session with the frozen closed message. A job error rejects the
/// operation without failing the Session, the same rule as committed reads.
/// A job runs while the mutation line is held: it must not call Session
/// methods that take the line (`failed`, `closed`, `commit`, reads), and it
/// must not call `Harness::inspect` (which admits a line job and would
/// self-wait). The Session and its Storage must outlive the operation until
/// its completion callback returns.
struct SessionLineAccess {
    static support::AsyncResult<void> read_on_line(
            Session& session, std::move_only_function<support::Expected<void>(Storage&)> job);
};

} // namespace cch::durable::detail
