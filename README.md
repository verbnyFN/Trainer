# Algorithm Trainer

Algorithm Trainer is a small full-stack application for solving programming problems. The current
MVP presents an A+B problem in a React/Monaco interface, submits Python solutions to a C++/Drogon
backend, runs them in an NsJail sandbox against hidden cases, and stores application data in
PostgreSQL.

Supported verdicts are Accepted, Wrong Answer, Runtime Error, and Time Limit Exceeded.

## Requirements

- Linux on `x86_64`
- Nix with flakes enabled
- Kernel support for unprivileged user, mount, PID, IPC, UTS, cgroup, and network namespaces
- seccomp-BPF support

All build and runtime dependencies are pinned by `flake.lock`; no global NsJail, Python, C++, Node.js,
or PostgreSQL installation is required outside the development shell.

## Build and test

Run all commands from the repository root:

```bash
nix develop
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
pnpm --dir frontend build
```

The NsJail integration tests require the host to permit unprivileged namespaces. A host hardening
policy or restricted container may prevent those tests from running.

## Run locally

Build the project, then start the database, backend, and frontend together with one command:

```bash
nix develop --command ./scripts/local-app start
```

Open <http://127.0.0.1:5173>. Stop the frontend, backend, and database completely with:

```bash
nix develop --command ./scripts/local-app stop
```

`scripts/local-app status` reports all three components and `scripts/local-app restart` restarts the
complete application. Vite proxies `/api` requests to the backend at `http://127.0.0.1:8080`.

The local launcher initializes a private PostgreSQL cluster under ignored `data/`, starts it on a
Unix socket without TCP access, backs up and transfers the legacy SQLite database once, and generates
local administrator credentials in `data/local-postgresql.env` with mode 600. See
[`docs/database.md`](docs/database.md) for production
PostgreSQL setup, backup, transfer, verification, and rollback instructions.

When the backend is served over HTTPS, set `ALGORITHM_TRAINER_SECURE_COOKIES=1` so authentication
cookies carry the `Secure` attribute. Leave it unset only for plain-HTTP local development.

## API

```text
POST /api/auth/register
POST /api/auth/login
POST /api/auth/logout
GET  /api/auth/me
GET  /api/profile
GET  /api/problems
GET  /api/problems/:slug
GET  /api/problems/:slug/submissions
POST /api/submissions
GET  /api/submissions/:id
```

Example submission:

```bash
curl --request POST http://127.0.0.1:8080/api/submissions \
  --header 'Content-Type: application/json' \
  --data '{"problemId":"a-plus-b","language":"python","code":"a, b = map(int, input().split())\nprint(a + b)"}'
```

Hidden test cases and sandbox diagnostics are never returned through the API.

## Architecture

- `frontend/`: React, TypeScript, Vite, and Monaco Editor
- `backend/`: C++20 Drogon API, judge logic, executor abstraction, and Catch2 tests
- `migrations/postgresql/`: production PostgreSQL schema migrations
- `migrations/001-*.sql` through `008-*.sql`: legacy SQLite migrations retained for transfer tests
- `docs/database.md`: submission persistence details
- `docs/sandbox.md`: isolation model, exact limits, host requirements, and known limitations

Python and C++20 submissions are passed through the `Executor` abstraction to NsJail. C++ source is
compiled inside a restricted sandbox before the resulting binary is executed; there is no
unsandboxed fallback. Each hidden case runs in a fresh temporary workspace with restricted
filesystem, environment, networking, processes, memory, CPU, wall time, and output. See
[`docs/sandbox.md`](docs/sandbox.md) for the complete security model.

## Current scope

The MVP supports one problem, Python and C++20 submissions, username/password authentication, and
per-problem submission history for authenticated users. Judging is synchronous. Password recovery,
OAuth, queues, additional languages, and distributed execution are intentionally outside the current
scope.
