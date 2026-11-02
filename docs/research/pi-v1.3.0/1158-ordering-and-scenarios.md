# #1158 — bounded ordering investigation and candidate scenario routing

## Submission completion and commit observers

The original durable selection and a separate rerun failed the `issue1163`
case `generation publishes the attempt before the model stream and usage with
the final answer`, checking `atomic_answer_commits == 1`. Their results and
subsequent five-pass comparisons are in [1158-main-comparison.txt](1158-main-comparison.txt).

A further bounded `--repeat until-fail:500` run stopped at the first failure:

- Branch source `5021cfc` (the test target was built after `1f6a0b7`; later
  branch changes are evidence documentation): one pass followed by a failure
  on repetition 2. [Full CTest output](1158-ordering-branch.txt).
- Independently configured and built exact main
  `b5b88e11ff0d45a806abe3fd828ac256ee0e1af4`: 58 passes followed by a failure
  on repetition 59. [Full CTest output](1158-ordering-main.txt).

Both failures name the same assertion, and the displayed expansion was
`1 == 1` despite the failed check. A separate immediately preceding branch run
of 100 repetitions passed all 100. These are bounded observations, not an
estimate of failure rate. The commands from the #1158 worktree were:

```sh
ctest --preset vcpkg -R '^generation publishes the attempt before the model stream and usage with the final answer$' --repeat until-fail:500 --output-on-failure
ctest --test-dir build/main-baseline-1158/build -R '^generation publishes the attempt before the model stream and usage with the final answer$' --repeat until-fail:500 --output-on-failure
```

The code establishes a window consistent with the symptom:

1. `Session::subscribe_commits` appends listeners to a vector
   (`src/durable/Session.cpp:2419`); commit publication invokes them in vector
   order (`Session.cpp:858`). Harness installs its internal listener during
   construction (`src/durable/Harness.cpp:1700`), before the test subscribes its
   listener (`tests/durable/HarnessWorkflowTest.cpp:3451`).
2. That internal listener calls `submissions->observe` first. For a settled
   submission, `Submission::Registry::observe` invokes a registered waiter's
   completion (`Harness.cpp:132`); the test's `consume` callback signals its
   condition variable (`tests/durable/Outcome.hpp:42`). The waiting test thread
   can therefore resume before the publication loop reaches its later listener,
   which increments `atomic_answer_commits`.
3. Independently, a `wait` started after storage records settlement returns the
   settled value directly (`Harness.cpp:210-215`), without waiting for the later
   observer. The test checks the counter after `wait` at
   `tests/durable/HarnessWorkflowTest.cpp:3490`; `wait` does not establish that
   the later listener has finished.

The same symptom reproduces on exact main, and `Session.cpp` and `Harness.cpp`
are byte-identical to that main revision. #1158 does not directly change this
ordering or the failing test. The ordering window and the differing failed-check
and displayed values are consistent with an observer timing race, but no trace
captured the original failure's thread interleaving; these observations do not
establish its specific cause, measure comparative failure rates, or exclude an
indirect effect of the branch. No production or test code was changed for this
investigation.

## Candidate `audit-1158` scenario routing

The candidate family is unregistered: none of its seven exact scenario names is
present in the current consolidation plan or consolidated ledger. They are not
included in the current coverage denominator, and the current zero-uncovered
result does not cover them. All seven are related to the existing partial,
upstream-captured `cch_durable:node-filesystem` row (source lane `1056`); no new
whole-row verification is justified by this slice.

| Candidate artifact and scenario | Current C++ comparison | Suggested classification on registration |
| --- | --- | --- |
| `listing.json`: directory pages continue where the previous page stopped | `Durable directory pages continue where the previous page stopped` compares page sizes, done and names | Explicit orphan initially; narrow page observation, not the complete filesystem row. |
| `listing.json`: listFiles lists files and directories depth first in name order | Removed tree-listing test; no current `list_files` implementation | Explicit orphan pending tree-listing implementation and comparison. |
| `listing.json`: listFiles stops at its entry and byte bounds and reports truncated | Removed tree-listing test; no current `list_files` implementation | Explicit orphan pending bounds and truncation implementation and comparison. |
| `listing.json`: listFiles applies hidden and excluded names and never lists .git | Removed tree-listing test; no current `list_files` implementation | Explicit orphan pending exclusion implementation and comparison; capture has `gitignore:false`. |
| `positioned-read.json`: positioned reads return up to the requested length and stop early only at end of file | `Durable positioned reads return the frozen bytes and stop only at end of file` compares captured byte results | Explicit orphan initially; narrow byte-range observation, not a complete reader or filesystem row. |
| `positioned-read.json`: a read at or past end of file returns the frozen empty result instead of an error | Same test compares captured empty-file and representable-offset results | Explicit orphan initially; observation does not cover offsets beyond `off_t`. |
| `positioned-read.json`: a positioned read longer than one internal chunk crosses reads unchanged | Same test compares the generated expected range plus captured length/head/tail; it does not compare the captured SHA-256 field | Explicit orphan initially; complete captured-field comparison and opened-reader identity remain follow-ups. |

For a narrow initial registration, add each exact `(family, artifact, scenario)`
as an `orphanScenarios` record with the related row, a reason and disposition;
this keeps the new observations from silently expanding the row's verification
claim. Alternatively, the serial merger may deliberately route reviewed
scenarios through `scenarioLinks` using the source lane and row, after checking
that the linked obligation and its evidence are correctly scoped. Every
registered scenario must be named, artifact-covered by a row, or an explicit
orphan; otherwise consolidation fails. Registry and plan edits change inputs
pinned by existing receipts, so the serial merger must review those deltas,
rebuild the affected comparison targets, rerun their exact selections and
refresh only the corresponding reviewed receipts before consolidation. The
candidate manifest and `baselines.json` were not modified by this analysis.
