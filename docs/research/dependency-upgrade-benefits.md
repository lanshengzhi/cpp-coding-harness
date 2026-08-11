# Dependency snapshot upgrade benefits: vcpkg builtin-baseline `f3e1065` → `2f1d605`

**Audit date:** 11 August 2026 (all primary sources below were retrieved and checked on this date; the newest dated primary source consulted is the `microsoft/vcpkg` HEAD commit `2f1d605400c8727cc00c15797aba796c88ccd523`, authored 2026-08-11T12:16:07Z — the GitHub API reports it as vcpkg HEAD at audit time).

**Scope.** Selected plan: bump `builtin-baseline` in `vcpkg.json` from `f3e10653cc27d62a37a3763cd84b38bca07c6075` to `2f1d605400c8727cc00c15797aba796c88ccd523`. Out of scope (evaluated separately, NOT in the selected plan): upstream-only OpenSSL 4.0.1 and CLI11 2.7.2, which official vcpkg does not package.

**Method.** Every version claim below was verified against the official vcpkg `versions/baseline.json` and port metadata at both commits; every upstream claim is cited to official release notes, changelogs, migration guides, or security advisories. Repository usage was mapped by reading the actual seams (`vcpkg.json`, `CMakeLists.txt`, `src/util/Json.{hpp,cpp}`, `src/util/JsonGlaze.hpp`, `src/ai/glaze/AiJson.hpp`, `src/harness/session/EntrySerializer.cpp`, `src/ai/providers/BoostBeastStreamTransport.cpp`, `src/ai/providers/SseParser.cpp`, `src/cli/CliParse.cpp`). No pre-existing `docs/research/` convention template exists in this repo (probed `docs/README.md`, `docs/research/README.md`, `docs/agents/research.md`); this brief follows the repo's documentation conventions (headings, links, clear English, no build required per AGENTS.md).

---

## 1. TL;DR and recommendation

**Adopt the selected baseline bump.** It moves exactly two direct dependencies — Glaze 7.7.1 → 8.0.0 and OpenSSL 3.6.2 → 3.6.3 — and nothing else changes among the repo's nine direct dependencies (verified against official vcpkg baseline data at both commits). Both moves are net wins for this harness:

- **Glaze 8.0.0** fixes real robustness/security gaps in the JSON reader the harness uses for typed DTO and session parsing (stack-overflow DoS via deep recursion, silent integer wraparound, incomplete UTF-8 validation, truncated-input misreporting). Most of its breaking changes target formats and APIs this repo does not use (BEVE, YAML, JSONB, REPE/JSON-RPC registry, streaming/lazy). Expected migration cost: one clean rebuild plus the existing test suite; a handful of behavior changes (stricter rejects) need fixture verification.
- **OpenSSL 3.6.3** is a security patch release fixing 17 CVE-assigned issues plus the handshake-buffer hardening issue known as "HollowByte" (18 security issues total); a subset sits in the TLS-client/certificate-parsing path the harness exercises on each new HTTPS/WSS connection. As a same-branch patch, it is not expected to require source migration, but it still requires a rebuild and test run.

**Do not chase OpenSSL 4.0.1 or CLI11 2.7.2 now.** Both are upstream-only today (not in official vcpkg as of HEAD `2f1d605`), and neither offers this harness a benefit that justifies leaving the audited official snapshot: OpenSSL 4.0's new features (ECH, post-quantum groups, new KDFs) are unused by the harness's asio-based TLS client, and a major-version move carries recompile/removed-API risk with vcpkg/Boost compatibility unverified; CLI11 2.7.x is mostly a bug-fix/audit release whose affected paths (help formatting, conversion edge cases, module/precompiled builds) barely touch this repo's startup-only CLI use. Revisit both when official vcpkg ports land.

**One forward-looking flag:** OpenSSL 3.6 is a non-LTS branch supported only until 2026-11-01 [1]; the LTS line is 3.5 (until 2030-04-08) [2]. vcpkg currently ships 3.6.3 [3]; expect vcpkg to move the `openssl` port again soon (likely to 3.5 LTS or 4.x) and plan the next bump when that lands, rather than pinning 4.0.1 by hand.

---

## 2. What the selected snapshot actually changes (verified against official vcpkg data)

`versions/baseline.json` at `f3e1065` [4] vs `2f1d605` [5], for every direct dependency in this repo's `vcpkg.json` [6]:

| Port | At `f3e1065` (current) | At `2f1d605` (selected) | Change |
| --- | --- | --- | --- |
| `glaze` | 7.7.1 | 8.0.0 | **minor→major upstream jump** |
| `openssl` | 3.6.2 | 3.6.3 | **security patch** |
| `cli11` | 2.6.2#0 | 2.6.2#0 | unchanged |
| `md4c` | 0.5.3#0 | 0.5.3#0 | unchanged |
| `utf8proc` | 2.11.3#0 | 2.11.3#0 | unchanged |
| `libwebp` | 1.6.0#2 | 1.6.0#2 | unchanged |
| `boost-asio` | 1.91.0#0 | 1.91.0#0 | unchanged |
| `boost-beast` | 1.91.0#0 | 1.91.0#0 | unchanged |
| `boost-process` | 1.91.0#0 | 1.91.0#0 | unchanged |
| `asio` (standalone) | 1.32.0#0 | 1.32.0#0 | **becomes a transitive dep of glaze** (see below) |

Transitive change to note: the vcpkg `glaze` port gained `"asio"` in its `dependencies` at 8.0.0 — `ports/glaze/vcpkg.json` at `f3e1065` lists only host tooling (`vcpkg-cmake`, `vcpkg-cmake-config`) [7], while at `2f1d605` it lists `asio` first [8]. Glaze's own 7.8.0 release notes explain why: "Centralize Asio backend selection into a `glaze::asio` target, and export it to `find_package` consumers so the standalone/Boost Asio macro pair is configured consistently" [9]. Impact on this repo: the standalone `asio` vcpkg port (header-only, 1.32.0) is now installed alongside `boost-asio`; the harness never uses Glaze's networking layer, so this is install/build-surface growth only — no harness code compiles against standalone Asio unless `glaze/ext/asio.hpp` (or a `glaze::asio`-consuming header) is included, which the repo's seams (`JsonGlaze.hpp`, `AiJson.hpp`, `EntrySerializer.cpp`) do not do. Whether vcpkg's header-only `glaze` install itself pulls asio headers into compile units is an unmeasured unknown (see §10).

The resolved host-tool set also changes: `vcpkg-cmake-config` moves from `2024-05-23` to `2026-07-21`. Together with standalone `asio`, the installed package count in the validated `dev-fast` tree grows from 73 to 74. This is dependency-install/build tooling drift, not a runtime library API change.

The selected commit itself is an unrelated routine vcpkg port update (`[opengl-registry] Update to 2026-08-03 (#53344)`, 2026-08-11) [10]; the baseline diff includes thousands of unrelated ports, but only the declared and resolved changes above matter for this repo.

---

## 3. How this repo uses the affected dependencies (the seams)

- **Glaze — typed JSON DTO parsing (private serialization layer).** Public contracts are Glaze-free; Glaze is confined to `src/`:
  - `src/util/JsonGlaze.hpp` — generic `glz::read_json<T>` / `glz::write_json(const T&)` wrappers plus `glz::generic` ⇄ `util::JsonValue` conversions; `JsonValue` I/O itself is a hand-rolled parser in `src/util/Json.cpp` ("implemented without Glaze") [11][12].
  - `src/ai/glaze/AiJson.hpp` — typed provider/session DTOs (`MessageDto`, `ContentDto`, `UsageDto`, `ContextDto`, `glz::generic` for `details`/tool `arguments`) and `read_message_json`/`write_message_json`/`read_context_json`/`write_context_json` [13].
  - `src/harness/session/EntrySerializer.cpp` — pi v3 session JSONL DTOs; field order in DTOs is deliberately the wire order because "Glaze writes members in declaration order, which is what makes the golden byte-identical" [14] (golden tests: `SessionRoundTripGoldenTest`, `SessionSuiteGoldenTest`, `GlazeRoundTripTest`).
  - Provider adapters (`src/ai/api/*Adapter.cpp`) build request bodies via `util::write_json` and parse SSE event payloads; the SSE framing itself is a hand-rolled line parser (`src/ai/providers/SseParser.cpp`, no Glaze streaming) [15].
- **OpenSSL — HTTPS/WSS transport (Boost.Beast/Asio).** `src/ai/providers/BoostBeastStreamTransport.cpp` (HTTPS, `boost::asio::ssl`, `ssl::host_name_verification`) [16], `BoostBeastWebSocketTransport.cpp` (WSS), and `src/ai/auth/*` (OAuth callback server / PKCE HTTP client) link `OpenSSL::SSL`/`OpenSSL::Crypto` [17]. The harness is a TLS *client* only; it does not exercise S/MIME, CMS, QUIC, OCSP stapling, PKCS#12, or server-side TLS.
- **CLI11 — CLI parsing.** `src/cli/CliParse.cpp` builds the full pi-aligned flag surface with `CLI/CLI.hpp` and links `CLI11::CLI11`; notably, `normalize_parse_error()` string-matches CLI11 error names and messages (`"ExcludesError"`, `"The following argument was not expected: "`), a seam sensitive to CLI11 error-wording changes [18][19].

---

## 4. Glaze 7.7.1 → 8.0.0 (selected plan)

Primary source: official v8.0.0 release notes, published 2026-08-03 [20] (the jump also spans 7.8.0–7.9.1, released June–July 2026 [21][22]).

### 4.1 Benefits relevant to this repo (typed JSON parsing)

1. **DoS hardening: reader recursion is now bounded at 256 levels.** Previously "the BEVE, CBOR, MessagePack and JSON readers enforced no depth limit, so a small hostile buffer overflowed the stack and crashed the process" — a JSON recursive struct at ~100k levels (≈900 KB) SIGSEGV'd; hostile input could hang the reader for 40 s (339 bytes → 40 s before; constant ~8 ms after) [20]. This harness parses network-derived JSON (provider SSE payloads) and local session files; a malicious/broken provider response that previously could crash the process now yields a clean `exceeded_max_recursive_depth` error through the existing `glz::error_ctx` plumbing in `JsonGlaze.hpp` [12]. This is the single most valuable fix in the jump for this repo.
2. **Out-of-range integers are rejected instead of silently wrapped.** Measured against a `__int128` reference over 103,135 inputs: accepted-with-a-wrong-value went from 17,714 to 0, wrongly-rejected from 150 to 0 (e.g. `[300]` into `uint8_t` previously read as 44) [20]. The repo's DTOs are full-width `std::int64_t`/`double` fields (usage, timestamps, token counts [13][14]), so the practical effect is: values that previously wrapped now fail the parse loudly instead of corrupting session/usage data.
3. **Complete UTF-8 validation on read (RFC 8259 §8.1).** v7 accepted invalid UTF-8 in strings (e.g. overlong encodings); v8 rejects them, including in fields the caller does not model [20]. Providers' text content and session transcripts now fail cleanly instead of persisting invalid bytes. Measured cost is small: "about 5–6% end to end" on a string-heavy, 22% non-ASCII document, "within noise" on pure-ASCII; the `validate_utf8 = false` opt-out compiles the check away entirely if ever needed [20].
4. **Truncated input reports `unexpected_end` instead of the documented-non-error `end_reached` escaping the read; empty input reports `no_read_input`.** These are correctness fixes at the error-reporting boundary the repo already converts into `ErrorCode::JsonParse` diagnostics [12].
5. **Assorted JSON-reader and writer correctness fixes** accumulated between 7.7.1 and 8.0.0, including "Reject truncated JSON arrays in null-terminated reads" in v7.9.0 [22] and "Prevent string truncation at embedded nulls across all formats" in v8.0.0 [20]. The earlier compile-time include split that made `jmespath`, `msgpack`, and `recorder` opt-in shipped in v7.7.0, so the current v7.7.1 dependency already has that benefit and it is not an upgrade gain here.

### 4.2 Irrelevant features (do not mis-weight these)

BEVE v2 wire-format changes, YAML/TOML/JSONB/CSV/MsgPack/CBOR changes, the REPE/JSON-RPC registry (`read_params` return change, registry buffer bounding), NDJSON/streaming refill points, `lazy_json`/`lazy_beve` option-slicing fix, `glz::http_headers`, SIMD-backend reporting, chrono serialization generalization — none of these paths are compiled or exercised by this repo (JSON-only, buffered `read_json`/`write_json`, no registry, no lazy APIs, no BEVE). The repo's `std::variant` DTOs (`MessageContentDto`, `NullableString`) declare no `glz::meta` tag, so the "variant tagging decided per variant" change leaves their wire form unchanged (untagged → "the alternative's own value") [20][13].

### 4.3 Breaking changes and migration cost for this repo

Most listed breaks do not apply (BEVE data at rest, REPE handlers, streaming views, forward-declared `lazy_document`, registry options) [20]. The ones that can bite, in decreasing likelihood:

- **Stricter parse rejection (behavior change, no compiler diagnostic):** documents with invalid UTF-8, out-of-range integers, or nesting > 256 levels that v7 accepted (or corrupted) now error. Any provider payload or legacy session file containing such bytes will surface as a `JsonParse` failure instead of being stored/wrapped. The repo's transcript path should produce valid UTF-8, but resumed *legacy* session files (older harness or pi-written) are the realistic exposure. Fix, if ever needed: `struct unchecked_opts : glz::opts { bool validate_utf8 = false; }` — would require threading custom opts through the `JsonGlaze.hpp` wrappers, a small, contained change [20].
- **`end_reached` semantics:** v7 callers could observe the documented-non-error `end_reached`; v8 reports `unexpected_end`. The repo treats any failed `glz::read_json<T>` as an error and formats `error_ctx`, so no repo call site inspects `end_reached` [12]; the hand-rolled `SseParser` and `util::Json` parser are unaffected [15][11].
- **`std::monostate` in an internally-tagged variant** writes `{"tag":"ID"}` instead of `null` — only under internal tagging; this repo's variants are untagged, so no wire change [20].
- **Compile surface:** v8's stricter `static_assert`s and the 7.8.x include-surface split mean a clean rebuild may surface new compile errors if any DTO shape was relying on now-deprecated spellings; "several of these breaks announce themselves as `static_assert`s naming the offending type, so a clean rebuild will find most of what applies" [20].

**Expected migration steps:** update `builtin-baseline` in `vcpkg.json` [6]; `cmake --preset vcpkg` (or bootstrap) to rebuild the manifest; run the full suite via `scripts/run-tests.sh` — especially `[ai]` (`GlazeRoundTripTest`, adapter tests, `SseParserTest`), `[harness][session]` (golden JSONL byte-identity), and `[cli]` shards. Golden-session tests are the early-warning system for any wire-format drift.

---

## 5. OpenSSL 3.6.2 → 3.6.3 (selected plan)

Primary sources: 3.6 series release notes [24], the 3.6.3 release tag [25], and the official vulnerabilities list [26].

3.6.3 (2026-06-09) is a **security patch release** ("the most severe CVE fixed in this release is High"), fixing 17 CVE-assigned issues plus HollowByte (18 security issues total): CVE-2026-45447 (heap use-after-free in `PKCS7_verify()`), CVE-2026-34182 (CMS `AuthEnvelopedData` forged messages), CVE-2026-34183 (QUIC `PATH_CHALLENGE` unbounded memory growth), CVE-2026-35188 (double-free on OCSP-stapled response), CVE-2026-42764 (QUIC server NULL deref), CVE-2026-45445 (AES-OCB IV ignored on `EVP_Cipher()`), CVE-2026-7383 (heap overflow in ASN.1 multibyte string conversion), CVE-2026-9076 (OOB read in CMS password-based decryption), CVE-2026-34180 (heap over-read in ASN.1 content parsing), CVE-2026-34181 (PKCS#12 PBMAC1 short HMAC keys), CVE-2026-42765 (NULL deref in cert verification with OCSP checking), CVE-2026-42766/42767 (NULL derefs in CMS/CRMF decryption), CVE-2026-42768 (multi-`RecipientInfo` Bleichenbacher oracle in `CMS_decrypt`/`PKCS7_decrypt`), CVE-2026-42769 (CMP `rootCaKeyUpdate` trust-anchor substitution), CVE-2026-42770 (FFC-DH peer validation), CVE-2026-45446 (AES-GCM-SIV/SIV empty-message tag processing), plus "excessive allocation of the handshake message buffer (aka HollowByte)" [24][25].

**Relevance mapping to this repo's usage (TLS client):**

| CVE / fix | Path | Relevant to harness? |
| --- | --- | --- |
| HollowByte handshake-buffer allocation | libssl handshake processing | **Yes — applies when this client receives peer handshake messages**; incremental allocation reduces memory retained when a peer advertises a large message and stalls |
| CVE-2026-7383, CVE-2026-34180 (ASN.1) | certificate/ASN.1 parsing | **Yes — in-path for TLS clients** (server certificates parsed on every connection) |
| CVE-2026-42765 (OCSP checking) | X509 verification with OCSP | Partially — only if OCSP checking is configured; harness uses default verification [16] |
| CVE-2026-45447, 34182, 9076, 34181, 42766–42768 (PKCS7/CMS/PKCS#12) | S/MIME, CMS, PKCS#12 | Not exercised (no cert/signature-format handling in harness) |
| CVE-2026-34183, 42764 (QUIC) | QUIC stack | Not exercised (no QUIC) |
| CVE-2026-45445, 45446, 42770 (OCB/SIV/FFC-DH) | cipher/KEM internals | Not exercised (TLS 1.3 AEAD paths unaffected) |
| CVE-2026-42769 (CMP) | CMP client | Not exercised |

So the upgrade is "patch a library whose *peripheral* formats we do not use, but whose *core* TLS/cert-parsing path we use on each new provider connection, and harden the handshake path". As a same-branch patch release, **no harness source migration is expected**; the practical cost is dependency rebuild plus transport/auth test validation. The current pin (3.6.2, released 2026-04-07 [24]) is already one patch behind and predates the June 2026 advisory batch.

**Support-window context (why this is not a one-time question):** 3.6 is a non-LTS release supported until 2026-11-01; the LTS line is 3.5 (supported until 2030-04-08) [1][2]. Adopting 3.6.3 is correct *now* (vcpkg's official default), but the follow-on decision — vcpkg's next OpenSSL default, likely 3.5 LTS or 4.x — should be tracked rather than hand-pinned (see §6/§10).

---

## 6. NOT IN THE SELECTED PLAN: OpenSSL 4.0.1 (upstream-only)

**Status:** OpenSSL 4.0.0 released 2026-04-14; 4.0.1 (security patch, 2026-06-09) [27][28]. **Official vcpkg does not package 4.x:** the `openssl` baseline at `2f1d605` is 3.6.3 [5], and there is no 4.x entry in the official port data. Adopting it would require a vcpkg `overrides`/overlay port outside the audited official snapshot.

**Upstream benefits (per official release notes [27][29] and migration guide [30]):** ECH (RFC 9849), RFC 8998 `sm2sig_sm3`, hybrid SM2-ML-KEM group, SNMP KDF, SRTP KDF, LMS signature verification, `-expected-rpks` pinning in s_client, and a large cleanup of "potentially significant or incompatible changes". **None of these benefit this harness:** it is an asio/Beast TLS client that needs TLS 1.2/1.3 + cert verification; ECH, PQ groups, KDFs, and LMS are not on the harness's wire path, and server certificate pinning (`-expected-rpks`) is a CLI tool feature, not a library API the harness uses.

**Migration costs (per official migration guide [30]):** a major release — "any application that currently uses an older version of OpenSSL will at the very least need to be recompiled"; previously-deprecated functions were *removed* (e.g. the fixed `TLSv1_2_method()` family); `ASN1_STRING` is now opaque; a large set of `X509` accessors were constified (compiler warnings at minimum); `OPENSSL_cleanup()` is no longer atexit-armed; custom `EVP_MD` implementations are gone. The harness itself calls no deprecated low-level APIs directly (asio/Beast drive the TLS seam [16][17]), so the realistic exposure is **Boost 1.91 asio/Beast source compatibility with OpenSSL 4.x headers — unverified** (Boost 1.91 predates 4.0; a build-and-test against 4.0 is required before any judgment, and vcpkg does not offer it).

**Support window:** 4.0 is explicitly *not* LTS, supported until 2027-05-14 [1][31]; the roadmap confirms a 4.2 LTS for April 2027 [32]. Net: 4.0.1's remaining support outlives 3.6's by ~6 months, but the *stable* 4.x target is the 4.2 LTS — chasing 4.0.1 now means a second major migration within a year, outside vcpkg, for zero harness-relevant features.

**Verdict: reject for now.** Revisit when (a) vcpkg packages 4.x, and (b) the 4.2 LTS (April 2027) or vcpkg's chosen 3.5-LTS path is the available default. Interim: the selected 3.6.3 covers all current advisories.

---

## 7. NOT IN THE SELECTED PLAN: CLI11 2.7.2 (upstream-only)

**Status:** CLI11 2.7.0 (2026-07-30, "Audit and documentation"), 2.7.1 (2026-07-31, LTO fix), 2.7.2 (2026-08-02, "Faster compiles") [33][34]. **Official vcpkg packages 2.6.2#0** — both the baseline and official `cli11` port at `2f1d605` are 2.6.2 [5][35]. Adopting 2.7.2 requires an overlay/override outside the official snapshot.

**Upstream benefits (per CHANGELOG [33]):** 2.7.0 is a systematic audit release — bug fixes for negative→unsigned conversion, whitespace-only float input, odd-length pair containers, empty strings into `std::optional`, `argc == 0` crash, out-of-bounds reads in the non-codecvt narrow/widen paths under C++26, `ignore_case`+`ignore_underscore` together, help-formatting fixes, stable help ordering (sorted `excludes`/`needs`), and "performance improvements that remove unnecessary copies in hot paths". 2.7.1/2.7.2 fix the precompiled/module builds and cut compile time for module consumers (removed `<iostream>`, `<codecvt>`, `<iomanip>`, `<fstream>`, `<locale>` from declaration headers; `App::exit()` became three overloads).

**Relevance to this repo:** the harness parses argv once at startup [18]; the audit fixes are edge-case conversions the pi-aligned surface is unlikely to hit, the help-formatting fixes *could* change `--help` output consumed by CLI golden tests (`CliParseTest`, `CliSmokeTest` [19]), and the module/precompiled work is irrelevant to vcpkg's header-only `CLI11::CLI11` usage. One real migration-sensitive seam: `normalize_parse_error()` string-matches CLI11's error name (`"ExcludesError"`) and message text (`"The following argument was not expected: "`) [18]; 2.7.0 explicitly fixed error-message bugs ("`ExtrasError` reporting the wrong error name", argument-order fixes in earlier releases) [33], so wording changes are plausible and must be re-verified in `CliParseTest` after any upgrade.

**Verdict: defer.** Low benefit (startup-only parser on an already-fixed surface), low-but-real migration cost (error-string/golden re-verification), and — decisively — not in official vcpkg as of the selected baseline. Revisit when vcpkg moves the `cli11` port past 2.6.2.

---

## 8. Benefit / risk table

| Item | Benefits | Risks / costs | Verdict |
| --- | --- | --- | --- |
| **Baseline `f3e1065` → `2f1d605`** (selected) | Current official audited snapshot; exactly two direct deps move; one new transitive (standalone `asio` 1.32.0, header-only, unused by harness code) [4][5][7][8] | New vcpkg install set; rebuild; unrelated baseline churn invisible to this manifest | **Adopt** |
| **Glaze 7.7.1 → 8.0.0** (selected) | Stack-overflow DoS fix (recursion bound 256) [20]; integer-range correctness (17,714 wrong accepts → 0) [20]; full UTF-8 validation [20]; truncation/empty-input error reporting [20]; embedded-NUL write correctness [20]; ~5–6% read cost on non-ASCII-heavy docs with free opt-out [20] | Breaking: stricter rejects (UTF-8/range/depth) can fail parses v7 accepted; `end_reached` → `unexpected_end`; untagged variants unchanged but tagged-variant/monostate wire changes exist for other users; rebuild may hit new `static_assert`s [20] | **Adopt** — relevant fixes, irrelevant breakage (BEVE/YAML/registry/lazy untouched by this repo) |
| **OpenSSL 3.6.2 → 3.6.3** (selected) | 17 CVE-assigned fixes + HollowByte hardening (18 security issues total); subset in TLS-client/cert path exercised on new connections [24][25][26] | Rebuild and transport/auth validation; branch EOL 2026-11-01 means a follow-on decision soon [1] | **Adopt** |
| **OpenSSL 4.0.1** (NOT in plan) | ECH, PQ groups, KDFs, LMS — none used by this harness [27][29] | Major-release recompile; removed APIs; asio/Beast 1.91 compatibility unverified; not in vcpkg; support only to 2027-05-14; 4.2 LTS (Apr 2027) is the real 4.x target [30][31][32][5] | **Defer** — wait for vcpkg + 4.2 LTS or 3.5 LTS default |
| **CLI11 2.7.2** (NOT in plan) | Audit bug fixes (conversion edge cases, help formatting), perf, module compile-time work [33] | Error-name/message wording changes could break `normalize_parse_error` string matching [18]; `--help` golden drift; not in vcpkg (2.6.2 latest) [5][35] | **Defer** — wait for vcpkg port update |

---

## 9. Implementation and validation record

1. `vcpkg.json` now fixes `builtin-baseline` at `2f1d605400c8727cc00c15797aba796c88ccd523`; the configured `dev-fast` manifest confirms Glaze 8.0.0, OpenSSL 3.6.3, standalone Asio 1.32.0, and `vcpkg-cmake-config` 2026-07-21.
2. Focused Glaze tests cover rejection of invalid UTF-8 and out-of-range message integers plus acceptance at the documented 256-level nesting limit and rejection at 257 levels. Existing provider, session-golden, and architecture tests exercise the unchanged serialization and dependency boundaries.
3. Validation completed on Linux with GCC 16.1.1: focused `[glaze]` tests passed, the 52 architecture tests passed, and `ctest --preset dev-fast` passed all eight shards. No live provider or real-network test was run, consistent with the repository validation policy.

---

## 10. Gaps and unknowns

- **Malformed legacy persisted inputs:** the suite verifies valid session goldens plus focused invalid UTF-8/range/depth rejection, but it does not inventory real user session files created by older versions.
- **Compile-time cost of Glaze 8 + the new transitive `asio` port** in this repo's TUs: unmeasured; the standalone Asio dependency adds install time even though the harness does not use Glaze networking.
- **Boost 1.91 asio/Beast + OpenSSL 4.x source compatibility**: unverified (only relevant if/when vcpkg packages 4.x).
- **vcpkg's next `openssl` default** after the 3.6 branch EOLs (2026-11-01): the decision that will make OpenSSL a recurring baseline item; 3.5 LTS (to 2030) vs 4.x LTS (4.2, Apr 2027) are the two plausible targets [1][2][32].
- **CLI11 error-message wording** in any future 2.7.x adoption: the repo's `normalize_parse_error` string-matching seam [18] needs a targeted `CliParseTest` pass.

---

## 11. Sources

**Kept (primary, official):**

- [1] OpenSSL Library — Release Strategy (support windows; 3.6 EOL 2026-11-01; 3.5 LTS to 2030-04-08; 4.0 to 2027-05-14) — https://openssl-library.org/policies/releasestrat/
- [2] OpenSSL Library — Roadmap (4.2 LTS April 2027) — https://openssl-library.org/roadmap/
- [3] OpenSSL Library — Downloads (3.6.3 / 4.0.1 / 3.5.7 EOL table) — https://www.openssl-library.org/source/
- [4] vcpkg `versions/baseline.json` at `f3e10653…` (glaze 7.7.1, openssl 3.6.2, cli11 2.6.2, …) — https://raw.githubusercontent.com/microsoft/vcpkg/f3e10653cc27d62a37a3763cd84b38bca07c6075/versions/baseline.json
- [5] vcpkg `versions/baseline.json` at `2f1d6054…` (glaze 8.0.0, openssl 3.6.3, cli11 2.6.2, …) — https://raw.githubusercontent.com/microsoft/vcpkg/2f1d605400c8727cc00c15797aba796c88ccd523/versions/baseline.json
- [6] Repo `vcpkg.json` (builtin-baseline `f3e10653…`, direct deps) — repo-internal (`/home/lansy/Work/github/coding-agent/cpp-coding-harness/vcpkg.json`)
- [7] vcpkg `ports/glaze/vcpkg.json` at `f3e10653…` (no asio dep) — https://raw.githubusercontent.com/microsoft/vcpkg/f3e10653cc27d62a37a3763cd84b38bca07c6075/ports/glaze/vcpkg.json
- [8] vcpkg `ports/glaze/vcpkg.json` at `2f1d6054…` (glaze 8.0.0, depends on `asio`) — https://raw.githubusercontent.com/microsoft/vcpkg/2f1d605400c8727cc00c15797aba796c88ccd523/ports/glaze/vcpkg.json
- [9] Glaze v7.8.0 release notes (glaze::asio target centralization) — https://github.com/stephenberry/glaze/releases/tag/v7.8.0
- [10] vcpkg commit `2f1d6054` metadata (2026-08-11, opengl-registry) — https://api.github.com/repos/microsoft/vcpkg/commits/2f1d605400c8727cc00c15797aba796c88ccd523
- [11] Repo `src/util/Json.hpp` / `src/util/Json.cpp` (JsonValue I/O without Glaze) — repo-internal
- [12] Repo `src/util/JsonGlaze.hpp` (glz::read_json/write_json wrappers, glaze_error) — repo-internal
- [13] Repo `src/ai/glaze/AiJson.hpp` (MessageDto/ContentDto/ContextDto) — repo-internal
- [14] Repo `src/harness/session/EntrySerializer.cpp` (session JSONL DTOs, member-order golden identity) — repo-internal
- [15] Repo `src/ai/providers/SseParser.cpp` (hand-rolled SSE framing) — repo-internal
- [16] Repo `src/ai/providers/BoostBeastStreamTransport.cpp` (Boost.Beast/asio/OpenSSL TLS client) — repo-internal
- [17] Repo `CMakeLists.txt` (OpenSSL::SSL/Crypto, CLI11::CLI11, package wiring) — repo-internal
- [18] Repo `src/cli/CliParse.cpp` (CLI11 surface, normalize_parse_error string matching) — repo-internal
- [19] Repo `tests/` + `CMakeLists.txt` (test shards incl. `CliParseTest`, `GlazeRoundTripTest`, session goldens) — repo-internal
- [20] Glaze v8.0.0 release notes (2026-08-03; breaking changes, DoS/UTF-8/range/truncation fixes, migration list) — https://github.com/stephenberry/glaze/releases/tag/v8.0.0
- [21] Glaze releases page (7.8.x–7.9.1 window, include-surface note) — https://github.com/stephenberry/glaze/releases
- [22] Glaze v7.9.0 release notes (truncated-JSON-array fix etc.) — https://github.com/stephenberry/glaze/releases/tag/v7.9.0
- [24] OpenSSL 3.6 Series Release Notes (3.6.2→3.6.3 full CVE list, HollowByte) — https://www.openssl.org/news/openssl-3.6-notes.html
- [25] OpenSSL 3.6.3 release tag (security patch release, High) — https://github.com/openssl/openssl/releases/tag/openssl-3.6.3
- [26] OpenSSL Library — Vulnerabilities 3.6 (affected ranges incl. 3.6.0 < 3.6.3) — https://openssl-library.org/news/vulnerabilities-3.6/
- [27] OpenSSL 4.0.0 release tag (feature release; incompatible changes; support to 2027-05-14) — https://github.com/openssl/openssl/releases/tag/openssl-4.0.0
- [28] OpenSSL 4.0.1 release tag (2026-06-09 security patch) — https://github.com/openssl/openssl/releases/tag/openssl-4.0.1
- [29] OpenSSL 4.0 Final Release announcement (ECH, SNMP/SRTP KDF, sm2sig_sm3, RFC 8998, hybrid SM2-ML-KEM; not LTS) — https://openssl-library.org/post/2026-04-14-openssl-40-final-release/
- [30] OpenSSL Guide: Migrating from older versions (4.0: recompile, removed APIs, ASN1_STRING opaque, X509 constification) — https://docs.openssl.org/4.0/man7/ossl-guide-migration/
- [31] OpenSSL 4.0 Final Release — OpenSSL Corporation (supported until 14 May 2027) — https://openssl-corporation.org/post/2026-04-14-openssl-40-final-release/
- [32] OpenSSL Library — Roadmap (4.2 LTS April 2027; 5.0 October 2027) — https://openssl-library.org/roadmap/
- [33] CLI11 CHANGELOG (2.7.0 audit, 2.7.1 LTO, 2.7.2 faster compiles) — https://github.com/CLIUtils/CLI11/blob/main/CHANGELOG.md
- [34] CLI11 releases (v2.7.0 2026-07-30, v2.7.1 2026-07-31, v2.7.2 2026-08-02) — https://github.com/CLIUtils/CLI11/releases
- [35] Official vcpkg `ports/cli11/vcpkg.json` at `2f1d6054…` (version 2.6.2) — https://raw.githubusercontent.com/microsoft/vcpkg/2f1d605400c8727cc00c15797aba796c88ccd523/ports/cli11/vcpkg.json

**Dropped (not used as evidence):** nixpkgs glaze 7.9.1→8.0.0 PR and Arch Linux package page (secondary packaging mirrors, release-date cross-checks only); 9to5Linux and LWN OpenSSL 4.0 coverage (secondary commentary; official notes/announcements cited instead); newreleases.io and versionlog/endoflife.date pages (derived data; official release-strategy page cited instead); inline0/glaze changelog (different project, false hit).
