# Remote retirement review: historical callback race, 2026-11-09

This is historical red evidence from an earlier, mixed isolated worktree candidate; its exact source tree was not recorded. It is not a result against this slice or against base commit `bae3a2b58345287d560048b4d938e2affe6a8a79`, and no reproducible command for that old tree is claimed.

A latch parked an executing stop callback while another thread retired its participant. The observer became ready before the callback was released. Ordinary and address/undefined-sanitized processes failed the assertion (Catch2 exit 42), each under an external 10-second timeout. The test translation unit was instrumented for the sanitizer run, but the linked Catch2 archives' instrumentation was not verified. This is a red reproduction, not a successful sanitizer validation.

Counting only `request_stop()` is insufficient: registering a callback on an already-stopped token can invoke it synchronously. The candidate in this slice counts registration and callback destruction tails, but does not establish a production Remote drain. See [the slice record](1232-retirement-review-fix-2026-11-09.md) for current scope, reproducible commands and remaining boundaries.
