# 01 — Decouple Runtime from JSONL persistence

**What to build:** Introduce a narrow Session Store capability between Agent Session Runtime and physical persistence without changing any user-visible CLI or SDK behavior. Persisted create, resume, prompt, event, close, and failure flows must continue to use the existing JSONL implementation while Runtime stops depending directly on that concrete store. This is the behavior-preserving prefactor that makes later in-memory operation small and safe.

**Blocked by:** None — can start immediately

**Category:** enhancement

**Status:** implemented

- [x] Runtime receives and owns a narrow Session Store capability exposing only the persistence operations and path information Runtime actually needs.
- [x] The existing JSONL store satisfies the Runtime-facing capability without publishing serialization, tree-navigation, or journal implementation details through it.
- [x] Session resume and JSONL tree/context reconstruction continue through the existing concrete session machinery rather than being widened into the Runtime interface.
- [x] New-session and resumed-session prompts preserve current incremental append ordering for completed user, assistant, and tool-result messages.
- [x] Persistence and subscriber failures preserve the existing contract: Live Session State remains available, the Agent Session stays open where currently documented, and durable entries are not rolled back.
- [x] SessionFactory remains the only production seam that constructs Agent Session Runtime.
- [x] Existing CLI and SDK session paths, results, and required target inputs remain unchanged in this prefactor.
- [x] Focused session lifecycle, SDK incremental-persistence, JSONL store, and architecture tests pass.
- [x] Module routing documents the Runtime-facing Session Store capability and keeps concrete storage details below that seam.

## Comments

- Implemented the Runtime-facing `SessionStore` append/path capability and adapted `JsonlSessionStore` without moving JSONL resume, tree, or journal behavior into the interface.
- Validation: session lifecycle, SDK live-state and incremental-persistence, JSONL session, and architecture slices passed; the complete CTest suite passed.
- Review: standards and ticket/spec reviews found no blocking issues.
