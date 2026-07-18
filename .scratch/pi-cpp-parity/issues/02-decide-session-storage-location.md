# Decide where session files live by default

Type: grilling
Status: open
Blocked by: None

## Question

Should default session storage stay project-local (`.cpp-harness/sessions/` inside the workspace, as today), or move into the agent config directory keyed by workspace, mirroring pi's `~/.pi/agent/sessions/<encoded-workspace>/`? Project-local keeps transcripts next to the workspace for inspection and cleanup; pi's layout keeps user-level state in one root and avoids writing harness state into workspaces.
