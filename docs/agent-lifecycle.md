# Agent Lifecycle

This document is the human-readable map of the Agent lifecycle. The canonical definitions are in [CONTEXT.md](../CONTEXT.md); this document explains how those terms nest. The detailed lifecycle and recovery decisions remain authoritative in [ADR 0014](adr/0014-follow-pi-agent-turn-lifecycle-order.md), [ADR 0027](adr/0027-keep-prompt-cancellation-with-the-admission-owner.md), and [ADR 0034](adr/0034-own-the-scoped-pi-agent-core-agent-and-agent-turn-capabilities.md).

## Boundary

The flow begins only after input has been admitted as an **Agent Prompt**. Native TUI routing may instead handle the input as a **Slash Command** or **User Bash**; neither starts a Prompt Run.

A **Prompt Run** is the outer session-level operation:

```text
Agent Prompt
└── Prompt Run
    ├── prompt preflight
    │   ├── prompt expansion
    │   ├── authentication guidance
    │   └── optional compaction
    ├── Agent Run
    │   ├── Agent Turn
    │   ├── Agent Turn ...
    │   └── Agent Run ends
    ├── optional Auto-Retry or compaction recovery
    │   └── another Agent Run through continuation
    └── Prompt Run settles
        └── Agent Session returns to idle
```

A preflight rejection may end a Prompt Run before an Agent Run starts. A normal Prompt Run may contain one Agent Run, while recovery can add more Agent Runs without creating another Prompt Run.

## Agent Turn

Each **Agent Turn** contains one **Agent Stream Flow** and the resulting assistant outcome. The outcome either has no Tool Call Batch or produces one Tool Call Batch followed by its Tool Executions.

```text
Agent Turn
├── Agent Stream Flow
├── assistant outcome
├── optional Tool Call Batch
│   ├── Tool Execution
│   └── Tool Execution ...
└── decision whether another Agent Turn is needed
```

- A direct provider answer is a **tool-free final Agent Turn**.
- A response requesting one or more tools is a **tool-calling Agent Turn**.
- Multiple tool calls in one assistant response are one **Tool Call Batch**, not multiple Agent Turns.
- Tool results may cause another Agent Turn. Multiple such turns are an Agent Run with multiple Agent Turns.
- A tool-calling Agent Turn can still be final when the termination policy prevents another model request.

## Recovery and settlement

**Auto-Retry** and compaction recovery belong to the same Prompt Run as the original Agent Prompt. When either recovery path continues the Agent, it starts another Agent Run; it does not start a new user-facing Prompt Run.

`AgentStartEvent` and `AgentEndEvent` delimit an Agent Run. `TurnStartEvent` and `TurnEndEvent` delimit an Agent Turn. `AgentEndEvent` ends that Agent Run, but the surrounding Prompt Run may still perform recovery, persistence, or settlement work before the Agent Session becomes idle.

## Everyday examples

These are representative traces, not fixed Provider transcripts. A Provider may choose a different valid tool sequence; the lifecycle boundaries remain the same.

### 1. A direct answer

```text
User:
“I only have 30 minutes. Please make this message more polite:
‘The meeting is at 3pm tomorrow. Don’t be late.’”

Prompt Run
└── Agent Run
    └── tool-free final Agent Turn

Assistant:
“The meeting has been moved to 3pm tomorrow. Please arrive on time.”
```

No workspace information is needed, so the Agent does not use a Tool.

### 2. Read a plan and give advice

```text
User:
“I wrote my weekend trip plan in trip.md. Please check whether any day
is too busy and suggest one change.”

Prompt Run
└── Agent Run
    ├── Agent Turn
    │   └── Tool Execution: read(path="trip.md")
    │       Result: Saturday contains a museum visit, a hike, and a
    │       restaurant booking within four hours.
    └── final Agent Turn

Assistant:
“Saturday is quite busy: the museum, hike, and restaurant leave little
travel time. I suggest moving the hike to Sunday afternoon.”
```

The `read` result causes the next Agent Turn, but both turns belong to one Agent Run.

### 3. Read, create a list, and verify it

```text
User:
“I wrote meeting notes in notes.md. Extract the action items, save them
in todo.md, and then verify the saved file.”

Prompt Run
└── Agent Run
    ├── Agent Turn
    │   └── Tool Execution: read(path="notes.md")
    │       Result: “Alice will send the slides; Bob will book the room.”
    ├── Agent Turn
    │   └── Tool Execution: write(
    │       path="todo.md",
    │       content="- Alice: send the slides\\n- Bob: book the room")
    ├── Agent Turn
    │   └── Tool Execution: read(path="todo.md")
    │       Result: the two action items are present.
    └── final Agent Turn

Assistant:
“Done. I saved the two action items to todo.md and verified the file.”
```

This is one Agent Run with several Agent Turns: read, write, verify, and report.

### 4. Read two files in one batch

```text
User:
“Please compare trip.md with budget.md and tell me whether the trip is
within budget.”

Prompt Run
└── Agent Run
    ├── Agent Turn
    │   └── Tool Call Batch
    │       ├── Tool Execution: read(path="trip.md")
    │       │   Result: three hotel nights and two paid activities.
    │       └── Tool Execution: read(path="budget.md")
    │           Result: the available budget is $500.
    └── final Agent Turn

Assistant:
“The trip is within budget. The estimated cost is $460, leaving $40 for
local transport or unexpected costs.”
```

This batch contains two Tool Executions but only one Agent Turn. A Provider may also perform the two reads in separate turns; that difference is not a new Prompt Run.

Auto-Retry and compaction recovery are less visible in a normal conversation: if either is needed, the same Prompt Run continues with another Agent Run before the Agent Session returns to idle.
