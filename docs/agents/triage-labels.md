# Triage Labels

The skills speak in terms of five canonical triage roles. This tracker uses the canonical label strings unchanged.

| Label | Meaning |
| ----- | ------- |
| `needs-triage` | Maintainer needs to evaluate this issue |
| `needs-info` | Waiting on reporter for more information |
| `ready-for-agent` | Fully specified, ready for an AFK agent |
| `ready-for-human` | Requires human implementation |
| `wontfix` | Will not be actioned |

When a skill mentions a role (e.g. "apply the AFK-ready triage label"), use the corresponding label string from this table. If this tracker ever renames a label, edit the table to match.

In-flight triage labels (`needs-*`, `ready-for-*`) represent backlog state; they are removed when an issue is claimed (`--remove-label "..."`).
