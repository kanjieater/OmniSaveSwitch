# AI Agent Instructions
## 0. Output Style
* Caveman mode ALWAYS ACTIVE at full level. Rules in `.claude/skills/caveman/SKILL.md`.
* No greetings, no "I'll help you", no trailing summaries, no "In summary", no "Here's what I did".
* Bullets + code > prose. Answer = action. Fragment sentences OK.
* Exception: security warnings / irreversible ops → full prose, then resume caveman.

## 1. Documentation Protocol (The "Read First" Rule)
* **SHADOW CONTEXT:** All documentation has been compressed for your context window and stored in `./context/`. You MUST treat `./context/` as the absolute source of truth for all specifications, roadmaps, and architecture.
* Before writing code, locate the relevant compressed file in `./context/` and read it.
* You are operating in a documentation-heavy repository. Do not guess project architecture, conventions, or goals.
* Before writing any code or answering architectural questions, you MUST search and read the relevant files in `context/` (NOT any human-only directories).
* Treat `context/` as the absolute source of truth. If your pre-training conflicts with the local documentation, the local documentation wins.
* If a user asks you to implement a feature, first identify if there is a roadmap, cycle, or spec document. Read it to ensure you align with the overarching design constraints.

## 2. Tool Usage & Context Gathering
* Use `grep`, `find`, and `rg` (ripgrep) extensively to locate relevant concepts within the documentation folder before modifying system behavior.
* Do not read massive files blindly. Use `grep -n` to find relevant lines, or `cat` combined with standard bash tools to extract what you need.
* If you are unsure where a concept is documented, run a wide search across all `.md` files first.

## 3. Documentation Maintenance
* When you modify code that changes system behavior, state machines, or architecture, you MUST cross-reference the documentation folder to see if any context file is now outdated.
* If a context file is outdated due to your changes, explicitly ask the user if you should update the documentation to match the new code.

## 4. Execution Rules
* Fail fast. If you cannot find the required context in the documentation, tell the user what you searched for and ask for clarification.
* Do not hallucinate dependencies or libraries. Check the project's dependency files (e.g., `Makefile`) and the setup guides.
* Keep responses concise. Show your reasoning, execute the tools, and provide direct answers.

## 5. Code Organization & Modularity
* **Strict Size Limits:** Keep files under 300 lines. If a file grows beyond this, halt and extract logic into smaller, single-responsibility components or utility modules.
* **Componentization:** Strictly separate I/O operations (network, file system) from pure business logic (hashing, HEAD selection, clock arbitration).
* **No Monoliths:** Do not blindly append new features to the end of `main.cpp`. Create new files and import/include them.

## 6. Anti-Laziness & Completion Rules
* **No Placeholders:** Never write `// TODO` or `/* implement later */`. If you modify a function or create a file, write the complete, working implementation.
* **No Elision:** Do not use `...` to skip code when outputting text. Output exactly what needs to be changed or rely entirely on file editing tools.

## 7. Error Handling & Memory (Project Specific)
* **Fail Loudly:** Do not silently catch exceptions or swallow errors. If an operation fails, log the exact failure reason and either propagate the error or halt safely.
* **Sysmodule Constraints:** In C++, absolutely no dynamic allocation (`malloc`/`new`) during active gameplay loops. Rely strictly on the pre-allocated inner heap.

## 8. Verification & Tooling
* **Always Compile:** If you change C++ code, you MUST run `./build.sh` (or `make`) and ensure it compiles without warnings before concluding the task.
* **Mandatory Formatting:** Before finalizing a task, run `clang-format` on any C++ file you modified.

## 8a. C++ Testing Stack
* **Test Runner & BDD:** Catch2 (v3) — use `SCENARIO`, `GIVEN`, `WHEN`, `THEN` macros.
* **Property-Based Testing:** RapidCheck (`rc::check`) — use for state machine transitions, data parsing, and any logic that must hold over arbitrary inputs.
* **Mocking:** Trompeloeil — fake libnx/hardware interfaces during host-compiled tests.
* **Coverage:** `gcov` → `gcovr` (XML) → `diff-cover`.

## 9. Security & Boundary Enforcement
* **Zero-Trust Inputs:** Treat all network payloads, manifests, and incoming variables as hostile. Validate data types, lengths, and formats before processing.
* **Path Traversal Protection:** Never blindly construct file paths using raw variables. Explicitly sanitize all incoming `title_id` and `device_id` strings to ensure they only contain alphanumeric characters and cannot escape `sdmc:/omnisave/` using `../`.
* **Memory Safety (C++):** Never use unsafe string functions (e.g., `strcpy`, `sprintf`). You MUST use `snprintf`, `strncpy`, and explicit buffer length checks for all array operations.
* **Credential Hygiene:** Never hardcode passwords, API keys, or FTP credentials in source code. Always read them from environment variables and never output them to the console or log files.

## 10. System Invariants (CI-Enforced)
1. **Hardware Abstraction Rule:** No business logic file may directly include `<switch.h>`. All Switch-specific OS calls (file system, `pmdmnt`, network) MUST be abstracted behind interfaces (e.g., `IFileSystem`, `IProcessMonitor`).
2. **Host-Target Compilation:** Production sysmodule compiles for AArch64 (libnx). The test suite compiles for x86_64 (Linux/Docker) and links against Trompeloeil mock implementations of hardware interfaces.
3. **Strict Sibling Pairing:** Every core logic file must have a corresponding test file mirroring its path (e.g., `source/core/reconcile.cpp` → `tests/core/test_reconcile.cpp`). Enforced by `scripts/enforce_sibling_tests.sh`.
4. **Sysmodule coverage gate:** `docker compose run --rm test-sysmodule` is the single source of truth. No production change is valid unless it passes with `diff-cover` at 100% on the diff.
5. **CI is the gate:** Never add git hooks (pre-commit, pre-push, commit-msg) that block or delay `git commit` or `git push`.
6. Tests should be considered early, but do not have to be written until the end.

### C++ Task Execution Workflow
When writing or modifying sysmodule logic:
1. Define the interface contract if the code interacts with the OS.
2. Write the Catch2/RapidCheck test file against the interface.
3. Write the production C++ code to pass the test.
4. Run `docker compose run --rm test-sysmodule` to verify coverage before declaring done.

## 11. Test Tiers

| Tier | Command | Time | When to use |
|---|---|---|---|
| **C++ gate** | `docker compose run --rm test-sysmodule` | ~2 min | Any sysmodule or overlay change; required before merge |

**Rules for Claude:**
* After editing `sysmodule/source/**` or `overlay/source/**`: `docker compose run --rm test-sysmodule` must pass before declaring done.