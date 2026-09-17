# Skill Capability Negotiation

Pike does not inspect, declare, or negotiate the tools and capabilities a skill
assumes. Skills enter the model's context verbatim, exactly as pi does it.

This covers the whole family of requests: a `requires-tools`-style frontmatter
declaration with load/invocation-time checks, a generic "your tool surface is
X, improvise" note injected into the skill wrapper, substitution or degradation
of skill-body instructions that name absent tools, and user-visible warnings
when a skill references something pike does not provide.

## Why this is out of scope

**The capability vocabulary belongs to the Agent Skills standard, not to
pike.** Pike's skill pipeline deliberately mirrors pi's: discovery lists
name/description/location, invocation wraps the body in a `<skill>` block,
nothing in between inspects content. If the standard or pi ever lands real
capability semantics, pike inherits them by parity; until then, inventing a
pike-private dialect buys divergence without an upstream behavior to align
with.

**There is no upstream design to copy.** Verified against pi source at HEAD
(`e4c75a732`, well past the frozen baseline): `SkillFrontmatter` is an open map
but only `name`, `description`, and `disable-model-invocation` are consumed.
The documented `allowed-tools` field is dead — and its semantics are
"pre-approved permissions", not "required capabilities", so it cannot be
adopted as a requirement declaration anyway. The one live industry sample
(Claude Code's `shell: bash` fail-fast check) guards a hard execution
dependency, not prose instructions.

**The shared-skill ecosystem makes pike-specific warnings permanent noise.**
Skill libraries like `~/.agents/skills` serve several harnesses at once and are
written against the richest tool surface available. Annotating or warning on
every cross-harness skill would tax the exact libraries pike users rely on,
without the skill authors being able to do anything about it.

**The pi-sanctioned degradation path already works.** The system prompt's
"Available tools" list states the tool surface truthfully, and models adapt —
this was observed working in the dogfooding session that raised the request:
told to "call the Skill tool" and "spawn a sub-agent", the agent read the
referenced files and inlined the instructions instead. That improvisation is
the accepted mechanism, not a gap to engineer around.

## Prior requests

- #620: "Skill documents reference tools pike does not provide (sub-agents, Skill tool); no capability negotiation"
