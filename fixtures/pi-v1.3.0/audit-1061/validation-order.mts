/**
 * Frozen pi v1.3.0 operation-validation observations for ticket #1061.
 *
 * `cch_chord:delta:op-validation` asks for path and operation validation, and
 * `cch_chord:delta:path-validation` for unsafe-path rejection. The 1057 capture
 * recorded only a whole-batch rejection ("rejected before mutation") without
 * saying what a *leading* valid operation leaves behind when a later operation
 * is rejected. Frozen Chord validates each operation as the loop reaches it, so
 * mutable application keeps successful leading writes while immutable
 * application keeps the base untouched. Root replacement is staged by the
 * applier and does not change the caller-owned root if a later operation fails.
 * These observations are recorded independently because pre-validating an
 * entire mutable batch would hide the partial-write behavior.
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

const apply = delta.apply as (target: Any, ops: readonly Any[]) => Any;
const applyImmutable = delta.applyImmutable as (target: Any, ops: readonly Any[]) => Any;
const assertValidOp = delta.assertValidOp as (op: Any) => void;

function jsonClone(value: Any): Any {
    return JSON.parse(JSON.stringify(value));
}

function errorShape(error: Any): { name: string; message: string } {
    return { name: String(error?.name ?? "Error"), message: String(error?.message ?? error) };
}

/** Run one mutable application, returning the caller-visible target and the rejection. */
function mutate(target: Any, ops: readonly Any[]): { targetAfter: Any; error: Any } {
    let error: Any = null;
    try {
        apply(target, ops);
    } catch (caught) {
        error = errorShape(caught);
    }
    return { targetAfter: jsonClone(target), error };
}

export async function validationOrderScenarios(): Promise<Scenario[]> {
    const scenarios: Scenario[] = [];

    {
        const initial = { value: 1, items: ["a"] };
        const target = jsonClone(initial);
        const { targetAfter, error } = mutate(target, [
            ["s", ["value"], 2],
            ["s", [], 3],
        ]);
        scenarios.push({
            name: "a later empty path keeps the preceding mutable write and reports the empty path",
            source:
                "packages/chord/src/delta/index.ts:apply,applyOps,assertValidOp; " +
                "packages/chord/test/delta.test.ts:rejects unknown verbs, malformed tuples, paths, and splice payloads",
            inputs: {
                initial,
                ops: [
                    ["s", ["value"], 2],
                    ["s", [], 3],
                ],
            },
            expected: { targetAfter, error },
            normalizations: [
                "Frozen validates each operation as the loop reaches it, so the leading set stays applied; a whole-batch pre-validation would leave `targetAfter` at `initial`.",
            ],
        });
    }

    {
        const initial = { value: 1 };
        const target = jsonClone(initial);
        const { targetAfter, error } = mutate(target, [
            ["s", ["value"], 2],
            ["s", ["__proto__", "polluted"], true],
        ]);
        scenarios.push({
            name: "a later unsafe path keeps the preceding mutable write and reports the unsafe segment",
            source: "packages/chord/src/delta/index.ts:apply,applyOps,assertSafePath,UnsafePathError",
            inputs: {
                initial,
                ops: [
                    ["s", ["value"], 2],
                    ["s", ["__proto__", "polluted"], true],
                ],
            },
            expected: { targetAfter, error },
        });
    }

    {
        const initial = { value: 1, items: ["a"] };
        const target = jsonClone(initial);
        const { targetAfter, error } = mutate(target, [
            ["s", ["value"], 2],
            ["p", ["items"], "0", 0, ["x"]],
        ]);
        scenarios.push({
            name: "a later string splice index keeps the preceding mutable write and reports the splice index",
            source: "packages/chord/src/delta/index.ts:apply,applyOps,assertValidOp",
            inputs: {
                initial,
                ops: [
                    ["s", ["value"], 2],
                    ["p", ["items"], "0", 0, ["x"]],
                ],
            },
            expected: { targetAfter, error },
        });
    }

    {
        const initial = { value: 1, a: ["x", "y"] };
        const target = jsonClone(initial);
        const { targetAfter, error } = mutate(target, [
            ["s", ["value"], 2],
            ["m", ["a"], [0, 0]],
        ]);
        scenarios.push({
            name: "a later non-bijective permutation keeps the preceding mutable write and reports the permutation",
            source: "packages/chord/src/delta/index.ts:apply,applyOps,assertValidOp",
            inputs: {
                initial,
                ops: [
                    ["s", ["value"], 2],
                    ["m", ["a"], [0, 0]],
                ],
            },
            expected: { targetAfter, error },
        });
    }

    {
        const initial = { value: 1 };
        const target = jsonClone(initial);
        const { targetAfter, error } = mutate(target, [
            ["r", { other: 2 }],
            ["s", [], 3],
        ]);
        scenarios.push({
            name: "a root replacement adopted before a failing operation leaves the caller object unchanged",
            source: "packages/chord/src/delta/index.ts:apply,applyOps",
            inputs: {
                initial,
                ops: [
                    ["r", { other: 2 }],
                    ["s", [], 3],
                ],
            },
            expected: { targetAfter, error },
            normalizations: [
                "A frozen root replacement is adopted into the applier's local root and never written into the caller's object, so the caller keeps `initial` even though the replacement preceded the failure.",
            ],
        });
    }

    {
        const initial = { value: 1, items: ["a"] };
        const base = jsonClone(initial);
        const ops = [
            ["s", ["value"], 2],
            ["p", ["items"], "0", 0, ["x"]],
        ];
        let error: Any = null;
        try {
            applyImmutable(base, ops);
        } catch (caught) {
            error = errorShape(caught);
        }
        scenarios.push({
            name: "immutable application rejects the same batch and leaves the base unchanged",
            source: "packages/chord/src/delta/index.ts:applyImmutable,applyImmutableBatches,applyOps",
            inputs: { initial, ops },
            expected: { baseAfter: jsonClone(base), error },
        });
    }

    {
        const shapes: Any[] = [
            ["s", [], 1],
            ["d", []],
            ["s", ["a"]],
            ["s", ["a"], 1, 2],
            ["t", ["s"], -1],
            ["p", ["a"], 0, -1, []],
            ["m", ["a"], [0, 0]],
            ["x", ["a"]],
        ];
        const results = shapes.map((op) => {
            try {
                assertValidOp(op);
                return { op: jsonClone(op), accepted: true };
            } catch (caught) {
                return { op: jsonClone(op), accepted: false, error: errorShape(caught) };
            }
        });
        scenarios.push({
            name: "the operation validator rejects each representable malformed shape with its recorded message",
            source:
                "packages/chord/src/delta/index.ts:assertValidOp,assertSafePath; " +
                "packages/chord/test/delta.test.ts:rejects unknown verbs, malformed tuples, paths, and splice payloads",
            inputs: { shapes },
            expected: { results },
            normalizations: [
                "The frozen validator takes untyped tuples; C++ spells the same rejections over its typed operation variant, so `s arity` and `unknown op verb` have no C++ spelling and are recorded as the frozen shape contract only.",
            ],
        });
    }

    return scenarios;
}
