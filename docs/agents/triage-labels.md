# Triage Labels

The local tracker uses Matt's two category roles and five canonical triage states without renaming them.

## Categories

| Matt role | `Category:` value | Meaning |
| --- | --- | --- |
| `bug` | `bug` | Existing behavior is broken |
| `enhancement` | `enhancement` | New behavior or an improvement |

## States

| Matt role | `Status:` value | Meaning |
| --- | --- | --- |
| `needs-triage` | `needs-triage` | Maintainer evaluation required |
| `needs-info` | `needs-info` | Waiting on the reporter |
| `ready-for-agent` | `ready-for-agent` | Fully specified for an AFK agent |
| `ready-for-human` | `ready-for-human` | Human implementation required |
| `wontfix` | `wontfix` | Will not be actioned |

A triaged open spec or ticket has exactly one category and one state. Completed local implementation records use the lifecycle status `implemented` instead of an open triage state; wayfinder tickets use their separate `open`/`claimed`/`resolved` lifecycle. See [`issue-tracker.md`](issue-tracker.md).
