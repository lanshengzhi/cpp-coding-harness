# Durable image processing and read outcomes (#1090)

## Scope

`cch_durable` owns image detection, preparation and the read-tool image branch. The durable Owner
does not depend on `cch_coding_agent`, and classifier or image-generation Provider execution stays
excluded. The ticket title's image read/write narrows to the read path: no frozen observation in
this baseline writes image bytes through the durable write tool (upstream `write` takes text
content), and the #1058 ledger carries no image-write row, so the slice delivers no write
capability. Observations were compared with frozen pi v1.3.0 at
`71023aec1546d6d677a65669c5665c0927ce0dd0`; the registered capture lives in
`fixtures/pi-v1.3.0/audit-1090/`.

## Delivered behavior

`src/durable/Images.*` provides the public `ImageProcessor` contract, `ImageLimits`,
`PreparedImage`, MIME detection, header-only dimensions, standard base64 and
`local_image_processor()`. The local processor decodes with vendored stb and libwebp, applies EXIF
orientation for JPEG and WebP, resizes to the limits, encodes PNG or falling-quality JPEG within the
base64 byte budget, and reports the upright source/output dimensions plus any format conversion.

`src/durable/CodingTools.cpp` consumes that contract through `CodingToolOptions` (`images`, `vision`
and `limits`) and returns the image content block before a `<harness>` info diagnostic. An upright
inline image within the limits passes through byte-identically; an oversized or non-inline image is
oriented, resized or converted; a model without vision receives the frozen placeholder note. When no
processor is configured, a BMP, an over-limit image and an unreadable header each produce the frozen
wrapped `<harness>` error with `isError=true`; a configured processor that cannot decode the bytes
produces the frozen prepare error. `vision` is caller-supplied and defaults to false: callers must
set it from the conversation model, so an unwired caller fails closed instead of claiming image
input. `CodingToolOptions` has no classifier or image-generation Provider seam at all; the
capability-absent path is the null processor/vision flag, and the request count only shows that no
extra model turn occurs. The `ImageProcessor` seam is recorded in `docs/agents/architecture.md`.

The capture records the frozen inline pass-through, patterned EXIF-oriented JPEG with four quadrant
samples, WebP orientation, BMP conversion pixels, vision-absent placeholder, processor decode
failure, processor-absent outcomes, and static PNG/JPEG/GIF/WebP sniffing plus header dimensions in
`fixtures/pi-v1.3.0/audit-1090/images.json`.

## Validation

Focused `[issue1090]`: 10 selected, 10 passed, 0 failed, 0 skipped, with 358 assertions. The public
Harness tests read through the registered `read` tool and compare the committed image block,
MIME type, upright dimensions, resized JPEG pixel within tolerance, exact diagnostic text and, for
the lossless BMP conversion, the exact RGBA pixels. An inline GIF is compared as exact base64, and
an inline PNG passes through byte-identically. Capability-absent sections assert the frozen errors,
`isError`, and that the model request count remains two. The processor test compares frozen MIME,
conversion and resized metadata, compares the exact RGBA pixels for the BMP conversion, checks four
quadrant samples of the patterned oriented JPEG within a JPEG tolerance, and compares the inline
base64 payload exactly. Sniffing and header dimensions for PNG/JPEG/GIF/WebP come from the
registered artifact.

Architecture: 43 selected, 43 passed, 0 failed, 0 skipped. `scripts/evidence/audit_evidence.py
verify` reports 40 bundles, 95 artifacts and 533 scenarios at the frozen baseline. The consolidation
check reports 530 scenarios, 0 uncovered and 116 explicit orphans, and its 42-case Python audit
passes. `scripts/format-check.sh` and `git diff --check` are clean.

Full CTest selected 3956: 3955 passed, 0 failed, 1 skipped. The skipped case was the Native TUI
differential capture. A timing-sensitive fullscreen scroll-indicator case failed in two batch runs
and passed five isolated reruns and the final full run.

Review follow-up at `09598c6a4`: a §9.8 range-for fix, an internal `ReadImage` parameter bundle and
the read-path scoping note above. No behavior or test change. Focused `[issue1090]` re-ran 10
selected, 10 passed (pinned JUnit refreshed); the consolidation check and its 42-case boundary
audit pass against the re-pinned record; format check is clean. Full CTest on this machine
selected 3956: 3948 passed, 1 failed, 7 skipped. The failure is `cch_pi_v130_named_evidence`'s
real-capture cases, which need the external frozen pi checkout — its `node_modules` currently lack
`@esbuild/linux-x64`, so tsx cannot start; the same failure reproduces with every local change
stashed, and the six newly skipped pi-differential cases skip for the same environmental reason.

## Capability status

The `image-processor`, `read-image`, `image-capability-absent` and `image-orientation-conversion`
rows in `1056-durable-harness-workflows.json` are `implemented` with `verified` verification and
`implementationTicket: "#1090"`. The whole-row delivery receipt `1090-durable-images` in
`1058-deliveries.json` pins the focused JUnit result and this report; the generated consolidated
ledger resolves all four rows to `implementation_ticket: "#1090"`, `verification: verified` and no
remaining work.

## Remaining boundary

- Encoded bytes come from stb/libwebp rather than Photon. Lossless BMP conversion and inline
  pass-through compare exact pixels or base64; the lossy JPEG path compares four quadrant colour
  samples within tolerance, and the lossy WebP path compares MIME, dimensions and metadata. The
  frozen Photon and local libwebp decoders disagree on the existing WebP stream's pixel values, so
  that stream is not pixel-compared.
- `CodingToolOptions.vision` is supplied by the caller and defaults to false. The runtime does not
  yet infer image-input capability from model metadata or consult a Provider to discover it.
- The pi-neutral decode/encode mechanics duplicate the existing `cch_coding_agent` image input
  implementation because `cch_durable` cannot depend on that Owner. Consolidating them into a shared
  support seam is a separate architecture decision, not part of this slice.
- BMP has no header-only dimension reader in the frozen behavior; without a processor a BMP reports
  that it needs conversion, not that its size is unreadable.
- No image-generation or classifier provider path was added, and the #1044 exclusions remain in
  force.
