---
status: accepted
---

# Treat pi v3 sessions as an interoperable wire contract

Session files identified as pi v3 implement pi's wire meanings for every supported entry alternative, including field requiredness, explicit nulls, parent/root topology, root-to-leaf branch order, and provider/model restoration from the active path. A C++-specific persistence need may not silently redefine a pi field or standard entry type; it uses an explicit extension mechanism or a separately named format.

## Considered options

- Keep a merely “pi-style” C++ v3 dialect: rejected because sharing pi's version and entry names creates a false interoperability promise and makes subtle topology corruption likely.
- Adopt an independent CCH session format: rejected for the currently supported contract because pi interoperability has higher product value and C++ has no limiting language constraint.
- Support pi v3 plus explicit extensions: accepted because unknown-entry tolerance permits local metadata without changing standard meanings.

## Consequences

- `parentId: null` remains an explicit root and is distinguishable from an absent legacy field.
- Public branch traversal with pi vocabulary returns root-to-leaf unless a differently named C++ API states otherwise.
- Resume derives provider/model state using pi's active-path rules and treats absent metadata as absent, never as an overriding empty value.
- Supported custom, compaction, and branch-summary entries preserve all pi fields.
- C++-specific leaf or metadata records are explicit extensions and cannot masquerade as pi union members.
- Contract fixtures exercise pi-to-C++ reading, C++-to-pi reading, and topology/model round trips.
