#!/usr/bin/env tsx
/**
 * Capture the `audit-1049` resource, configuration, native-extension and print-mode family
 * against the frozen `pi-v1.3.0` baseline.
 *
 * The observations are public-call/scenario results from the frozen checkout, not a Pike
 * expectation and not a capability-alignment claim. Every scenario uses a controlled temporary
 * workspace and a scripted faux model; no live credentials, provider catalogs or `~/.pi` state
 * are read. The bundle is written only after the baseline, clean-source, host and family guards
 * pass and every observation succeeds.
 *
 * Usage (from the repository root):
 *   PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1049/capture-1049.mts
 */

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1049";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));

if (process.env.TSX_TSCONFIG_PATH === undefined) {
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        stdio: "inherit",
        env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
    });
    if (result.error) {
        throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
    }
    process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
    throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
    throw new Error(
        `unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
            Object.keys(registry.baselines).sort().join(", "),
    );
}
const head = runGit(piCheckout, ["rev-parse", "HEAD"]);
if (head !== baseline.revision) {
    throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (runGit(piCheckout, ["status", "--porcelain"])) {
    throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
    throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
    if (name !== baselineName && entry.bundles && family in entry.bundles) {
        throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
    }
}

const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
    tsxVersion = requirePi("tsx/package.json").version;
} catch {
    // The tsx version is provenance, not a capture precondition; keep the sentinel explicit.
}
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: glibc,
    node: process.version,
    tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();

const helpers = await import(
    pathToFileURL(path.join(familyDir, "capture-helpers.mts")).href
);
helpers.sanitizePiEnvironment();

const root = mkdtempSync(path.join(tmpdir(), "audit-1049-"));
const workspace = path.join(root, "workspace");
const agentDir = path.join(root, "agent");
const homeDir = path.join(root, "home");
helpers.isolateHome(homeDir);
for (const directory of [workspace, agentDir, homeDir]) {
    mkdirSync(directory, { recursive: true });
}

const sha256 = (bytes: Buffer | string) => createHash("sha256").update(bytes).digest("hex");
const writeJson = (target: string, value: any) => {
    writeFileSync(target, `${JSON.stringify(value, null, 2)}\n`);
};
const readJson = (target: string) => JSON.parse(readFileSync(target, "utf8"));
const pathNames = (value: string) => value.split(path.sep).join("/");

try {
    const promptScenarios = await capturePromptExpansion(workspace, agentDir, homeDir);
    const resourceScenarios = await captureResources(workspace, agentDir, homeDir);
    const extensionScenarios = await captureExtension(workspace, agentDir, homeDir);
    const configurationScenarios = await captureConfiguration(workspace, agentDir, homeDir);
    const printScenarios = await capturePrintMode();

    const artifactSpecs = [
        {
            name: "prompt-expansion.json",
            sourceEndpoints: [
                "packages/coding-agent/src/core/prompt-templates.ts:parseCommandArgs,substituteArgs,expandPromptTemplate",
                "packages/coding-agent/src/core/agent-session.ts:prompt",
                "packages/coding-agent/test/prompt-templates.test.ts:expandPromptTemplate",
            ],
            scenarios: promptScenarios,
        },
        {
            name: "resource-selection.json",
            sourceEndpoints: [
                "packages/coding-agent/src/core/resource-loader.ts:loadProjectContextFiles,DefaultResourceLoader",
                "packages/coding-agent/src/utils/paths.ts:resolvePath,isLocalPath",
                "packages/coding-agent/src/main.ts:resolveCliPaths",
            ],
            scenarios: resourceScenarios,
        },
        {
            name: "extension-run.json",
            sourceEndpoints: [
                "packages/coding-agent/src/core/extensions/loader.ts:loadExtensionFromFactory",
                "packages/coding-agent/src/core/extensions/runner.ts:emitBeforeAgentStart",
                "packages/coding-agent/src/core/agent-session.ts:_prepareRun",
                "packages/coding-agent/src/core/extensions/types.ts:BeforeAgentStartEvent",
            ],
            scenarios: extensionScenarios,
        },
        {
            name: "configuration-schema.json",
            sourceEndpoints: [
                "packages/coding-agent/src/core/settings-schema.ts:SettingsSchema",
                "packages/coding-agent/src/core/settings-manager.ts:SettingsManager",
                "packages/coding-agent/src/core/keybindings.ts:KeybindingsManager",
                "packages/coding-agent/src/core/keybindings-schema.ts:KeybindingsSchema",
                "packages/coding-agent/src/modes/interactive/theme/theme-schema.ts:validateThemeJson",
                "packages/coding-agent/src/modes/interactive/theme/theme.ts:loadThemeFromPath",
                "packages/coding-agent/src/modes/interactive/theme/theme-tokens.ts:THEME_TOKENS",
            ],
            scenarios: configurationScenarios,
        },
        {
            name: "print-mode.json",
            sourceEndpoints: [
                "packages/coding-agent/src/modes/print-mode.ts:runPrintMode",
                "packages/coding-agent/src/modes/interactive/components/assistant-message.ts:Response was truncated",
            ],
            scenarios: printScenarios,
        },
    ];

    const replacements: Array<[string, string]> = [
        [pathNames(workspace), "<workspace>"],
        [pathNames(agentDir), "<agent-dir>"],
        [pathNames(homeDir), "<home>"],
        [pathNames(root), "<temp-root>"],
        [pathNames(piCheckout), "<pi-checkout>"],
    ];
    const artifactRecords = [];
    for (const spec of artifactSpecs) {
        const scenarios = helpers.normalizeValue(spec.scenarios, replacements);
        const sourceEndpoints = helpers.normalizeValue(spec.sourceEndpoints, replacements);
        const artifact = {
            schema: "pike/pi-v1.3.0-audit-evidence/1",
            baseline: baselineName,
            revision: baseline.revision,
            capturedAt,
            environment,
            sourceEndpoints,
            scenarios,
        };
        const bytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
        writeFileSync(path.join(familyDir, spec.name), bytes);
        artifactRecords.push({
            path: spec.name,
            family,
            bytes: bytes.length,
            sha256: sha256(bytes),
            sourceEndpoints,
            scenarios,
        });
    }

    const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
    const generatorSources = [
        scriptPath,
        path.join(familyDir, "capture-helpers.mts"),
        path.join(familyDir, "print-mode-child.mts"),
    ].map((source) => ({
        path: path.relative(repositoryRoot, source).split(path.sep).join("/"),
        sha256: sha256(readFileSync(source)),
    }));
    const manifest = {
        schema: "pike/pi-v1.3.0-audit-evidence/1",
        baseline: baselineName,
        revision: baseline.revision,
        ticket: "1049",
        family,
        capturedAt,
        environment,
        generator: {
            path: generatorPath,
            sha256: sha256(readFileSync(scriptPath)),
            command:
                `PI_CHECKOUT=<pi-checkout> PI_BASELINE=${baselineName} ` +
                `<pi-checkout>/node_modules/.bin/tsx ${generatorPath}`,
            sources: generatorSources,
        },
        artifacts: artifactRecords,
    };
    writeJson(path.join(familyDir, "manifest.json"), manifest);
    console.log(`captured ${family} for ${baselineName} (${baseline.revision})`);
    for (const record of artifactRecords) {
        console.log(`  ${record.path}: ${record.scenarios.length} scenarios, ${record.bytes} bytes`);
    }
} finally {
    rmSync(root, { recursive: true, force: true });
}

function runGit(cwd: string, args: string[]): string {
    const result = spawnSync("git", args, {
        cwd,
        encoding: "utf8",
        env: { ...process.env, GIT_CONFIG_NOSYSTEM: "1", GIT_CONFIG_GLOBAL: "/dev/null" },
    });
    if (result.error) throw result.error;
    if (result.status !== 0) {
        throw new Error(`git ${args.join(" ")} failed in ${cwd}: ${result.stderr}`);
    }
    return result.stdout.trim();
}

async function capturePromptExpansion(workspacePath: string, agentDirectory: string, home: string) {
    const promptTemplates = await helpers.importPi("packages/coding-agent/src/core/prompt-templates.ts");
    const { DefaultResourceLoader } = await helpers.importPi("packages/coding-agent/src/core/resource-loader.ts");
    const { SettingsManager } = await helpers.importPi("packages/coding-agent/src/core/settings-manager.ts");
    const { fauxAssistantMessage } = await helpers.importPi("packages/ai/src/compat.ts");

    const promptsDir = path.join(workspacePath, "prompts");
    mkdirSync(promptsDir, { recursive: true });
    writeFileSync(path.join(promptsDir, "review.md"), "Review $2. Focus: ${1:-correctness}.");
    const settingsManager = SettingsManager.inMemory();
    const loader = new DefaultResourceLoader({
        cwd: workspacePath,
        agentDir: agentDirectory,
        settingsManager,
        noSkills: true,
        noThemes: true,
        noExtensions: true,
        additionalPromptTemplatePaths: [promptsDir],
    });
    await loader.reload();
    const loadedPrompts = loader.getPrompts();
    const template = loadedPrompts.prompts.find((entry: any) => entry.name === "review");

    const doubleQuoted = promptTemplates.expandPromptTemplate('/review "" src/index.ts', loadedPrompts.prompts);
    const singleQuoted = promptTemplates.expandPromptTemplate("/review '' src/index.ts", loadedPrompts.prompts);
    const parsedDoubleQuoted = promptTemplates.parseCommandArgs('""');
    const parsedMixed = promptTemplates.parseCommandArgs('alpha "beta gamma" "" \'delta epsilon\'');
    const substituted = promptTemplates.substituteArgs(
        "$1|$2|$@|$ARGUMENTS|${1:-x}|${2:-y}|${@:2}|${@:2:1}|${@:-z}|${ARGUMENTS:-z}",
        ["", "src/index.ts"],
    );

    const modelHarness = await helpers.createModelHarness();
    const { session } = await helpers.createSession({
        workspace: workspacePath,
        agentDir: agentDirectory,
        modelHarness,
        resourceLoader: loader,
    });
    let request: any;
    modelHarness.faux.setResponses([
        (context: any) => {
            request = {
                roles: context.messages.map((message: any) => message.role),
                userText: helpers.messageText(context.messages.find((message: any) => message.role === "user")),
                systemContainsTemplateCommand: helpers
                    .systemText(context.messages.find((message: any) => message.role === "system"))
                    .includes("/review"),
            };
            return fauxAssistantMessage("prompt scenario response");
        },
    ]);
    await session.prompt('/review "" src/index.ts');
    const userMessage = session.messages.find((message: any) => message.role === "user");
    const finalAssistant = session.messages.filter((message: any) => message.role === "assistant").at(-1);
    const sessionObservation = {
        promptNames: loadedPrompts.prompts.map((entry: any) => entry.name),
        promptDiagnostics: loadedPrompts.diagnostics.map((diagnostic: any) => diagnostic.message),
        userText: helpers.messageText(userMessage),
        requestUserText: request?.userText ?? null,
        requestSystemContainsTemplateCommand: request?.systemContainsTemplateCommand ?? null,
        assistantText: helpers.messageText(finalAssistant),
        isIdleAfter: session.isIdle,
    };
    session.dispose();
    modelHarness.dispose();
    void home;

    return [
        helpers.scenario(
            "quoted-empty arguments are retained by the prompt argument parser",
            {
                arguments: ['""', "''", 'alpha "beta gamma" "" \'delta epsilon\''],
                quotedTemplateCommand: '/review "" src/index.ts',
            },
            {
                doubleQuoted: parsedDoubleQuoted,
                singleQuoted: promptTemplates.parseCommandArgs("''"),
                mixed: parsedMixed,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/prompt-templates.test.ts",
                    case: "should use positional default for empty quoted argument without shifting later args",
                },
            },
        ),
        helpers.scenario(
            "quoted-empty argument does not shift positional defaults in template expansion",
            {
                template: "Review $2. Focus: ${1:-correctness}.",
                commands: ['/review "" src/index.ts', "/review '' src/index.ts"],
            },
            {
                doubleQuoted,
                singleQuoted,
                templateFound: template !== undefined,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/prompt-templates.test.ts",
                    case: "should use positional default for empty quoted argument without shifting later args",
                },
            },
        ),
        helpers.scenario(
            "positional, collection and default substitution forms",
            {
                arguments: ["", "src/index.ts"],
                template:
                    "$1|$2|$@|$ARGUMENTS|${1:-x}|${2:-y}|${@:2}|${@:2:1}|${@:-z}|${ARGUMENTS:-z}",
            },
            { substituted },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/prompt-templates.test.ts",
                    case: "parseCommandArgs + substituteArgs integration",
                },
            },
        ),
        helpers.scenario(
            "session prompt expands the quoted-empty template before the model call",
            {
                command: '/review "" src/index.ts',
                template: "Review $2. Focus: ${1:-correctness}.",
                workspace: "controlled temporary workspace",
            },
            sessionObservation,
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/suite/agent-session-prompt.test.ts",
                    case: "expands prompt templates before sending the prompt",
                },
            },
        ),
    ];
}

async function captureResources(workspacePath: string, agentDirectory: string, home: string) {
    const { DefaultResourceLoader, loadProjectContextFiles } = await helpers.importPi(
        "packages/coding-agent/src/core/resource-loader.ts",
    );
    const { SettingsManager } = await helpers.importPi("packages/coding-agent/src/core/settings-manager.ts");
    const paths = await helpers.importPi("packages/coding-agent/src/utils/paths.ts");

    const precedenceDir = path.join(workspacePath, "precedence");
    mkdirSync(precedenceDir, { recursive: true });
    writeFileSync(path.join(precedenceDir, "AGENTS.override.md"), "override marker\n");
    writeFileSync(path.join(precedenceDir, "AGENTS.md"), "plain marker\n");
    writeFileSync(path.join(precedenceDir, "CLAUDE.md"), "claude marker\n");
    const precedence = loadProjectContextFiles({ cwd: precedenceDir, agentDir: agentDirectory });

    const treeDir = path.join(workspacePath, "tree");
    mkdirSync(path.join(treeDir, "nested"), { recursive: true });
    writeFileSync(path.join(treeDir, "AGENTS.md"), "root marker\n");
    writeFileSync(path.join(treeDir, "nested", "AGENTS.md"), "nested marker\n");
    const ancestors = loadProjectContextFiles({ cwd: path.join(treeDir, "nested"), agentDir: agentDirectory });

    const mainRepo = path.join(workspacePath, "worktree", "main");
    mkdirSync(mainRepo, { recursive: true });
    const git = (args: string[]) => runGit(mainRepo, args);
    git(["init", "-q"]);
    writeFileSync(path.join(mainRepo, "AGENTS.md"), "main marker\n");
    git(["add", "AGENTS.md"]);
    git(["-c", "user.name=Audit 1049", "-c", "user.email=audit-1049@example.invalid", "commit", "-q", "-m", "init"]);
    const nestedWorktree = path.join(mainRepo, "nested");
    git(["worktree", "add", "-q", "-b", "audit-1049-nested", nestedWorktree]);
    writeFileSync(path.join(nestedWorktree, "AGENTS.md"), "nested worktree marker\n");
    const worktreeContext = loadProjectContextFiles({ cwd: nestedWorktree, agentDir: agentDirectory });

    const linkedWorktree = path.join(mainRepo, "linked");
    git(["worktree", "add", "-q", "-b", "audit-1049-linked", linkedWorktree]);
    rmSync(path.join(linkedWorktree, "AGENTS.md"));
    symlinkSync("../AGENTS.md", path.join(linkedWorktree, "AGENTS.md"));
    const symlinkContext = loadProjectContextFiles({ cwd: linkedWorktree, agentDir: agentDirectory });

    const templatesDir = path.join(workspacePath, "home", "templates");
    mkdirSync(templatesDir, { recursive: true });
    const pathCases = {
        tilde: paths.resolvePath("~/templates", workspacePath, { homeDir: home }),
        tildeDisabled: paths.resolvePath("~/templates", workspacePath, { homeDir: home, expandTilde: false }),
        relative: paths.resolvePath("./templates", workspacePath),
        trimmed: paths.resolvePath(" ../templates ", workspacePath, { trim: true }),
        unicodeTrimmed: paths.resolvePath("\u00A0templates\u00A0", workspacePath, {
            trim: true,
            normalizeUnicodeSpaces: true,
        }),
        atDefault: paths.resolvePath("@templates", workspacePath),
        atPrefixed: paths.resolvePath("@templates", workspacePath, { stripAtPrefix: true }),
        fileUrl: paths.resolvePath(pathToFileURL(templatesDir).href, workspacePath),
        isLocal: {
            git: paths.isLocalPath("git:github.com/example/repo"),
            npm: paths.isLocalPath("npm:@scope/pkg"),
            fileUrl: paths.isLocalPath(pathToFileURL(templatesDir).href),
        },
        packageSources: {
            git: {
                isLocal: paths.isLocalPath("git:github.com/example/repo"),
                resolved: paths.isLocalPath("git:github.com/example/repo")
                    ? paths.resolvePath("git:github.com/example/repo", workspacePath)
                    : null,
            },
            npm: {
                isLocal: paths.isLocalPath("npm:@scope/pkg"),
                resolved: paths.isLocalPath("npm:@scope/pkg")
                    ? paths.resolvePath("npm:@scope/pkg", workspacePath)
                    : null,
            },
        },
    };

    const promptsDir = path.join(workspacePath, "resources", "prompts");
    const skillsDir = path.join(workspacePath, "resources", "skills");
    const themesDir = path.join(workspacePath, "resources", "themes");
    mkdirSync(promptsDir, { recursive: true });
    mkdirSync(skillsDir, { recursive: true });
    mkdirSync(themesDir, { recursive: true });
    writeFileSync(path.join(promptsDir, "review.md"), "Review $2. Focus: ${1:-correctness}.");
    writeFileSync(
        path.join(skillsDir, "audit-skill.md"),
        "---\nname: audit-skill\ndescription: Audit skill\n---\nSkill body\n",
    );
    const darkTheme = readJson(
        path.join(piCheckout, "packages/coding-agent/src/modes/interactive/theme/dark.json"),
    );
    darkTheme.name = "audit-1049-dark";
    writeFileSync(path.join(themesDir, "audit-1049-dark.json"), `${JSON.stringify(darkTheme, null, 2)}\n`);
    const resourceLoader = new DefaultResourceLoader({
        cwd: workspacePath,
        agentDir: agentDirectory,
        settingsManager: SettingsManager.inMemory(),
        noSkills: true,
        noPromptTemplates: true,
        noThemes: true,
        noExtensions: true,
        additionalPromptTemplatePaths: [promptsDir],
        additionalSkillPaths: [skillsDir],
        additionalThemePaths: [themesDir],
    });
    await resourceLoader.reload();
    const resourceSelection = {
        prompts: resourceLoader.getPrompts().prompts.map((entry: any) => entry.name),
        promptDiagnostics: resourceLoader.getPrompts().diagnostics.length,
        skills: resourceLoader.getSkills().skills.map((entry: any) => entry.name),
        skillDiagnostics: resourceLoader.getSkills().diagnostics.length,
        themes: resourceLoader.getThemes().themes.map((entry: any) => entry.name),
        themeDiagnostics: resourceLoader.getThemes().diagnostics.length,
    };

    return [
        helpers.scenario(
            "AGENTS.override.md wins over AGENTS.md and ancestor context is root-to-leaf",
            {
                precedenceFiles: ["AGENTS.override.md", "AGENTS.md", "CLAUDE.md"],
                nestedFiles: ["tree/AGENTS.md", "tree/nested/AGENTS.md"],
                workspace: "controlled temporary workspace",
            },
            {
                precedence: {
                    paths: precedence.map((entry: any) => entry.path),
                    contents: precedence.map((entry: any) => entry.content),
                },
                ancestors: {
                    paths: ancestors.map((entry: any) => entry.path),
                    contents: ancestors.map((entry: any) => entry.content),
                },
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/resource-loader.test.ts",
                    case: "should prefer AGENTS.override.md within each directory while preserving ancestor layering",
                },
            },
        ),
        helpers.scenario(
            "nested linked worktree shadows the main repo context file",
            {
                repository: "worktree/main",
                linkedWorktree: "worktree/main/nested",
                files: ["worktree/main/AGENTS.md", "worktree/main/nested/AGENTS.md"],
                workspace: "controlled temporary workspace",
            },
            {
                paths: worktreeContext.map((entry: any) => entry.path),
                contents: worktreeContext.map((entry: any) => entry.content),
                mainContextFileExists: true,
                mainContextLoadedSeparately: false,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/resource-loader.test.ts",
                    case: "should skip the main repo's duplicate when the worktree root has its own context",
                },
            },
        ),
        helpers.scenario(
            "worktree context file symlink is loaded once and not duplicated from the main repo",
            {
                repository: "worktree/main",
                linkedWorktree: "worktree/main/linked",
                symlinkTarget: "../AGENTS.md",
                workspace: "controlled temporary workspace",
            },
            {
                paths: symlinkContext.map((entry: any) => entry.path),
                contents: symlinkContext.map((entry: any) => entry.content),
                mainContextLoadedSeparately: false,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/resource-loader.test.ts",
                    case: "should load the worktree's context once when it symlinks to the main repo's file",
                },
            },
        ),
        helpers.scenario(
            "local path modifiers expand tilde and strip a leading @",
            {
                cwd: "controlled temporary workspace",
                home: "controlled temporary home",
                input: "~/templates",
                variants: ["expandTilde:false", "trim", "normalizeUnicodeSpaces", "default @", "stripAtPrefix", "file: URL"],
            },
            pathCases,
        ),
        helpers.scenario(
            "package source prefixes bypass local path resolution",
            {
                values: ["git:github.com/example/repo", "npm:@scope/pkg"],
                localPathPolicy: "isLocalPath",
            },
            pathCases.packageSources,
        ),
        helpers.scenario(
            "explicit resource paths load prompts, skills and themes",
            {
                promptPath: "resources/prompts",
                skillPath: "resources/skills",
                themePath: "resources/themes",
                discoveryFlags: ["noSkills", "noPromptTemplates", "noThemes", "noExtensions"],
            },
            resourceSelection,
        ),
    ];
}

async function captureExtension(workspacePath: string, agentDirectory: string, home: string) {
    const { loadExtensionFromFactory, createExtensionRuntime } = await helpers.importPi(
        "packages/coding-agent/src/core/extensions/index.ts",
    );
    const { createEventBus } = await helpers.importPi("packages/coding-agent/src/core/event-bus.ts");
    const { fauxAssistantMessage, fauxToolCall } = await helpers.importPi("packages/ai/src/compat.ts");

    mkdirSync(workspacePath, { recursive: true });
    writeFileSync(path.join(workspacePath, "note.txt"), "tool content\n");
    const record: any = { invocations: [], api: undefined };
    const factory = (pi: any) => {
        record.api = pi;
        pi.on("before_agent_start", async (event: any, context: any) => {
            record.invocations.push({
                phase: event.messageType === "user" ? "user-prompt" : "extension-triggered",
                isIdle: context.isIdle(),
                messageType: event.messageType,
                prompt: event.messageType === "user" ? event.prompt : null,
                customText: event.messageType === "custom" ? helpers.messageText(event.message) : null,
            });
            return {
                systemPrompt: `${event.systemPrompt}\nAUDIT_1049_SYSTEM_CONTRIBUTION`,
                message: {
                    customType: "audit-1049-contribution",
                    content: [{ type: "text", text: "audit-1049 custom contribution" }],
                    display: true,
                },
            };
        });
    };
    const eventBus = createEventBus();
    const extensionRuntime = createExtensionRuntime();
    const extension = await loadExtensionFromFactory(
        factory,
        workspacePath,
        eventBus,
        extensionRuntime,
        "<inline:audit-1049>",
    );
    const baseLoader = await helpers.createResourceLoaderStub();
    const extensionsResult = { extensions: [extension], errors: [], runtime: extensionRuntime };
    const resourceLoader = { ...baseLoader, getExtensions: () => extensionsResult };
    const modelHarness = await helpers.createModelHarness();
    const { session } = await helpers.createSession({
        workspace: workspacePath,
        agentDir: agentDirectory,
        modelHarness,
        resourceLoader,
    });

    const requests: any[] = [];
    const captureRequest = (context: any) => {
        const messages = context.messages;
        const system = messages.find((message: any) => message.role === "system");
        requests.push({
            roles: messages.map((message: any) => message.role),
            hasSystemContribution: helpers.systemText(system).includes("AUDIT_1049_SYSTEM_CONTRIBUTION"),
            contributionCount: messages.filter((message: any) =>
                helpers.messageText(message).includes("audit-1049 custom contribution"),
            ).length,
            hasToolResult: messages.some((message: any) => message.role === "toolResult"),
            toolResultText:
                messages
                    .filter((message: any) => message.role === "toolResult")
                    .map((message: any) => helpers.messageText(message))
                    .at(-1) ?? null,
        });
    };
    modelHarness.faux.setResponses([
        (context: any) => {
            captureRequest(context);
            return fauxAssistantMessage(fauxToolCall("read", { path: "note.txt" }), { stopReason: "toolUse" });
        },
        (context: any) => {
            captureRequest(context);
            return fauxAssistantMessage("first run complete");
        },
    ]);
    await session.prompt("start");
    const idleAfterFirstRun = session.isIdle;
    const firstRunMessages = session.messages.map((message: any) => ({
        role: message.role,
        customType: message.customType ?? null,
        text: helpers.messageText(message),
    }));

    const settled = () =>
        new Promise<void>((resolve) => {
            const unsubscribe = session.subscribe((event: any) => {
                if (event.type === "agent_settled") {
                    unsubscribe();
                    resolve();
                }
            });
        });
    modelHarness.faux.setResponses([
        (context: any) => {
            captureRequest(context);
            return fauxAssistantMessage("extension-triggered run complete");
        },
    ]);
    const secondSettlement = settled();
    record.api.sendMessage(
        { customType: "audit-1049-trigger", content: "extension-triggered", display: true },
        { triggerTurn: true },
    );
    await secondSettlement;
    const finalMessages = session.messages.map((message: any) => ({
        role: message.role,
        customType: message.customType ?? null,
        text: helpers.messageText(message),
    }));
    const idleAfterTriggeredRun = session.isIdle;

    session.dispose();
    modelHarness.dispose();
    void home;

    return [
        helpers.scenario(
            "before_agent_start observes busy state and contributes prompt plus custom message",
            {
                extension: "inline before_agent_start handler",
                firstPrompt: "start",
                handlerReturn: ["systemPrompt replacement", "custom message"],
                workspace: "controlled temporary workspace",
            },
            {
                invocations: record.invocations,
                firstRunRequest: requests[0],
                idleAfterFirstRun,
                firstRunMessages,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/suite/agent-session-model-extension.test.ts",
                    case: "allows before_agent_start handlers to inject custom messages and modify the system prompt",
                },
            },
        ),
        helpers.scenario(
            "extension-triggered run starts from an idle session and retains contributions across a tool turn",
            {
                trigger: "pi.sendMessage({ triggerTurn: true })",
                triggerCustomType: "audit-1049-trigger",
                priorToolTurn: "read note.txt",
                workspace: "controlled temporary workspace",
            },
            {
                triggerInvocation: record.invocations[1] ?? null,
                priorRunRequest: requests[1] ?? null,
                triggeredRunRequest: requests[2] ?? null,
                finalMessages,
                isIdleAfterTriggeredRun: idleAfterTriggeredRun,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/suite/agent-session-prompt.test.ts",
                    case: "sendUserMessage while idle triggers a turn",
                },
            },
        ),
    ];
}

async function captureConfiguration(workspacePath: string, agentDirectory: string, home: string) {
    const { SettingsSchema } = await helpers.importPi("packages/coding-agent/src/core/settings-schema.ts");
    const { SettingsManager } = await helpers.importPi("packages/coding-agent/src/core/settings-manager.ts");
    const { KeybindingsManager } = await helpers.importPi("packages/coding-agent/src/core/keybindings.ts");
    const { KeybindingsSchema } = await helpers.importPi("packages/coding-agent/src/core/keybindings-schema.ts");
    const { validateThemeJson } = await helpers.importPi(
        "packages/coding-agent/src/modes/interactive/theme/theme-schema.ts",
    );
    const { loadThemeFromPath } = await helpers.importPi(
        "packages/coding-agent/src/modes/interactive/theme/theme.ts",
    );
    const { THEME_TOKENS } = await helpers.importPi(
        "packages/coding-agent/src/modes/interactive/theme/theme-tokens.ts",
    );
    const { Compile } = requirePi("typebox/compile");
    const settingsSchema = Compile(SettingsSchema);
    const keybindingsSchema = Compile(KeybindingsSchema);

    const settingsDir = path.join(workspacePath, "configuration", "settings");
    mkdirSync(settingsDir, { recursive: true });
    const initialSettings = {
        defaultProvider: "magpie",
        defaultModel: "group/model",
        defaultThinkingLevel: "high",
        enabledModels: ["group/a", "group/b"],
        packages: ["npm:@scope/a", "git:github.com/example/repo"],
        extensions: ["./ext"],
        skills: ["./skills"],
        prompts: ["./prompts"],
        themes: ["./themes"],
        compaction: { enabled: true, reserveTokens: 1234 },
        retry: { enabled: false, maxRetries: 0 },
    };
    writeJson(path.join(settingsDir, "settings.json"), initialSettings);
    const settings = SettingsManager.create(workspacePath, settingsDir);
    const initialSettingsObservation = {
        defaultProvider: settings.getSettings().defaultProvider,
        defaultModel: settings.getSettings().defaultModel,
        defaultThinkingLevel: settings.getSettings().defaultThinkingLevel,
        enabledModels: settings.getEnabledModels?.() ?? [],
        packages: settings.getPackages(),
        extensionPaths: settings.getExtensionPaths(),
        skillPaths: settings.getSkillPaths(),
        promptPaths: settings.getPromptTemplatePaths(),
        themePaths: settings.getThemePaths(),
        compactionReserveTokens: settings.getCompactionSettings?.().reserveTokens ?? null,
        retryMaxRetries: settings.getRetrySettings?.().maxRetries ?? null,
    };
    const updatedSettings = {
        ...initialSettings,
        defaultModel: "group/model2",
        packages: ["npm:@scope/b"],
    };
    writeJson(path.join(settingsDir, "settings.json"), updatedSettings);
    await settings.reload();
    const reloadedSettingsObservation = {
        defaultModel: settings.getSettings().defaultModel,
        packages: settings.getPackages(),
    };

    const invalidSettingsDir = path.join(workspacePath, "configuration", "settings-invalid");
    mkdirSync(invalidSettingsDir, { recursive: true });
    writeJson(path.join(invalidSettingsDir, "settings.json"), {
        defaultThinkingLevel: "bogus",
        packages: "not-array",
    });
    const invalidSettings = SettingsManager.create(workspacePath, invalidSettingsDir);
    const invalidSettingsObservation = {
        defaultThinkingLevel: invalidSettings.getSettings().defaultThinkingLevel,
        packages: invalidSettings.getPackages(),
        schemaCheck: settingsSchema.Check({ defaultThinkingLevel: "bogus", packages: "not-array" }),
    };

    const keyDir = path.join(workspacePath, "configuration", "keybindings");
    mkdirSync(keyDir, { recursive: true });
    const keybindingsPath = path.join(keyDir, "keybindings.json");
    const keybindingsInput = {
        "app.interrupt": "ctrl+q",
        "app.clear": "ctrl+q",
        "app.unknown": "ctrl+u",
        cursorUp: "ctrl+u",
        "app.model.select": "not-a-key",
    };
    writeFileSync(keybindingsPath, `\uFEFF${JSON.stringify(keybindingsInput)}`);
    const keybindings = KeybindingsManager.create(keyDir);
    const selectedKeybindings = (manager: any) => {
        const effective = manager.getEffectiveConfig();
        return {
            catalogSize: Object.keys(effective).length,
            appInterrupt: effective["app.interrupt"],
            appClear: effective["app.clear"],
            cursorUp: effective["tui.editor.cursorUp"],
            appUnknownPresent: Object.hasOwn(effective, "app.unknown"),
            userBindings: manager.getUserBindings(),
            conflicts: manager.getConflicts(),
        };
    };
    const keybindingsInitial = selectedKeybindings(keybindings);
    writeJson(keybindingsPath, { "app.interrupt": "escape" });
    keybindings.reload();
    const keybindingsReloaded = selectedKeybindings(keybindings);

    const themeDir = path.join(workspacePath, "configuration", "themes");
    mkdirSync(themeDir, { recursive: true });
    const darkTheme = readJson(
        path.join(piCheckout, "packages/coding-agent/src/modes/interactive/theme/dark.json"),
    );
    darkTheme.name = "audit-1049-dark";
    const themePath = path.join(themeDir, "audit-1049-dark.json");
    writeJson(themePath, darkTheme);
    const validThemeObservation = {
        schemaAccepted: true,
        tokenCount: 0,
        requiredTokenCount: 0,
        optionalTokens: [] as string[],
        runtimeAccepted: false,
        runtimeName: null as string | null,
        runtimeColorCount: 0,
    };
    try {
        const validated = validateThemeJson("audit-valid", darkTheme);
        validThemeObservation.tokenCount = Object.keys(validated.colors).length;
        validThemeObservation.requiredTokenCount = Object.entries(THEME_TOKENS).filter(
            ([, descriptor]: [string, any]) => descriptor.fallback === undefined,
        ).length;
        validThemeObservation.optionalTokens = Object.entries(THEME_TOKENS)
            .filter(([, descriptor]: [string, any]) => descriptor.fallback !== undefined)
            .map(([name]) => name);
        const loaded = loadThemeFromPath(themePath);
        validThemeObservation.runtimeAccepted = true;
        validThemeObservation.runtimeName = loaded.name;
        validThemeObservation.runtimeColorCount = Object.keys(loaded.colors).length;
    } catch (error) {
        validThemeObservation.schemaAccepted = false;
        validThemeObservation.runtimeAccepted = false;
    }

    const unknownTheme = structuredClone(darkTheme);
    unknownTheme.colors.bogusToken = "#000000";
    const unknownThemePath = path.join(themeDir, "audit-unknown.json");
    writeJson(unknownThemePath, unknownTheme);
    const unknownThemeObservation = observeTheme(
        "audit-unknown",
        unknownTheme,
        unknownThemePath,
        validateThemeJson,
        loadThemeFromPath,
    );

    const missingTheme = structuredClone(darkTheme);
    delete missingTheme.colors.text;
    const missingThemePath = path.join(themeDir, "audit-missing.json");
    writeJson(missingThemePath, missingTheme);
    const missingThemeObservation = observeTheme(
        "audit-missing",
        missingTheme,
        missingThemePath,
        validateThemeJson,
        loadThemeFromPath,
    );

    const slashTheme = structuredClone(darkTheme);
    slashTheme.name = "audit/name";
    const slashThemePath = path.join(themeDir, "audit-slash.json");
    writeJson(slashThemePath, slashTheme);
    const slashThemeObservation = observeTheme(
        "audit/name",
        slashTheme,
        slashThemePath,
        validateThemeJson,
        loadThemeFromPath,
    );

    void home;
    return [
        helpers.scenario(
            "settings schema accepts the supported shape and unknown top-level keys",
            {
                supportedKeys: [
                    "defaultProvider",
                    "defaultModel",
                    "defaultThinkingLevel",
                    "enabledModels",
                    "packages",
                    "extensions",
                    "skills",
                    "prompts",
                    "themes",
                    "compaction",
                    "retry",
                ],
                unknownTopLevel: true,
            },
            {
                supportedCheck: settingsSchema.Check(initialSettings),
                unknownTopLevelCheck: settingsSchema.Check({ ...initialSettings, futureSetting: true }),
            },
        ),
        helpers.scenario(
            "settings schema rejects invalid thinking level and negative compaction reserve",
            {
                invalidThinking: { defaultThinkingLevel: "bogus" },
                negativeReserve: { compaction: { reserveTokens: -1 } },
            },
            {
                invalidThinkingCheck: settingsSchema.Check({ defaultThinkingLevel: "bogus" }),
                negativeReserveCheck: settingsSchema.Check({ compaction: { reserveTokens: -1 } }),
            },
        ),
        helpers.scenario(
            "settings manager reads package and local resource paths and reloads changed values",
            {
                initialSettings,
                updatedSettings,
                settingsPath: "controlled temporary agent settings.json",
            },
            {
                initial: initialSettingsObservation,
                afterReload: reloadedSettingsObservation,
            },
        ),
        helpers.scenario(
            "settings manager consumes values the published schema rejects",
            {
                storedSettings: { defaultThinkingLevel: "bogus", packages: "not-array" },
                schema: "SettingsSchema",
            },
            invalidSettingsObservation,
        ),
        helpers.scenario(
            "keybindings manager migrates legacy names, keeps unknown actions, drops invalid values and reloads",
            {
                initialConfig: keybindingsInput,
                bom: true,
                reloadConfig: { "app.interrupt": "escape" },
            },
            {
                initial: keybindingsInitial,
                afterReload: keybindingsReloaded,
            },
        ),
        helpers.scenario(
            "keybindings schema accepts unknown action ids with valid keys and rejects malformed values",
            {
                validKnownAction: { "app.interrupt": "ctrl+q" },
                unknownAction: { "future.action": "ctrl+u" },
                malformedValue: { "app.model.select": "not-a-key" },
            },
            {
                validKnownActionCheck: keybindingsSchema.Check({ "app.interrupt": "ctrl+q" }),
                unknownActionCheck: keybindingsSchema.Check({ "future.action": "ctrl+u" }),
                malformedValueCheck: keybindingsSchema.Check({ "app.model.select": "not-a-key" }),
            },
        ),
        helpers.scenario(
            "theme schema accepts the 56-token built-in shape",
            {
                theme: "dark.json with audit name",
                tokenSource: "packages/coding-agent/src/modes/interactive/theme/theme-tokens.ts",
            },
            validThemeObservation,
        ),
        helpers.scenario(
            "theme schema rejects an unknown color token while runtime loading throws",
            {
                colors: "valid dark theme plus bogusToken",
                schema: "validateThemeJson",
                runtime: "loadThemeFromPath",
            },
            unknownThemeObservation,
        ),
        helpers.scenario(
            "theme runtime accepts a missing required token and a slash name that the schema rejects",
            {
                missingColor: "text",
                slashName: "audit/name",
                schema: "validateThemeJson",
                runtime: "loadThemeFromPath",
            },
            {
                missing: missingThemeObservation,
                slashName: slashThemeObservation,
            },
        ),
    ];
}

function observeTheme(
    label: string,
    theme: any,
    themePath: string,
    validateThemeJson: (label: string, json: unknown) => any,
    loadThemeFromPath: (themePath: string) => any,
) {
    const observation = {
        label,
        schemaAccepted: false,
        schemaErrorMentionsLabel: false,
        schemaErrorMentionsToken: false,
        runtimeAccepted: false,
        runtimeErrorName: null as string | null,
        runtimeErrorMessageIncludesSlot: false,
        runtimeName: null as string | null,
        runtimeColorCount: 0,
    };
    try {
        validateThemeJson(label, theme);
        observation.schemaAccepted = true;
    } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        observation.schemaErrorMentionsLabel = message.includes(label);
        observation.schemaErrorMentionsToken =
            message.includes("bogusToken") || message.includes("text");
    }
    try {
        const loaded = loadThemeFromPath(themePath);
        observation.runtimeAccepted = true;
        observation.runtimeName = loaded.name;
        observation.runtimeColorCount = Object.keys(loaded.colors).length;
    } catch (error) {
        observation.runtimeErrorName = error instanceof Error ? error.name : null;
        observation.runtimeErrorMessageIncludesSlot = String(error).includes("slot");
    }
    return observation;
}

async function capturePrintMode() {
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const child = path.join(familyDir, "print-mode-child.mts");
    const run = (scenario: "truncated" | "complete") => {
        const resultPath = path.join(root, `print-${scenario}.json`);
        const spawned = spawnSync(tsx, [child, scenario, resultPath], {
            encoding: "utf8",
            timeout: 180000,
            maxBuffer: 10 * 1024 * 1024,
            env: {
                ...process.env,
                PI_CHECKOUT: piCheckout,
                TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json"),
            },
        });
        if (spawned.error) throw spawned.error;
        if (spawned.status === null) {
            throw new Error(`print child ${scenario} did not exit: ${spawned.signal ?? "unknown signal"}`);
        }
        return {
            status: spawned.status,
            stdout: spawned.stdout,
            stderr: spawned.stderr,
            result: readJson(resultPath),
        };
    };
    const truncated = run("truncated");
    const complete = run("complete");
    return [
        helpers.scenario(
            "length-truncated text response prints partial stdout, warning stderr and exits 1",
            {
                mode: "text",
                scriptedResponse: "partial answer",
                scriptedStopReason: "length",
                modelMaxTokens: truncated.result.modelMaxTokens,
            },
            {
                processExitCode: truncated.status,
                stdout: truncated.stdout,
                stderr: truncated.stderr,
                finalAssistant: truncated.result.finalAssistant,
                sessionRoles: truncated.result.sessionRoles,
            },
            {
                upstreamTest: {
                    path: "packages/coding-agent/test/print-mode.test.ts",
                    case: "warns on stderr and returns non-zero when the response was truncated",
                },
            },
        ),
        helpers.scenario(
            "complete text response prints final text and exits 0",
            {
                mode: "text",
                scriptedResponse: "complete answer",
                scriptedStopReason: "stop",
                modelMaxTokens: complete.result.modelMaxTokens,
            },
            {
                processExitCode: complete.status,
                stdout: complete.stdout,
                stderr: complete.stderr,
                finalAssistant: complete.result.finalAssistant,
                sessionRoles: complete.result.sessionRoles,
            },
        ),
    ];
}
