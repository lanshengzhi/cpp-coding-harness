#pragma once

#include "../harness/session/JsonlSessionStore.hpp"

namespace cch::session {

using SessionMetadata = harness::session::SessionMetadata;
using SessionEntryKind = harness::session::SessionEntryKind;
using SessionEntry = harness::session::SessionEntry;
using LoadedSession = harness::session::LoadedSession;
using JsonlSessionStore = harness::session::JsonlSessionStore;

} // namespace cch::session
