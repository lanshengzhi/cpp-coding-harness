import { join } from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT;
if (!checkout) throw new Error("PI_CHECKOUT is required");
export const { toLlmContent } = await import(pathToFileURL(join(checkout, "packages/mcp/src/protocol/content.ts")).href);
