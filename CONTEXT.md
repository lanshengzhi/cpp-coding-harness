# C++ Coding Agent Harness

This context names the concepts used to describe the harness as a product. It is a glossary, not an implementation or interface specification.

## Language

**Agent Session**:
One ongoing coding-agent conversation, including its current interaction state and durable history.
_Avoid_: Conversation handle, runtime session

**Live Session State**:
The current in-process view of an Agent Session, which may be newer than its durable history.
_Avoid_: Persisted state, session file

**Session Entry**:
One durable record in an Agent Session history.
_Avoid_: Wire payload, JSON line

**Session Event Commitment**:
The ordered policy that commits one agent lifecycle event to an Agent Session — advancing Live Session State, delivering to subscribers, and appending Session Entries — with explicit failure precedence.
_Avoid_: Event handler, callback chain

**Session Resume**:
Reopening an Agent Session from its durable history at the selected active point.
_Avoid_: Reload, replay

**Session Topology**:
The shape of an Agent Session history, such as linear, branched, or compacted.
_Avoid_: Completion state

**Project Resource**:
A project-associated skill or prompt template that may be made available to an Agent Session after policy checks.
_Avoid_: Runtime service, project file

**Project Trust**:
The user-controlled authorization decision governing whether project-authored resources may be loaded.
_Avoid_: Workspace configuration, project self-approval

**Agent Config Directory**:
The user-level root for durable harness state shared across workspaces.
_Avoid_: Config home, user profile directory

**User Settings**:
User-level preferences for providers, Project Resources, and Agent Session storage.
_Avoid_: User config, config file

**User Bash**:
A frontend operation that runs a user-entered shell command without treating it as an agent prompt.
_Avoid_: Bash tool, prompt processing
