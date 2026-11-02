/**
 * Scenarios for the `audit-1187` Chord progress-pacing capture.
 *
 * The frozen `Progress` class paces commits with real timers. A wall-clock measurement cannot
 * separate the pacing arithmetic from scheduler lateness, so each scenario drives the frozen class
 * under deterministic virtual timers — the same observation method the frozen unit test
 * `packages/chord/test/output.test.ts:Progress` uses — and records the exact commit schedule.
 */

import { Progress, PROGRESS_BYTES_PER_SECOND } from "./pi.mts";

export interface Scenario {
    name: string;
    source: string;
    expected: unknown;
}

export interface ProgressStep {
    /** Add this many coalescing marks at the current virtual time. */
    mark?: number;
    /** Advance the virtual clock by this many milliseconds, firing due commits. */
    advanceMs?: number;
}

export interface Commit {
    atMs: number;
    bytes: number;
}

/** The real timer functions, kept so the virtual clock can still yield to Node. */
const realSetTimeout = globalThis.setTimeout;
const realClearTimeout = globalThis.clearTimeout;
const realNow = Date.now;

class VirtualClock {
    readonly #timers = new Map<number, { at: number; fire: () => void }>();
    #now = 0;
    #nextId = 1;

    install(): void {
        Date.now = () => this.#now;
        globalThis.setTimeout = ((fire: () => void, ms = 0) => {
            const id = this.#nextId++;
            this.#timers.set(id, { at: this.#now + Math.max(0, Number(ms)), fire });
            return id;
        }) as typeof globalThis.setTimeout;
        globalThis.clearTimeout = ((id: unknown) => {
            this.#timers.delete(Number(id));
        }) as typeof globalThis.clearTimeout;
    }

    restore(): void {
        Date.now = realNow;
        globalThis.setTimeout = realSetTimeout;
        globalThis.clearTimeout = realClearTimeout;
    }

    /** Let every pending promise reaction settle without advancing time. */
    async flush(): Promise<void> {
        await new Promise<void>((resolve) => realSetTimeout(resolve, 0));
    }

    /** Advance to `target`, firing each timer in due order and settling its reactions. */
    async advanceTo(target: number): Promise<void> {
        await this.flush();
        for (;;) {
            let chosen: [number, { at: number; fire: () => void }] | undefined;
            for (const entry of this.#timers) {
                if (entry[1].at > target) continue;
                if (!chosen || entry[1].at < chosen[1].at || (entry[1].at === chosen[1].at && entry[0] < chosen[0]))
                    chosen = entry;
            }
            if (!chosen) break;
            this.#timers.delete(chosen[0]);
            this.#now = chosen[1].at;
            chosen[1].fire();
            await this.flush();
        }
        this.#now = target;
        await this.flush();
    }

    /** The earliest pending timer time, or undefined when the clock is idle. */
    nextDue(): number | undefined {
        let due: number | undefined;
        for (const timer of this.#timers.values()) {
            if (due === undefined || timer.at < due) due = timer.at;
        }
        return due;
    }

    /** The current virtual time. */
    now(): number {
        return this.#now;
    }
}

/** Run `body` with `Date.now`, `setTimeout` and `clearTimeout` replaced by a virtual clock. */
async function withVirtualTimers<T>(body: (clock: VirtualClock) => Promise<T>): Promise<T> {
    const clock = new VirtualClock();
    clock.install();
    try {
        return await body(clock);
    } finally {
        clock.restore();
    }
}

async function driveProgress(
    minIntervalMs: number,
    steps: readonly ProgressStep[],
    writeBytes: readonly number[],
): Promise<Commit[]> {
    return withVirtualTimers(async (clock) => {
        const commits: Commit[] = [];
        let written = 0;
        const progress = new Progress(
            async () => {
                const bytes = writeBytes[written];
                if (bytes === undefined) throw new Error("the schedule wrote more commits than declared");
                commits.push({ atMs: Date.now(), bytes });
                written++;
                return bytes;
            },
            (error: unknown) => {
                throw error;
            },
            minIntervalMs,
        );
        for (const step of steps) {
            if (step.advanceMs !== undefined) {
                await clock.advanceTo(clock.now() + step.advanceMs);
            } else {
                for (let count = 0; count < (step.mark ?? 0); count++) progress.mark();
                await clock.flush();
            }
        }
        // Drain a commit the last mark may still owe.
        for (let guard = 0; guard < 16; guard++) {
            const due = clock.nextDue();
            if (due === undefined) break;
            await clock.advanceTo(due);
        }
        await progress.stop();
        return commits;
    });
}

export async function progressPacesLaterCommitsByWrittenBytes(): Promise<Scenario> {
    const minIntervalMs = 100;
    const steps: ProgressStep[] = [
        { mark: 1 },
        { advanceMs: 0 },
        { mark: 2 },
        { advanceMs: 499 },
        { advanceMs: 1 },
        { mark: 1 },
        { advanceMs: 99 },
        { advanceMs: 1 },
    ];
    const writeBytes = [51_200, 10, 10];
    const commits = await driveProgress(minIntervalMs, steps, writeBytes);
    return {
        name: "progress commits the first change at once then paces later commits by their written bytes",
        source: "packages/chord/src/output.ts:Progress,PROGRESS_BYTES_PER_SECOND; packages/chord/test/output.test.ts:Progress",
        expected: {
            bytesPerSecond: PROGRESS_BYTES_PER_SECOND,
            commits,
        },
    };
}

export async function progressKeepsExactFractionalByteRatePause(): Promise<Scenario> {
    const minIntervalMs = 0;
    const steps: ProgressStep[] = [
        { mark: 1 },
        { advanceMs: 0 },
        { mark: 1 },
        { advanceMs: 10.009765625 },
    ];
    const writeBytes = [1_025, 1];
    const commits = await driveProgress(minIntervalMs, steps, writeBytes);
    return {
        name: "progress keeps the exact fractional byte-rate pause instead of rounding it",
        source: "packages/chord/src/output.ts:Progress,PROGRESS_BYTES_PER_SECOND",
        expected: {
            bytesPerSecond: PROGRESS_BYTES_PER_SECOND,
            commits,
        },
    };
}
