# #1239 stale-field and receipt gates

## Scope and result

On 2026-11-06, 44 previously verified claims were bounded because their discriminating case reused a comparison test. Their comparisons remain in the receipts; each row reverts to its source-audit status. The measured verified count fell from 609 to 565. No membership changed.

## Bounded rows and follow-up owners

- `cch_durable:image-processor` — #1242
- `cch_durable:read-image` — #1242
- `cch_durable:image-capability-absent` — #1242
- `cch_durable:image-orientation-conversion` — #1242
- `cch_coding_agent:codemode-models-description-gate` — #1241
- `cch_chord:context:without-abort-signal` — #1240
- `cch_chord:delta:batch-application` — #1240
- `cch_chord:delta:immutable-application` — #1240
- `cch_chord:delta:mutable-application` — #1240
- `cch_chord:delta:overlap` — #1240
- `cch_chord:delta:prepare-replace` — #1240
- `cch_chord:state:attached-hydration` — #1240
- `cch_chord:state:dispose` — #1240
- `cch_chord:state:listener-error` — #1240
- `cch_chord:Chord:draft` — #1240
- `cch_chord:json:copy-json` — #1240
- `cch_chord:json:strict-validation` — #1240
- `cch_chord:json:json-value` — #1240
- `cch_chord:Chord:json` — #1240
- `cch_chord:state:delivery-order` — #1240
- `cch_codemode:codemode-declaration-identifier` — #1241
- `cch_codemode:codemode-script-globals` — #1241
- `cch_codemode:codemode-script-store-load` — #1241
- `cch_codemode:codemode-script-output-text-image-console` — #1241
- `cch_codemode:codemode-script-error-taxonomy` — #1241
- `cch_codemode:codemode-script-abort-reason` — #1241
- `cch_codemode:codemode-deterministic-start` — #1241
- `cch_codemode:codemode-deterministic-clock-seed` — #1241
- `cch_codemode:codemode-deterministic-seq` — #1241
- `cch_codemode:codemode-deterministic-during` — #1241
- `cch_codemode:codemode-deterministic-validation` — #1241
- `cch_codemode:codemode-inline-success` — #1241
- `cch_codemode:codemode-remote-exchange` — #1241
- `cch_codemode:codemode-remote-caps` — #1241
- `cch_codemode:codemode-remote-failures` — #1241
- `cch_codemode:codemode-worker-admission` — #1241
- `cch_agent_core:InputQueue:peek-clear` — #1243
- `cch_agent_core:Agent:reset` — #1243
- `cch_agent_core:Agent:prompt-busy` — #1243
- `cch_coding_agent:theme-token-set` — #1243
- `cch_codemode:codemode-execute-outcome` — #1241
- `cch_codemode:codemode-portable-entry` — #1241
- `cch_durable:Storage:jsonl:sidecar` — #1242
- `cch_durable:Storage:jsonl:ordering` — #1242

## Remaining boundary

The follow-up tickets own independent discriminating cases. The dated stale-field baseline records existing contradictions separately; #1180 owns durable cleanup, with #1238 retained as the historical reference, and #1044 owns the remaining baseline.
