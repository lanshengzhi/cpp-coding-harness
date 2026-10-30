/**
 * Frozen pi v1.3.0 modules for the `audit-1093` deterministic-run
 * outcome/settlement ordering capture.
 *
 * The capture lives in the C++ harness repository, but the measured code is
 * the private checkout named by `PI_CHECKOUT`. Loading by absolute file URL
 * keeps every import inside that checkout, so the manifest revision guard is
 * the authority for what was executed.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the private pi v1.3.0 checkout");

async function load(relativePath: string): Promise<Record<string, any>> {
    return (await import(pathToFileURL(path.join(checkout!, relativePath)).href)) as Record<string, any>;
}

const codemode = await load("packages/codemode/src/index.ts");

export const CodemodeSandbox = codemode.CodemodeSandbox;
export const loadQuickJSWasm = codemode.loadQuickJSWasm;

/** One frozen observation: the comparison unit a later C++ test must reproduce. */
export interface Scenario {
    name: string;
    source: string;
    inputs: Record<string, unknown>;
    expected: Record<string, unknown>;
    observed: Record<string, unknown>;
}
