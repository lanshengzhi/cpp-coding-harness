// Read-only probe of frozen NodeExecutionEnv DirReader's exact-full-page behavior.
// Run with the frozen checkout's tsx and its tsconfig; this does not register an audit family.
import { execFileSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

const checkout = process.env.PI_CHECKOUT ?? "/tmp/pi-1113-frozen";
const revision = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const git = (...args: string[]) => execFileSync("git", ["-C", checkout, ...args], { encoding: "utf8" }).trim();
if (git("rev-parse", "HEAD") !== revision || git("status", "--porcelain") !== "") {
	throw new Error("probe requires the clean frozen pi checkout at " + revision);
}
const source = (relative: string) => pathToFileURL(path.join(checkout, relative)).href;
const { BACKGROUND_CONTEXT } = await import(source("packages/chord/src/context/index.ts"));
const { NodeExecutionEnv } = await import(source("packages/durable/src/env/node.ts"));
const { getOrThrow } = await import(source("packages/durable/src/env/index.ts"));
const root = mkdtempSync(path.join(tmpdir(), "cch-1158-exact-"));
try {
	for (const name of ["a", "b", "c", "d"]) writeFileSync(path.join(root, name), "");
	const env = new NodeExecutionEnv({ cwd: root });
	const reader = getOrThrow(await env.openDirReader(".", BACKGROUND_CONTEXT));
	const pages = [];
	try {
		for (let index = 0; index < 3; index++) {
			const page = getOrThrow(await reader.next(2, BACKGROUND_CONTEXT));
			pages.push({ names: page.entries.map((entry: { name: string }) => entry.name).sort(), done: page.done });
		}
	} finally {
		await reader.close(BACKGROUND_CONTEXT);
	}
	console.log(JSON.stringify({ revision, files: 4, pageSize: 2, pages }));
} finally {
	rmSync(root, { recursive: true, force: true });
}
