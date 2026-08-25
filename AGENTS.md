# Repository Guidelines

## Project Structure & Module Organization

This repository currently defines a lightweight Nix development environment:

- `flake.nix` declares the `x86_64-linux` development shell and its packages.
- `flake.lock` pins `nixpkgs` for reproducible environments; update it intentionally and commit it with related flake changes.
- `.agents/` and `.codex/` contain local automation metadata, not application source.

There is no application or test tree yet. When adding one, keep implementation code under a clear top-level directory such as `src/`, tests under `tests/`, and static resources under `assets/`. Avoid placing generated output in the repository root.

## Build, Test, and Development Commands

- `nix develop` enters the pinned development shell. At present it provides Git.
- `nix flake check` evaluates the flake and runs any checks added to its `checks` output.
- `nix flake show` displays the outputs exposed by the flake.
- `nix flake update` refreshes locked inputs. Review `flake.lock` carefully before committing.

No build or application run command is currently defined. Add new workflows as flake outputs or documented scripts rather than relying on machine-specific setup.

## Coding Style & Naming Conventions

Use two-space indentation in Nix files, keep attribute sets readable, and include trailing semicolons. Prefer descriptive, lowercase attribute names and hyphenated file names where Nix naming permits. Group related packages and outputs together. No formatter or linter is configured; if one is introduced, add it to the development shell and document its command here.

ning a pull request.

## Commit & Pull Request Guidelines

The repository has no commit history from which to infer conventions. Use short, imperative commit subjects, optionally scoped, such as `flake: add formatter` or `tests: cover package output`. Keep each commit focused.
Pull requests should explain the motivation and behavior change, list verification commands, and link relevant issues. Include screenshots only for visible UI changes. Call out changes to pinned dependencies or required local configuration explicitly.
# Project: Algorithm Trainer

## Goal

Build a web application for learning algorithms and data structures.

Users should be able to:

* read programming problems;
* write solutions in an embedded code editor;
* submit solutions;
* execute submitted code in an isolated environment;
* run solutions against hidden test cases;
* receive verdicts such as:

  * Accepted;
  * Wrong Answer;
  * Runtime Error;
  * Time Limit Exceeded;
* later view submission history and learning progress.

The initial goal is a small working MVP, not a complete LeetCode-like platform.

---

## Language preference

C++ is the preferred language for everything except frontend code.

Use C++ whenever it is reasonably practical for:

* backend services;
* REST API;
* business logic;
* database access;
* submission processing;
* judge integration;
* filesystem operations;
* background processing;
* server-side utilities;
* tests;
* command-line tools created specifically for this project.

Prefer modern C++20 or newer when supported by the project toolchain.

Do not introduce another backend programming language such as:

* JavaScript / TypeScript;
* Python;
* Go;
* Rust;
* Java;
* C#;

unless using C++ would cause significant complexity, poor maintainability, major platform problems, or create a security risk.

If a non-C++ technology is clearly more appropriate, explain why before introducing it.

Do not replace a straightforward C++ implementation merely because another ecosystem has a more popular library.

---

## Important exceptions

The C++ preference does not apply when another language or runtime is inherently required.

Examples:

* frontend code may use TypeScript / JavaScript;
* Node.js may be used as frontend build tooling;
* Python must be available when executing Python user submissions;
* other language runtimes may be installed when support for those submission languages is added;
* shell scripts may be used for very small build or development tasks;
* Nix expressions are used for the Nix development environment;
* SQL may be used for database schemas and migrations.

These exceptions must not result in application business logic being moved away from C++ without a strong reason.

---

## Architecture

Prefer a simple architecture.

The project consists primarily of:

1. a browser frontend;
2. a C++ backend;
3. an isolated code execution mechanism.

Avoid microservices unless they solve an actual problem.

Do not create additional backend services without a concrete reason.

The MVP should remain deployable as a small application.

---

## Frontend

Use:

* React;
* TypeScript;
* Vite;
* Monaco Editor.

Frontend responsibilities include:

* rendering problem statements;
* displaying the code editor;
* selecting programming language;
* sending submissions to the backend;
* displaying submission status;
* displaying judge results.

The frontend must never contain hidden tests.

The frontend must never decide whether a submission is Accepted or Wrong Answer.

The frontend must never be trusted with security-sensitive validation.

Keep frontend business logic minimal.

---

## Backend

Use C++20.

Preferred framework:

* Drogon.

The C++ backend is responsible for:

* REST API;
* loading programming problems;
* retrieving hidden test cases;
* receiving submissions;
* validating requests;
* storing submissions;
* communicating with the judge;
* comparing outputs when appropriate;
* calculating submission verdicts;
* returning results to the frontend.

Do not introduce:

* Next.js backend;
* Express;
* NestJS;
* FastAPI;
* Flask;
* Django;
* Spring;
* Go HTTP services;
* Rust backend frameworks;

unless there is a compelling technical reason.

---

## HTTP API

Prefer simple REST endpoints.

Initial API shape:

```text
GET /api/problems/:slug

POST /api/submissions

GET /api/submissions/:id
```

Example submission request:

```json
{
  "problemId": "a-plus-b",
  "language": "python",
  "code": "a, b = map(int, input().split())\nprint(a + b)"
}
```

Keep API contracts explicit.

Validate all input on the server.

Do not expose internal filesystem paths, judge configuration, hidden tests, or sensitive diagnostics through the public API.

---

## Database

For the initial MVP prefer SQLite.

Reasons:

* no separate database server is required;
* simple local development;
* easy integration with C++;
* easy backup and inspection;
* sufficient for the initial application.

Use Drogon's database facilities where practical.

Initial database entities may include:

* problems;
* test_cases;
* submissions.

Hidden test cases must never be sent to the frontend.

Do not introduce PostgreSQL until there is an actual requirement that SQLite cannot reasonably satisfy.

When the application grows, PostgreSQL may replace SQLite without changing the high-level application architecture.

---

## Code execution and judge

Submitted user code is untrusted.

Security is more important than the preference for implementing everything in C++.

Never execute submitted code directly using a normal unrestricted process.

Never do something equivalent to:

```cpp
std::system(("python " + userControlledPath).c_str());
```

without a properly designed isolation layer.

Never execute submitted code with access to:

* the application source code;
* hidden tests outside the specific execution context;
* application secrets;
* the database;
* the host user's home directory;
* arbitrary network access.

Design the application around an abstraction such as:

```cpp
struct JudgeRequest;
struct JudgeResult;

class Judge {
public:
    virtual ~Judge() = default;

    virtual JudgeResult run(
        const JudgeRequest& request
    ) = 0;
};
```

Application code must depend on the Judge abstraction rather than directly depending on a particular sandbox implementation.

---

## Sandbox

On Linux/NixOS, NsJail is a preferred technology to investigate for local isolated execution.

The sandbox should eventually restrict:

* filesystem access;
* network access;
* process count;
* CPU time;
* wall-clock time;
* memory;
* file size;
* available syscalls.

Do not assume that merely starting a child process is sufficient isolation.

For an internet-facing production system, treat sandbox security as a separate security-sensitive subsystem.

Do not weaken isolation merely to simplify implementation.

If a mature non-C++ sandbox solution is safer than a custom C++ implementation, prefer the mature solution.

Do not implement a custom security sandbox from scratch unless explicitly requested.

---

## Initial language support

The MVP supports only Python submissions.

This does not mean backend code should be written in Python.

The backend remains C++.

Python exists only as the runtime used to execute submitted Python programs.

Design the judge interfaces so additional languages can be added later.

Potential future languages:

* C++;
* Python;
* Java;
* JavaScript;
* Rust.

Do not implement them during the initial MVP unless explicitly requested.

---

## Build system

Use CMake as the primary C++ build system.

Prefer Ninja as the build backend when available.

Expected commands should remain approximately:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Keep CMake targets modern and target-oriented.

Prefer:

```cmake
target_link_libraries(...)
target_include_directories(...)
target_compile_features(...)
```

Avoid global compiler flags and global include directories where possible.

---

## Dependency management

Because development happens on NixOS, prefer Nix for system-level development dependencies.

Use `flake.nix` to provide a reproducible development shell.

The development environment should provide tools such as:

* clang;
* clang-tools;
* cmake;
* ninja;
* drogon;
* sqlite;
* pkg-config;
* git;
* nodejs;
* frontend package manager;
* Python runtime for judge testing;
* required sandbox tools.

Avoid installing development dependencies globally outside Nix when practical.

C++ dependencies should preferably come from Nix packages or be integrated cleanly through CMake.

Do not add an additional C++ package manager unless it provides a concrete benefit.

---

## Frontend dependency management

Frontend dependencies are an exception to the C++ preference.

Use a standard Node.js package manager for frontend-only dependencies.

Prefer pnpm if the project already uses it.

Node.js tooling must remain isolated to the frontend.

Do not implement backend application logic in Node.js merely because Node is already required to build the frontend.

---

## Testing

Use Catch2 for C++ unit tests unless there is a strong reason to choose another C++ testing framework.

Tests should cover important backend behavior such as:

* problem lookup;
* request validation;
* output comparison;
* verdict calculation;
* judge result translation;
* submission state transitions.

Prefer deterministic tests.

Do not require external network access for normal unit tests.

Integration tests for the sandbox should be separated from normal unit tests where practical.

---

## Code quality

Prefer:

* RAII;
* value semantics;
* standard library types;
* smart pointers when ownership is dynamic;
* `std::optional`;
* `std::variant`;
* `std::filesystem`;
* `std::chrono`;
* strong types when they improve correctness.

Avoid:

* raw owning pointers;
* manual memory management where unnecessary;
* unnecessary macros;
* global mutable state;
* premature template abstraction;
* excessive inheritance;
* unnecessary metaprogramming.

Use clear and conventional modern C++.

Prefer readability over cleverness.

---

## Formatting and static analysis

Use:

* clang-format;
* clang-tidy.

Keep configuration files in the repository.

After meaningful C++ changes, run formatting, compilation, and relevant tests.

When practical run:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Run clang-tidy when changes affect important backend logic.

---

## Error handling

Do not ignore errors.

Handle:

* invalid HTTP requests;
* database failures;
* compilation failures;
* sandbox failures;
* runtime errors;
* timeouts;
* malformed judge output.

Do not expose sensitive internal error information to the client.

Log enough information on the server to diagnose failures.

---

## Project structure

Prefer a straightforward repository layout.

Example:

```text
algorithm-trainer/
├── AGENTS.md
├── flake.nix
├── CMakeLists.txt
│
├── frontend/
│   ├── package.json
│   ├── vite.config.ts
│   └── src/
│
├── backend/
│   ├── CMakeLists.txt
│   ├── include/
│   ├── src/
│   │   ├── controllers/
│   │   ├── services/
│   │   ├── judge/
│   │   └── database/
│   └── tests/
│
├── migrations/
│
├── sandbox/
│
└── docs/
```

Do not create directories or architectural layers until they are actually useful.

---

## Deployment model

Prefer a simple production deployment.

Frontend should be compiled into static assets.

The C++ backend may serve the compiled frontend files itself.

Conceptually:

```text
Browser
   |
   v
Drogon
   |
   +-- static React application
   |
   +-- /api/*
   |
   +-- SQLite
   |
   +-- isolated judge
```

Avoid requiring a Node.js server in production.

The frontend build tool may use Node.js during the build process.

---

## MVP

Keep the first milestone intentionally small.

Implement one problem:

```text
A + B
```

The user should be able to:

1. open the problem;
2. see the description;
3. write Python in Monaco Editor;
4. press Submit;
5. send the code to the C++ backend;
6. execute it against hidden tests through the judge;
7. receive a verdict.

Initial verdicts:

```text
Accepted
Wrong Answer
Runtime Error
Time Limit Exceeded
```

Do not add authentication, profiles, ratings, contests, achievements, multiple languages, Redis, queues, microservices, or distributed workers before this vertical slice works.

---

## Development philosophy

Build vertically and incrementally.

Prefer:

```text
one complete small feature
```

over:

```text
many partially implemented architectural components
```

Avoid speculative abstractions.

Do not build infrastructure merely because a larger platform might eventually need it.

Implement what the current milestone requires.

---

## Codex workflow

Before making a significant change:

1. read this `AGENTS.md`;
2. inspect the relevant existing files;
3. understand the current architecture;
4. provide a short implementation plan;
5. identify which files are likely to change.

Then implement the requested feature.

After implementation:

1. format changed code;
2. build the affected targets;
3. run relevant tests;
4. inspect failures;
5. fix failures caused by the change;
6. summarize what changed.

Do not silently make large architectural changes.

Do not modify unrelated files.

Prefer small, reviewable diffs.

---

## Learning-oriented behavior

This project is also intended to help the developer learn how to work with Codex and modern C++.

When introducing a non-trivial architectural decision, briefly explain:

* what problem it solves;
* why this approach was chosen;
* reasonable alternatives;
* important tradeoffs.

When adding an unfamiliar C++ technique, prefer readable code and briefly explain the technique.

Do not intentionally over-engineer code merely to demonstrate advanced C++.

---

## Dependency policy

Before adding a new dependency:

1. determine whether the C++ standard library already solves the problem;
2. determine whether Drogon already provides the required functionality;
3. determine whether an existing project dependency already solves it;
4. only then consider adding another dependency.

For backend functionality, prefer a mature C++ dependency over an equivalent dependency requiring another application runtime.

Exceptions are allowed when the non-C++ solution provides a substantial security, reliability, portability, or maintainability advantage.

---

## Security rules

Treat all submitted code and submission input as hostile.

Never:

* trust client-side validation;
* expose hidden test cases;
* interpolate untrusted input into shell commands;
* give submitted code unrestricted filesystem access;
* give submitted code unrestricted network access;
* run submitted code with application privileges;
* expose secrets through environment variables;
* allow submitted code to access the application database.

Security-related shortcuts must be explicitly identified as development-only and must not silently become production behavior.
