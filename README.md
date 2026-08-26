# Algorithm Trainer

Algorithm Trainer is a small full-stack application for solving programming problems. The current
MVP presents an A+B problem in a React/Monaco interface, submits Python solutions to a C++/Drogon
backend, runs them in an NsJail sandbox against hidden cases, and stores submission results in
SQLite.

Supported verdicts are Accepted, Wrong Answer, Runtime Error, and Time Limit Exceeded.

## Requirements

- Linux on `x86_64`
- Nix with flakes enabled
- Kernel support for unprivileged user, mount, PID, IPC, UTS, cgroup, and network namespaces
- seccomp-BPF support

All build and runtime dependencies are pinned by `flake.lock`; no global NsJail, Python, C++, Node.js,
or SQLite installation is required.

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

Build the project first, then start the backend and frontend in separate terminals.

Terminal 1:

```bash
nix develop --command ./build/backend/algorithm-trainer-backend
```

Terminal 2:

```bash
nix develop --command pnpm --dir frontend dev --host 127.0.0.1
```

Open <http://127.0.0.1:5173>. Vite proxies `/api` requests to the backend at
`http://127.0.0.1:8080`.

The backend creates `data/algorithm-trainer.sqlite3` by default. To choose another database file:

```bash
ALGORITHM_TRAINER_DATABASE=/absolute/path/to/database.sqlite3 \
  nix develop --command ./build/backend/algorithm-trainer-backend
```

When the backend is served over HTTPS, set `ALGORITHM_TRAINER_SECURE_COOKIES=1` so authentication
cookies carry the `Secure` attribute. Leave it unset only for plain-HTTP local development.

## API

```text
POST /api/auth/register
POST /api/auth/login
POST /api/auth/logout
GET  /api/auth/me
GET  /api/problems/:slug
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
- `migrations/`: versioned SQLite schema migrations
- `docs/database.md`: submission persistence details
- `docs/sandbox.md`: isolation model, exact limits, host requirements, and known limitations

Python submissions are passed through the `Executor` abstraction to NsJail. There is no unsandboxed
fallback. Each hidden case runs in a fresh temporary workspace with restricted filesystem,
environment, networking, processes, memory, CPU, wall time, and output. See
[`docs/sandbox.md`](docs/sandbox.md) for the complete security model.

## Current scope

The MVP supports one problem, Python submissions, and username/password authentication. Judging is
synchronous. Profiles, password recovery, OAuth, queues, additional languages, and distributed
execution are intentionally outside the current scope.
