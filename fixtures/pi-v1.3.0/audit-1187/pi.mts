/**
 * Imports the frozen pi v1.3.0 Chord output module named by `PI_CHECKOUT`.
 *
 * The capture lives in the Pike repository, but the measured source is the separate private frozen
 * checkout. Loading by absolute file URL keeps every import inside that checkout, so the manifest's
 * revision guard is the authority for what was executed.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/pi-1113-frozen";

async function load(relativePath: string): Promise<Record<string, any>> {
    return (await import(pathToFileURL(path.join(checkout, relativePath)).href)) as Record<string, any>;
}

const output = await load("packages/chord/src/output.ts");

export const Progress = output.Progress;
export const PROGRESS_BYTES_PER_SECOND = output.PROGRESS_BYTES_PER_SECOND;
