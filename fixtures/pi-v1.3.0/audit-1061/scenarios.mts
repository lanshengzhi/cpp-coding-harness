/**
 * Frozen pi v1.3.0 `Tracker.prepareReplace` observations for ticket #1061.
 *
 * The capture lives in the Pike repository but executes the private checkout
 * named by `PI_CHECKOUT`; `capture-1061.mts` guards its exact revision.
 */

import { existsSync } from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

export interface Scenario {
    readonly name: string;
    readonly source: string;
    readonly inputs: unknown;
    readonly expected: unknown;
    readonly normalizations?: readonly string[];
}

/* eslint-disable @typescript-eslint/no-explicit-any */
type Any = any;

const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") throw new Error("PI_CHECKOUT must name the frozen pi v1.3.0 checkout");

const deltaPath = path.join(checkout, "packages/chord/src/delta/index.ts");
if (!existsSync(deltaPath)) throw new Error(`missing frozen Chord delta source: ${deltaPath}`);
const delta = (await import(pathToFileURL(deltaPath).href)) as Any;

const track = delta.track as (initial: Any) => Any;

function jsonClone(value: Any): Any {
    return JSON.parse(JSON.stringify(value));
}

function errorShape(error: Any): { name: string; message: string } {
    return { name: String(error?.name ?? "Error"), message: String(error?.message ?? error) };
}

export async function prepareReplaceScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const tracker = track({ value: 1, items: ["a"] });
        const before = jsonClone(tracker.value);
        const replacement = { value: 2, items: ["a"] };
        const prepared = tracker.prepareReplace(replacement);
        const preparedBase = jsonClone(prepared.base);
        const preparedValue = jsonClone(prepared.value);
        const ownsReplacement = prepared.value === replacement;
        const ops = jsonClone(prepared.ops);
        const baseRevision = prepared.baseRevision;
        const revisionBefore = tracker.revision;
        const baseSame = prepared.base === tracker.value;

        const competitor = tracker.beginChange();
        competitor.state.value = 3;
        const competitorPrepared = competitor.prepare();

        tracker.adopt(prepared);
        let staleError: Any;
        try {
            tracker.adopt(competitorPrepared);
        } catch (error) {
            staleError = errorShape(error);
        }
        let usedError: Any;
        try {
            tracker.adopt(prepared);
        } catch (error) {
            usedError = errorShape(error);
        }

        assertValue("after adoption", jsonClone(tracker.value), preparedValue);
        assertValue("base remains", jsonClone(prepared.base), before);
        scenarios.push({
            name: "a prepared whole-root replacement keeps the prior revision as its base and stales a same-base preparation after adoption",
            source:
                "packages/chord/test/delta.test.ts:takes immutable ownership of replacement input and applies no-op normalization; " +
                "packages/chord/test/delta.test.ts:rejects foreign, stale, and repeated preparations while allowing competing changes; " +
                "packages/chord/src/delta/tracker.ts:Tracker.prepareReplace,Tracker.adopt",
            inputs: {
                initial: before,
                replacement: { value: 2, items: ["a"] },
                competingDraftValue: 3,
            },
            expected: {
                preparedBase,
                preparedValue,
                ops,
                baseRevision,
                revisionBefore,
                baseSame,
                ownsReplacement,
                revisionAfter: tracker.revision,
                valueAfter: jsonClone(tracker.value),
                preparedBaseAfter: jsonClone(prepared.base),
                staleError,
                usedError,
            },
            normalizations: [
                "`baseSame` is JavaScript identity; the C++ comparison may use the recorded immutable base/value and the exact stale/used errors.",
            ],
        });
    }

    {
        const tracker = track({ value: 1, items: ["a"] });
        const replacement = { value: 1, items: ["a"] };
        const prepared = tracker.prepareReplace(replacement);
        const valueSame = prepared.value === tracker.value;
        const baseSame = prepared.base === tracker.value;
        const ownsReplacement = prepared.value === replacement;
        const competitor = tracker.prepareReplace({ value: 1, items: ["a"] });
        tracker.adopt(prepared);
        let staleError: Any;
        try {
            tracker.adopt(competitor);
        } catch (error) {
            staleError = errorShape(error);
        }
        scenarios.push({
            name: "a deeply equal whole-root replacement emits no operations and still stales a same-base preparation",
            source:
                "packages/chord/test/delta.test.ts:takes immutable ownership of replacement input and applies no-op normalization; " +
                "packages/chord/test/delta.test.ts:makes competing same-base preparations stale after adopting a no-op; " +
                "packages/chord/src/delta/tracker.ts:Tracker.prepareReplace",
            inputs: { initial: { value: 1, items: ["a"] }, replacement: { value: 1, items: ["a"] } },
            expected: {
                ops: jsonClone(prepared.ops),
                valueSame,
                baseSame,
                ownsReplacement,
                revision: tracker.revision,
                value: jsonClone(tracker.value),
                staleError,
            },
        });
    }

    {
        const tracker = track({ value: 1 });
        const prepared = tracker.prepareReplace({ value: 2 });
        prepared.abort();
        let abortedError: Any;
        try {
            tracker.adopt(prepared);
        } catch (error) {
            abortedError = errorShape(error);
        }
        scenarios.push({
            name: "an aborted whole-root preparation cannot be adopted",
            source:
                "packages/chord/src/delta/tracker.ts:Prepared.abort,Tracker.adopt; " +
                "packages/chord/test/delta.test.ts:invalidates a prepared result when its change is aborted",
            inputs: { initial: { value: 1 }, replacement: { value: 2 } },
            expected: {
                abortedError,
                revision: tracker.revision,
                value: jsonClone(tracker.value),
            },
        });
    }

    return scenarios;
}

function assertValue(label: string, actual: Any, expected: Any): void {
    const actualText = JSON.stringify(actual);
    const expectedText = JSON.stringify(expected);
    if (actualText !== expectedText) throw new Error(`${label}: ${actualText} != ${expectedText}`);
}

export async function draftArrayHoleScenarios(): Promise<Scenario[]> {
    const tracker = track({ items: ["a", "b"] });
    const change = tracker.beginChange();
    let deleteError: Any;
    try {
        delete change.state.items[0];
    } catch (error) {
        deleteError = errorShape(error);
    }
    const prepared = change.prepare();
    return [
        {
            name: "deleting an array element from a draft rejects the hole and leaves the prepared revision empty",
            source:
                "packages/chord/src/delta/tracker.ts:deleteProperty",
            inputs: { initial: { items: ["a", "b"] }, deletedIndex: 0 },
            expected: {
                deleteError,
                ops: jsonClone(prepared.ops),
                value: jsonClone(prepared.value),
                revision: tracker.revision,
            },
        },
    ];
}
