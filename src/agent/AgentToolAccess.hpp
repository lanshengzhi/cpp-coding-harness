#pragma once

#include <cch/agent/Agent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/support/Error.hpp>

#include <string>

namespace cch::agent::detail {

/// Private runtime adapter for registering one Agent Tool with an Agent that
/// already owns its registry.
///
/// The Agent takes its `ToolRegistry` by value at construction, so a host that
/// discovers a tool after the Agent exists has no handle left to mutate it
/// with. This adapter is that one handle, and it is deliberately narrow: it
/// adds a tool and reports the name through the same read models the
/// construction path populates, so the model-facing tool payload
/// (`ToolRegistry::definitions()`, re-read at the top of every turn) and the
/// reported `AgentState::active_tool_names` can never disagree.
///
/// Like `AgentMessageAccess::replace_messages`, it must be called from the
/// Agent's own serialized domain. The window it is safe in is a turn boundary
/// or an idle Agent: the turn machine reads the registry at the top of a turn
/// and resolves calls against it while one runs.
class AgentToolAccess {
public:
    /// Register `tool` under its own definition name.
    ///
    /// Re-registering a name the registry already holds is rejected, not
    /// overwritten: `ToolRegistry::add` overwrites silently, and a second
    /// publication of the same tool would otherwise replace a live tool
    /// value with an equivalent one for no reason. Idempotent rebinds are the
    /// caller's business, not a silent success here.
    ///
    /// Admitted on the Agent's own serialized domain, at a turn boundary or
    /// on an idle Agent — including the between-turn window of a live run.
    /// There is deliberately no run-state guard, because that window is
    /// exactly where a discovery or an activation must land; a Tool Call
    /// Batch is the thing that may not overlap a change of the set, and it
    /// never does, because the turn machine resolves a whole batch inside one
    /// turn.
    ///
    /// This is the **add-only** half of the loadout contract in v1 (ADR
    /// 0066): nothing removes an active tool, and `toolsRemoved` is a
    /// transcript shape with no producer in the product. Activation is sticky
    /// for the session on purpose (spec #833 story 16), and a removal
    /// operation, when one is ever needed, is a decision that arrives with
    /// its own caller and its own tests.
    [[nodiscard]] static support::ExpectedVoid add_tool(Agent& agent, Tool tool);
};

} // namespace cch::agent::detail
