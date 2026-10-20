/**
 * Frozen pi v1.3.0 observations added by the #1061 review repair.
 *
 * These scenarios distinguish mutable application, draft-read absence, adoption
 * revocation and operation folding. `capture-1061.mts` guards the exact
 * revision before executing them.
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

const apply = delta.apply as (target: Any, ops: readonly Any[]) => Any;
const track = delta.track as (initial: Any) => Any;

function jsonClone(value: Any): Any {
    return JSON.parse(JSON.stringify(value));
}

function errorShape(error: Any): { name: string; message: string } {
    return { name: String(error?.name ?? "Error"), message: String(error?.message ?? error) };
}

function read(value: () => Any): unknown {
    try {
        const result = value();
        if (result === undefined) return { type: "undefined" };
        return { type: "value", value: jsonClone(result) };
    } catch (error) {
        return { type: "error", error: errorShape(error) };
    }
}

export async function mutableApplyScenarios(): Promise<Scenario[]> {
    const initial = { value: 1, items: ["a"] };
    const target = jsonClone(initial);
    let pathError: Any;
    try {
        apply(target, [
            ["s", ["value"], 2],
            ["s", ["missing", "value"], 20],
        ]);
    } catch (error) {
        pathError = errorShape(error);
    }
    return [
        {
            name: "a later traversal failure keeps the preceding mutable write",
            source:
                "packages/chord/src/delta/index.ts:apply,applyOps; " +
                "packages/chord/test/delta.test.ts:mutable application mutates in place except for a root replacement",
            inputs: {
                initial,
                ops: [
                    ["s", ["value"], 2],
                    ["s", ["missing", "value"], 20],
                ],
            },
            expected: {
                targetAfter: jsonClone(target),
                pathError,
            },
            normalizations: [
                "Frozen `PathError` reports the failing path after `unresolvable path: `; C++ records the same failure with `unresolvable path` and leaves the successful leading write applied.",
            ],
        },
    ];
}

export async function draftReadScenarios(): Promise<Scenario[]> {
    const initial = {
        present: 1,
        nil: null,
        object: { present: 2 },
        items: [10],
    };
    const tracker = track(initial);
    const change = tracker.beginChange();
    const state: Any = change.state;
    const beforeSettlement = {
        present: read(() => state.present),
        missingLeaf: read(() => state.missing),
        nullLeaf: read(() => state.nil),
        nestedMissingLeaf: read(() => state.object.missing),
        missingArrayIndex: read(() => state.items[2]),
        missingReceiver: read(() => state.missing.deep),
        nullReceiver: read(() => state.nil.deep),
    };
    change.prepare();
    const afterSettlement = {
        missingLeaf: read(() => state.missing),
    };
    return [
        {
            name: "draft reads distinguish absent leaves from null and from absent receiver paths",
            source:
                "packages/chord/src/delta/tracker.ts:getProperty,getArrayIndex; " +
                "packages/chord/test/delta.test.ts:reads draft state and rejects stale writes",
            inputs: {
                initial,
                paths: {
                    present: ["present"],
                    missingLeaf: ["missing"],
                    nullLeaf: ["nil"],
                    nestedMissingLeaf: ["object", "missing"],
                    missingArrayIndex: ["items", 2],
                    missingReceiver: ["missing", "deep"],
                    nullReceiver: ["nil", "deep"],
                },
            },
            expected: {
                beforeSettlement,
                afterSettlement,
            },
            normalizations: [
                "Frozen absent leaves and an out-of-range array index read as JavaScript `undefined`; C++ represents those successful reads as `std::nullopt`.",
                "Frozen absent or null receivers reject path traversal with a JavaScript TypeError; C++ records the same invalid traversal with its `unresolvable path` error.",
                "Frozen reads after settlement reject with TypeError `Cannot use a settled overlay`; C++ retains that exact error.",
            ],
        },
    ];
}

export async function adoptionInvalidationScenarios(): Promise<Scenario[]> {
    const tracker = track({ x: 1 });
    const open = tracker.beginChange();
    const openDraft: Any = open.state;
    openDraft.x = 5;
    const preparedCompetitor = tracker.beginChange();
    preparedCompetitor.state.x = 6;
    const prepared = preparedCompetitor.prepare();
    const abortedCompetitor = tracker.prepareReplace({ x: 3 });
    const winner = tracker.prepareReplace({ x: 2 });
    tracker.adopt(winner);

    let openStateError: Any;
    try {
        void open.state;
    } catch (error) {
        openStateError = errorShape(error);
    }
    let openWriteError: Any;
    try {
        openDraft.x = 7;
    } catch (error) {
        openWriteError = errorShape(error);
    }
    let openPrepareError: Any;
    try {
        open.prepare();
    } catch (error) {
        openPrepareError = errorShape(error);
    }
    open.abort();

    let preparedAdoptError: Any;
    try {
        tracker.adopt(prepared);
    } catch (error) {
        preparedAdoptError = errorShape(error);
    }

    abortedCompetitor.abort();
    let abortedPreparedAdoptError: Any;
    try {
        tracker.adopt(abortedCompetitor);
    } catch (error) {
        abortedPreparedAdoptError = errorShape(error);
    }

    return [
        {
            name: "adoption immediately revokes competing open drafts while prepared competitors retain normal prepared abort and stale states",
            source:
                "packages/chord/src/delta/tracker.ts:TrackerImpl.adopt,TrackerImpl.#invalidate,assertReadable,assertWritable; " +
                "packages/chord/test/delta.test.ts:rejects foreign, stale, and repeated preparations while allowing competing changes",
            inputs: {
                initial: { x: 1 },
                competingOpenWrite: 5,
                competingPreparedWrite: 6,
                competingReplacement: { x: 3 },
                replacement: { x: 2 },
            },
            expected: {
                openStateError,
                openWriteError,
                openPrepareError,
                preparedAdoptError,
                abortedPreparedAdoptError,
                revision: tracker.revision,
                value: jsonClone(tracker.value),
            },
            normalizations: [
                "Frozen open handles fail immediately with TypeError `Cannot use a settled overlay`; C++ retains that exact error for draft and change operations.",
                "A prepared competitor still reports `Prepared change is stale` when adopted after the winner; aborting it first reports `Prepared change has been aborted`.",
            ],
        },
    ];
}

export async function operationFoldingScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const tracker = track({ a: 1 });
        const change = tracker.beginChange();
        const state: Any = change.state;
        state.added = { leaf: 3 };
        delete state.added.leaf;
        delete state.added;
        const prepared = change.prepare();
        scenarios.push({
            name: "adding a key and erasing it again normalizes to an empty batch over the unchanged base",
            source:
                "packages/chord/src/delta/tracker.ts:deleteProperty,emitObjectOperations; " +
                "packages/chord/src/delta/tracker.ts:materializePrepared",
            inputs: {
                initial: { a: 1 },
                addedValue: { leaf: 3 },
            },
            expected: {
                ops: jsonClone(prepared.ops),
                value: jsonClone(prepared.value),
                baseSame: prepared.value === tracker.value,
                revision: tracker.revision,
            },
            normalizations: [
                "`baseSame` is JavaScript identity; C++ compares the prepared base and value with `Json::shares_container_with`.",
            ],
        });
    }

    {
        const tracker = track({ a: { nested: { existing: 1 } } });
        const change = tracker.beginChange();
        const state: Any = change.state;
        state.a.nested.existing = 2;
        delete state.a.nested.existing;
        state.a.nested.existing = 1;
        const prepared = change.prepare();
        scenarios.push({
            name: "an existing nested key keeps explicit delete and readd order",
            source:
                "packages/chord/src/delta/tracker.ts:deleteProperty,emitObjectOperations,setProperty",
            inputs: {
                initial: { a: { nested: { existing: 1 } } },
                intermediateValue: 2,
                restoredValue: 1,
            },
            expected: {
                ops: jsonClone(prepared.ops),
                value: jsonClone(prepared.value),
                revision: tracker.revision,
            },
        });
    }

    return scenarios;
}
