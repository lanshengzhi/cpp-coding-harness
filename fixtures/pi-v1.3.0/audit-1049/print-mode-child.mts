/**
 * Print-mode child for the `audit-1049` capture.
 *
 * This process exists so the capture records the real process stdout/stderr and exit status of
 * `runPrintMode`, not an in-process approximation. The parent supplies the frozen checkout via
 * `PI_CHECKOUT` and the destination result file as argv[2]; this child writes only the ancillary
 * observation JSON. All model and filesystem state lives under a fresh temporary root.
 */

import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

import {
    createModelHarness,
    createResourceLoaderStub,
    createSession,
    importPi,
    isolateHome,
    sanitizePiEnvironment,
} from "./capture-helpers.mts";

const scenarioName = process.argv[2];
const resultPath = process.argv[3];
if (scenarioName !== "truncated" && scenarioName !== "complete") {
    throw new Error(`unknown print scenario: ${scenarioName}`);
}
if (!resultPath) throw new Error("print child requires a result path");

sanitizePiEnvironment();
const root = mkdtempSync(path.join(tmpdir(), "audit-1049-print-"));
const workspace = path.join(root, "workspace");
const agentDir = path.join(root, "agent");
const homeDir = path.join(root, "home");
mkdirSync(workspace, { recursive: true });
mkdirSync(agentDir, { recursive: true });
mkdirSync(homeDir, { recursive: true });
isolateHome(homeDir);

const modelHarness = await createModelHarness({ maxTokens: 4 });
const resourceLoader = await createResourceLoaderStub();
const { session, settingsManager } = await createSession({ workspace, agentDir, modelHarness, resourceLoader });
const { fauxAssistantMessage } = await importPi("packages/ai/src/compat.ts");
const { AgentSessionRuntime } = await importPi("packages/coding-agent/src/core/sdk.ts");
const { runPrintMode } = await importPi("packages/coding-agent/src/modes/print-mode.ts");

modelHarness.faux.setResponses([
    scenarioName === "truncated"
        ? fauxAssistantMessage("partial answer", { stopReason: "length" })
        : fauxAssistantMessage("complete answer"),
]);
const services = {
    cwd: workspace,
    agentDir,
    modelRuntime: modelHarness.modelRuntime,
    settingsManager,
    resourceLoader,
    diagnostics: [],
};
const runtimeHost = new AgentSessionRuntime(session, services, async () => {
    throw new Error("print-mode capture does not switch sessions");
});
const exitCode = await runPrintMode(runtimeHost, { mode: "text", initialMessage: "go" });

const assistants = session.messages.filter((message: any) => message.role === "assistant");
const last = assistants.at(-1);
const result = {
    scenario: scenarioName,
    modelMaxTokens: modelHarness.model.maxTokens,
    exitCode,
    sessionRoles: session.messages.map((message: any) => message.role),
    finalAssistant: last
        ? {
              stopReason: last.stopReason,
              text: last.content
                  .filter((block: any) => block.type === "text")
                  .map((block: any) => block.text)
                  .join(""),
              outputTokens: last.usage?.output ?? null,
          }
        : null,
};
writeFileSync(resultPath, `${JSON.stringify(result, null, 2)}\n`);

modelHarness.dispose();
rmSync(root, { recursive: true, force: true });
process.exitCode = exitCode;
