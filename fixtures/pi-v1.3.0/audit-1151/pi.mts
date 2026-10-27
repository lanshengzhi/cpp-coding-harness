import { join } from "node:path";
import { pathToFileURL } from "node:url";
const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT required");
const load = async (path: string) => import(pathToFileURL(join(checkout, path)).href);
export const durable = await load("packages/durable/src/index.ts");
export const ai = await load("packages/ai/src/index.ts");
export const { BACKGROUND_CONTEXT: ctx } = await load("packages/chord/src/context/index.ts");
export const { openNodeJsonlStorage } = await load("packages/durable/src/storage/jsonl/node.ts");
