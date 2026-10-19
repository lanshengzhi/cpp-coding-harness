/**
 * Frozen pi v1.3.0 public modules for the `audit-1056` durable Harness capture.
 *
 * The capture lives in the Pike repository, but the measured code is the private checkout named by
 * `PI_CHECKOUT`. Loading by absolute file URL keeps every import inside that checkout, so the
 * manifest revision guard is the authority for what was executed.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the private pi v1.3.0 checkout");

async function load(relativePath: string): Promise<Record<string, any>> {
    return (await import(pathToFileURL(path.join(checkout!, relativePath)).href)) as Record<string, any>;
}

const durable = await load("packages/durable/src/index.ts");
const durableSqlite = await load("packages/durable/src/storage/sqlite/node.ts");
const durableJsonl = await load("packages/durable/src/storage/jsonl/node.ts");
const envNode = await load("packages/durable/src/env/node.ts");
const tools = await load("packages/durable/src/tools/index.ts");
const imagesNode = await load("packages/durable/src/images/node.ts");
const durableCodemode = await load("packages/durable/src/codemode/index.ts");
const durableToolSearch = await load("packages/durable/src/tool-search/index.ts");
const codemode = await load("packages/codemode/src/index.ts");
const ai = await load("packages/ai/src/index.ts");
const chordContext = await load("packages/chord/src/context/index.ts");

export const Harness = durable.Harness;
export const MemoryStorage = durable.MemoryStorage;
export const createRegistry = durable.createRegistry;
export const createSession = durable.createSession;
export const defineDoc = durable.defineDoc;
export const defineEntry = durable.defineEntry;
export const defineTask = durable.defineTask;
export const defineExtension = durable.defineExtension;
export const defineTool = durable.defineTool;
export const hook = durable.hook;
export const section = durable.section;
export const wrapSection = durable.wrapSection;
export const wrapTool = durable.wrapTool;
export const configure = durable.configure;
export const AgentDoc = durable.AgentDoc;
export const InboxDoc = durable.InboxDoc;
export const LiveDoc = durable.LiveDoc;
export const ProviderDoc = durable.ProviderDoc;
export const UsageDoc = durable.UsageDoc;
export const ResetEntry = durable.ResetEntry;
export const UserEntry = durable.UserEntry;
export const AssistantEntry = durable.AssistantEntry;
export const SystemEntry = durable.SystemEntry;
export const ToolResultEntry = durable.ToolResultEntry;
export const CompactionEntry = durable.CompactionEntry;
export const GenerationTask = durable.GenerationTask;
export const ToolTask = durable.ToolTask;
export const CompactionTask = durable.CompactionTask;
export const DEFAULT_COMPACTION_POLICY = durable.DEFAULT_COMPACTION_POLICY;
export const DEFAULT_PROGRESS_POLICY = durable.DEFAULT_PROGRESS_POLICY;
export const DEFAULT_RETRY_POLICY = durable.DEFAULT_RETRY_POLICY;
export const watchEvents = durable.watchEvents;
export const ConversationBusy = durable.ConversationBusy;

export const openNodeSqliteStorage = durableSqlite.openNodeSqliteStorage;
export const openNodeJsonlStorage = durableJsonl.openNodeJsonlStorage;
export const NodeExecutionEnv = envNode.NodeExecutionEnv;
export const createCodingTools = tools.createCodingTools;
export const CodingTools = tools.CodingTools;
export const createReadTool = tools.createReadTool;
export const createWriteTool = tools.createWriteTool;
export const createEditTool = tools.createEditTool;
export const createBashTool = tools.createBashTool;
export const DEFAULT_IMAGE_LIMITS = tools.DEFAULT_IMAGE_LIMITS;
export const createNodePhotonImages = imagesNode.createNodePhotonImages;

export const createCodemodeTool = durableCodemode.createCodemodeTool;
export const createCodemodeExtension = durableCodemode.createCodemodeExtension;
export const CODEMODE_TOOL_NAME = durableCodemode.CODEMODE_TOOL_NAME;
export const CodemodeRunDoc = durableCodemode.CodemodeRunDoc;
export const CodemodeJournalDoc = durableCodemode.CodemodeJournalDoc;
export const CodemodeStoreDoc = durableCodemode.CodemodeStoreDoc;
export const createToolSearchExtension = durableToolSearch.createToolSearchExtension;
export const createToolSearchTool = durableToolSearch.createToolSearchTool;
export const TOOL_SEARCH_TOOL_NAME = durableToolSearch.TOOL_SEARCH_TOOL_NAME;
export const DEFAULT_TOOL_SEARCH_LIMIT = durableToolSearch.DEFAULT_TOOL_SEARCH_LIMIT;

export const CodemodeSandbox = codemode.CodemodeSandbox;
export const InlineCodemodeSandbox = codemode.InlineCodemodeSandbox;
export const DEFAULT_INTERRUPT_BUDGET = codemode.DEFAULT_INTERRUPT_BUDGET;
export const parseCodemodeSource = codemode.parseCodemodeSource;

export const createModels = ai.createModels;
export const fauxProvider = ai.fauxProvider;
export const fauxAssistantMessage = ai.fauxAssistantMessage;
export const fauxText = ai.fauxText;
export const fauxToolCall = ai.fauxToolCall;
export const Type = ai.Type;
export const BACKGROUND_CONTEXT = chordContext.BACKGROUND_CONTEXT;

/** One frozen observation: the comparison unit a later C++ test must reproduce. */
export interface Scenario {
    name: string;
    source: string;
    inputs: Record<string, unknown>;
    expected: unknown;
    normalizations?: string[];
}
