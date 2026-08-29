# A++++ Engineering Plan — Six Pillars

**Progress: 0 / 6 pillars complete — foundational quality work in progress across build hygiene and test discipline**

Goal: take the project from a working but fragile B/B+ state to an A++++ standard where a senior engineer can clone the repo, run one command, get a green build, pass the test suite, and never fight leaked artifacts, undocumented assumptions, or brittle runtime behavior.

This is not a generic cleanup checklist. It is a real execution plan for the current codebase, built around the actual structure and patterns already visible in the project: the Windows overlay bootstrap in [Project/Project.cpp](../Project/Project.cpp), the build configuration in [Project/Project.vcxproj](../Project/Project.vcxproj), the test harness in [Project/Tests/main.cpp](../Project/Tests/main.cpp), and the runtime logging in [Project/Core/SessionLog.cpp](../Project/Core/SessionLog.cpp).

---

## Pillar 1 — Clean, reproducible build

**Why it comes first:** no CI, no reliable test gate, and no safe refactor can exist until the repo stops leaking build output into version control and the build becomes deterministic.

Current issues:
- build artifacts are landing in tracked folders such as Build and Project/Build
- artifact leakage is a recurring regression risk
- warnings and platform drift are not enforced centrally
- outputs are not isolated from source control

Execution steps:
- ☑ Remove tracked artifact trees from the repo and keep them ignored.
- ☑ Move large runtime dumps and generated logs out of git-tracked workspace paths.
- ☑ Consolidate the build flow through a single script with explicit modes: build, run, test, package, full rebuild.
- ☐ Enforce warning-as-error policy for project sources with a central build policy in Directory.Build.props.
- ☐ Keep third-party code on a separate warning policy so vendored libraries do not poison the app build.
- ☐ Move outputs to a single out/ directory and block repo-root artifact writes.

**Done when:** a clean checkout plus one build command yields a green build and a clean git status with no tracked artifacts.

---

## Pillar 2 — Test coverage for the real logic

**Why this is the highest leverage move:** the deterministic code paths are the pure math, parser logic, and decode routines. Those are exactly the functions that benefit most from a machine-enforced test gate.

Current strengths:
- a dedicated test project already exists under [Project/Tests](../Project/Tests)
- doctest is vendored and active
- memory overlays and synthetic memory harnesses are already in place

Execution steps:
- ☑ Add a dedicated test project without DMA or game-runtime dependencies.
- ☑ Build a memory-read seam so the FName decode path can be exercised with a synthetic-memory harness.
- ☑ Cover the most valuable deterministic logic first:
  1. vector and matrix math
  2. bone transforms and aim smoothing math
  3. JSON parsing and fuzz safety
  4. SteamDecrypt and FName decode
- ☐ Add synthetic raycast and world-projection cases for the visibility path.
- ☐ Add coverage measurement and a minimum gate for pure logic.
- ☑ Make the test runner part of the project build flow.

**Done when:** the project test command is required for green builds, and pure logic coverage clears a meaningful threshold.

---

## Pillar 3 — CI as enforcement, not decoration

**Why this matters:** the overlay cannot run in CI, but a Windows build pipeline can still enforce the parts that matter most: compile correctness, test pass, and hygiene.

Execution steps:
- ☐ Add a GitHub Actions Windows pipeline running the release build and the test suite.
- ☐ Fail on any nonzero exit from the test binary.
- ☐ Fail if git status is dirty after build/test.
- ☐ Add a static analysis pass with a staged warning gate.
- ☐ Publish a badge row for build, tests, and hygiene.

**Done when:** every push or pull request has a simple, green compile-and-test signal and no artifact drift.

---

## Pillar 4 — Defensive coding and crash hygiene

**Why:** the project already has a crash handler and a session log, which is a strong base. The missing piece is making the system resilient and diagnosable under real-world failure modes.

Execution steps:
- ☐ Wrap timeBeginPeriod in an RAII guard so timer lifetime is symmetric.
- ☐ Centralize memory reads behind a checked interface that validates bounds and alignment before acting.
- ☐ Add bounded retry/backoff logic for hardware and serial communication paths.
- ☐ Extend crash handling to create dump artifacts and structured logs for failures.
- ☐ Add SDL-enabled enforcement for stricter runtime checks.

**Done when:** fuzzed parser input cannot crash the app, every Begin is paired with a corresponding resource cleanup, and a crash leaves a meaningful dump behind.

---

## Pillar 5 — Architecture clarity

**Why:** the project has grown in layers and passes, and the code increasingly resembles a system assembled over time rather than a fully designed pipeline.

Execution steps:
- ☐ Document a single end-to-end architecture: read parameters → decode state → build world model → render and control.
- ☐ Define a clear entity model and pass ordering for update logic.
- ☐ Split runtime state into explicit data containers instead of relying on a large set of globals.
- ☐ Refactor in small, build-safe stages rather than a risky rewrite.

**Done when:** a new engineer can trace one entity from memory read to render without hunting through unrelated modules.

---

## Pillar 6 — Documentation and process discipline

**Why:** a repo with strong process discipline is easier to trust, easier to maintain, and much less likely to regress silently.

Execution steps:
- ☐ Add a project README describing the purpose, one-command build, and test flow.
- ☐ Add a changelog and conventional commit guidance.
- ☐ Enforce a pre-commit checklist: clean status, test pass, no tracked artifacts after build.
- ☐ Keep plans under docs/ and archive completed efforts so the work history stays readable.

**Done when:** a new developer can clone the repo, read one set of docs, and go from zero to a green build and test pass without tribal knowledge.

---

## Execution order and effort

| # | Pillar | Effort | Unlocks |
| --- | --- | --- | --- |
| 1 | Clean build | 1–2 sessions | everything |
| 2 | Test coverage | 3–5 sessions | CI and refactor safety |
| 3 | CI enforcement | 1–2 sessions | quality gate |
| 4 | Defensive coding | 2–3 sessions | hardening |
| 5 | Architecture clarity | 2–4 sessions | maintainability |
| 6 | Docs and process | 1 session | onboarding |

Total effort: roughly two weeks of focused work.

---

## Definition of A++++

A project reaches A++++ only when all of these are true:

- one-command build works from a clean checkout
- git status stays clean after build and test
- the test suite is required for green status
- pure-logic coverage clears a meaningful threshold
- CI enforces compile, tests, and hygiene
- runtime errors are logged and dump artifacts are created when needed
- docs explain the architecture and the operating model
- repeated builds are deterministic and no artifact leaks into the repo again

This is the standard to aim for. The plan is intentionally staged so the cheap wins come first, then the project becomes enforceable, then maintainable, then demonstrably elite under repeatable engineering practice.

---

## Summary

The project already has the right ingredients for an A++ trajectory: modular C++ structure, a test harness, low-level memory handling, logging, and real engineering constraints. The missing work is not creativity; it is discipline, automation, and a consistent execution sequence.

The next move is simple:
1. fix the build hygiene issue,
2. enforce test pass as a gate,
3. make CI catch regressions,
4. then refactor the architecture without breaking the logic that already works.

That sequence is the difference between a clever experimental project and a professional-grade engineering system.
