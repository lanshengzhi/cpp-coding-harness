# pi-ai compatibility fixtures

These committed Model-shape goldens are copied from the supported contract at the frozen pi baseline
`83114817c68f5413e4d7ba6d7003ddc511cd31d2` (`packages/ai/src/types.ts` and
`packages/agent/src/agent.ts`). They contain no credentials or secret-derived values.

- `models/default-model.json` mirrors pi Agent's `DEFAULT_MODEL`; omitted optional members mean no thinking map,
  headers, cost tiers, or API compat.
- `models/complete-anthropic-model.json` exercises every supported Model field. Its absent `off` thinking key means
  provider default, while `"low": null` means that level is unsupported.

The full adapter/auth/persistence capability checklist and shard hash land with the pi-ai completion gate (#347).
