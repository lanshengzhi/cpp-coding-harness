/**
 * Shared harness for the `audit-1049` frozen-pi captures.
 *
 * Every upstream import is dynamic so the capture can isolate `HOME`/XDG state and the
 * inherited pi environment before a module reads it. The model path is a scripted faux
 * provider; no credential, provider catalog, network request or live `~/.pi` state is used.
 */

import { pathToFileURL } from "node:url";
import path from "node:path";

export const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? "");
if (!piCheckout || piCheckout === path.parse(piCheckout).root) {
    throw new Error("PI_CHECKOUT must name the private frozen pi checkout");
}

/** Import one frozen-checkout source path by its repository-relative spelling. */
export function importPi(relativePath: string): Promise<any> {
    return import(pathToFileURL(path.join(piCheckout, relativePath)).href);
}

/** Drop inherited pi state so the capture cannot read this session's agent directory. */
export function sanitizePiEnvironment(): void {
    for (const key of Object.keys(process.env)) {
        if (key.startsWith("PI_")) delete process.env[key];
    }
}

/** Point home and XDG lookups at a controlled temporary directory. */
export function isolateHome(homeDir: string): void {
    process.env.HOME = homeDir;
    process.env.XDG_CONFIG_HOME = path.join(homeDir, ".config");
    process.env.XDG_DATA_HOME = path.join(homeDir, ".local", "share");
    process.env.XDG_STATE_HOME = path.join(homeDir, ".local", "state");
    process.env.XDG_CACHE_HOME = path.join(homeDir, ".cache");
}

export async function createModelHarness(options: { maxTokens?: number } = {}): Promise<any> {
    const [{ AuthStorage }, { ModelRuntime }, { InMemoryCodingAgentModelsStore }, compat] = await Promise.all([
        importPi("packages/coding-agent/src/core/auth-storage.ts"),
        importPi("packages/coding-agent/src/core/model-runtime.ts"),
        importPi("packages/coding-agent/src/core/models-store.ts"),
        importPi("packages/ai/src/compat.ts"),
    ]);
    const maxTokens = options.maxTokens ?? 16384;
    const faux = compat.registerFauxProvider({
        models: [{ id: "faux-1", name: "Faux Model", maxTokens, contextWindow: 128000 }],
    });
    const model = faux.getModel();
    const auth = AuthStorage.inMemory();
    await auth.modify(model.provider, async () => ({ type: "api_key", key: "dummy-audit-1049" }));
    const modelRuntime = await ModelRuntime.create({
        credentials: auth,
        modelsPath: null,
        modelsStore: new InMemoryCodingAgentModelsStore(),
        allowModelNetwork: false,
    });
    modelRuntime.registerProvider(model.provider, {
        baseUrl: model.baseUrl,
        apiKey: "dummy-audit-1049",
        api: faux.api,
        models: faux.models.map((entry: any) => ({
            id: entry.id,
            name: entry.name,
            api: entry.api,
            reasoning: entry.reasoning,
            input: entry.input,
            inputLimits: entry.inputLimits,
            cost: entry.cost,
            contextWindow: entry.contextWindow,
            maxTokens: entry.maxTokens,
            baseUrl: entry.baseUrl,
        })),
    });
    return {
        faux,
        model,
        modelRuntime,
        dispose() {
            faux.unregister();
        },
    };
}

export async function createResourceLoaderStub(): Promise<any> {
    const { createExtensionRuntime } = await importPi("packages/coding-agent/src/core/extensions/index.ts");
    const runtime = createExtensionRuntime();
    return {
        getExtensions: () => ({ extensions: [], errors: [], runtime }),
        getSkills: () => ({ skills: [], diagnostics: [] }),
        getPrompts: () => ({ prompts: [], diagnostics: [] }),
        getThemes: () => ({ themes: [], diagnostics: [] }),
        getAgentsFiles: () => ({ agentsFiles: [] }),
        getSystemPrompt: () => undefined,
        getSystemPromptSource: () => undefined,
        getAppendSystemPrompt: () => [],
        getAppendSystemPromptSources: () => [],
        extendResources: () => {},
        reload: async () => {},
    };
}

export async function createSession(options: {
    workspace: string;
    agentDir: string;
    modelHarness: any;
    resourceLoader: any;
    settings?: any;
}): Promise<any> {
    const [{ SettingsManager }, { SessionManager }, { createAgentSession }] = await Promise.all([
        importPi("packages/coding-agent/src/core/settings-manager.ts"),
        importPi("packages/coding-agent/src/core/session-manager.ts"),
        importPi("packages/coding-agent/src/core/sdk.ts"),
    ]);
    const settingsManager = options.settings ?? SettingsManager.inMemory();
    const sessionManager = SessionManager.inMemory(options.workspace);
    const created = await createAgentSession({
        cwd: options.workspace,
        agentDir: options.agentDir,
        modelRuntime: options.modelHarness.modelRuntime,
        model: options.modelHarness.model,
        sessionManager,
        settingsManager,
        resourceLoader: options.resourceLoader,
    });
    return { session: created.session, settingsManager, sessionManager };
}

export function messageText(message: any): string {
    if (typeof message?.content === "string") return message.content;
    if (!Array.isArray(message?.content)) return "";
    return message.content
        .map((block: any) => (typeof block?.text === "string" ? block.text : ""))
        .join("");
}

export function systemText(message: any): string {
    if (typeof message?.content === "string" && message.content.length > 0) return message.content;
    if (message?.sections && typeof message.sections === "object") {
        return Object.values(message.sections).join("\n");
    }
    return "";
}

/** A deep JSON-safe value with machine paths replaced by declared placeholders. */
export function normalizeValue(value: any, replacements: Array<[string, string]>): any {
    if (typeof value === "string") {
        let normalized = value;
        for (const [needle, placeholder] of replacements) {
            if (needle) normalized = normalized.split(needle).join(placeholder);
        }
        return normalized;
    }
    if (Array.isArray(value)) return value.map((entry) => normalizeValue(entry, replacements));
    if (value && typeof value === "object") {
        return Object.fromEntries(
            Object.entries(value).map(([key, entry]) => [key, normalizeValue(entry, replacements)]),
        );
    }
    return value;
}

export function scenario(
    name: string,
    inputs: Record<string, unknown> | unknown[],
    expected: unknown,
    options: { normalizations?: string[]; upstreamTest?: { path: string; case: string } } = {},
): any {
    return {
        name,
        inputs,
        expected,
        normalizations: options.normalizations ?? [],
        ...(options.upstreamTest ? { upstreamTest: options.upstreamTest } : {}),
    };
}
