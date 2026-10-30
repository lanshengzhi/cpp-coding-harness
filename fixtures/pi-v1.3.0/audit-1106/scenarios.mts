export async function captureScenarios(pi: any) {
    const call = { type: "toolCall", id: "call-1", name: "work", arguments: {} };
    const assistantMessage = { role: "assistant", content: [call], stopReason: "toolUse", timestamp: 0 };
    const context = { messages: [] };
    const observations: any[] = [];
    for (const mode of ["cancelled-before", "cancelled-during", "after-error", "unknown"]) {
        let executed = 0;
        const controller = new AbortController();
        if (mode === "cancelled-before") controller.abort();
        const tool = {
            name: "work", label: "work", description: "work", parameters: { type: "object", properties: {}, additionalProperties: false },
            execute: async () => {
                executed++;
                if (mode === "cancelled-during") {
                    controller.abort();
                    throw new Error("cancelled during execute");
                }
                return { content: [{ type: "text", text: "ok" }], details: {} };
            },
        };
        const outcome = await pi.runToolCall(call, {
            tools: mode === "unknown" ? [] : [tool], assistantMessage, context,
            signal: controller.signal,
            beforeToolCall: async () => {
                if (mode === "cancelled-before") throw new Error("cancelled before execute");
            },
            afterToolCall: async () => {
                if (mode === "after-error") throw new Error("after failed");
            },
        });
        const observed = { executed, isError: outcome.isError, durationPresent: outcome.durationMs !== undefined };
        observations.push({ name: `tool duration ${mode}`, inputs: { mode }, expected: observed, observed, normalizations: [] });
    }
    return [{ path: "duration.json", sourceEndpoints: ["packages/agent/src/agent-loop.ts:runToolCall"], scenarios: observations }];
}
