# Agent instructions

Rules for every AI agent and contributor working in this repository. Read before making changes.

## Single executable rule

- This project ships exactly **one** executable: `Build/ArcRaiders.exe`.
- **Never add a new executable.** No new `.exe`, test `.exe`, helper `.exe`, console tool `.exe`, or launcher.
- Do not add application targets (`.vcxproj`) or build steps that emit an `.exe`.
- Do not create side executables for tools, tests, diagnostics, or one-off scripts. Put that code inside the main application, gated behind a config/debug flag, and run it from `ArcRaiders.exe`.
- Anything that would normally be "a small program" must be a function/pass in the existing main target, not a second binary.

## Build

- Build from source: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`
- Output must land at `Build/ArcRaiders.exe` and stay the only executable.
- If the build reports `LNK1104: cannot open file 'Build\ArcRaiders.exe'`, an old instance is still running — close it first.

## No unsolicited kill / build / run

- **Never kill, build, or run anything unless the user explicitly asks.**
- Do not run `scripts/build.ps1` with any flag (`-Test`, `-Run`, `-RunOnly`, `-FullRebuild`, `-Package`), and do not invoke `MSBuild`, `taskkill`, or a test/bench binary on your own initiative.
- `scripts/build.ps1` **force-kills any running `ArcRaiders.exe` before building.** Running it unasked terminates the user's live session. This has already happened — do not repeat it.
- Do not build or run the test executable (`Project/Project.Tests.vcxproj`, `Build/Tests/`) unless the user asks for tests by name. The single-executable rule above still applies.
- A review request ("look over", "review", "check", "what do you think") is **read-only**: report findings, change nothing, build nothing, run nothing.
- If a check seems necessary to verify a change, say what you want to run and wait for the go-ahead.

## After edits: close, rebuild, run

- After finishing any code edit, the user's default expectation is: **close the running overlay, rebuild, and run** to verify the change.
- The user has explicitly requested this workflow; it overrides the "wait for a go-ahead" rule for post-edit verification.
- Before closing or rebuilding, check whether another AI agent is currently building or editing the project (e.g. a running `scripts/build.ps1`, `MSBuild`, or `cl`/`link` process, or recent edits from another session). If one is, let it finish; it will rebuild the project. Do not close the app or start a build in that case.
- If nothing else is building or coding the project, close the running `ArcRaiders.exe` overlay, rebuild, and rerun it.
- While rebuilding, watch for the `LNK1104` lock error (an old `ArcRaiders.exe` still running) and the `Output is locked: Build\ArcRaiders.exe` refusal — both mean the overlay must be closed first.
- This does not apply to review-only requests ("look over", "review", "check"): those stay read-only.

## Notes

- `Build/` is the runnable output (shipped exe, DLLs, `Data/`). Source lives in `Project/`.
- Keep generated corpora and reports out of the tree (see `.gitignore`).
