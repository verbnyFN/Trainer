# Algorithm Trainer

Algorithm Trainer is a full-stack web application for learning algorithms and data structures. It
provides a searchable problem catalog, a React/Monaco solving workspace, authenticated submission
history and progress, and an administrator interface for managing problems, hidden tests, and
submissions. The C++20/Drogon backend stores application data in PostgreSQL and judges Python and
C++20 solutions against hidden cases in a hardened NsJail execution boundary.

Supported verdicts are Accepted, Wrong Answer, Runtime Error, and Time Limit Exceeded.

## Requirements

- Linux on `x86_64`
- Nix with flakes enabled
- Kernel support for unprivileged user, mount, PID, IPC, UTS, cgroup, and network namespaces
- seccomp-BPF support
- systemd with delegation of cgroup v2 `cpu`, `memory`, and `pids` controllers

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

`ctest` runs the C++ unit tests, frontend Vitest suite, and hardened sandbox integration suite. The
NsJail tests run in a transient delegated systemd user service. The host must permit unprivileged
namespaces and user services; a restrictive container or hardening policy causes the suite to fail
rather than silently reducing isolation.

The PostgreSQL and live HTTP suites are registered with CTest but require explicit connection
settings so normal development runs never modify an existing database or application. Run them only
against disposable instances. Without these settings, CTest reports the affected cases as skipped:

```bash
ALGORITHM_TRAINER_TEST_DATABASE_URL='postgresql:///algorithm_trainer_test' \
  ctest --test-dir build --output-on-failure -L postgresql

ALGORITHM_TRAINER_TEST_BASE_URL='http://127.0.0.1:8080' \
  ctest --test-dir build --output-on-failure -L http
```

The HTTP suite registers a temporary user and submits a real Python solution, so its target database
and judge must be disposable and fully operational.

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

## Run with Docker

On a native Linux host, the secure hybrid container launcher builds and starts the application with:

```bash
./scripts/container-app start
```

Use `status`, `logs`, `restart`, or `stop` in place of `start` for the corresponding lifecycle
operation. PostgreSQL and the static frontend run in containers; the backend and NsJail judge remain
in a delegated host systemd scope so Docker does not need dangerous privileges. No Docker socket is
mounted, and unsupported hosts fail closed. See
[`docs/container-deployment.md`](docs/container-deployment.md) for requirements, security rationale,
backup, and production guidance.

Secure judging requires native Linux. On macOS or Windows, run this deployment inside a dedicated
Linux machine or VM; Docker Desktop alone does not provide the required audited namespace and cgroup
boundary.

## Features

- Problem catalog with tag filtering and dedicated problem-solving pages
- Monaco editor with Python and C++20 starter code
- Asynchronous judging with Accepted, Wrong Answer, Runtime Error, and Time Limit Exceeded verdicts
- Registration, login, secure session cookies, profile activity, streaks, and submission history
- Administrator management of problems, starter code, hidden test cases, and failed submissions
- PostgreSQL persistence with migrations, backup, legacy SQLite transfer, and verification tooling

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
GET  /api/admin/problems
POST /api/admin/problems
GET  /api/admin/problems/:slug
PUT  /api/admin/problems/:slug
DELETE /api/admin/problems/:slug
POST /api/admin/problems/:slug/tests
PUT  /api/admin/problems/:slug/tests/:id
DELETE /api/admin/problems/:slug/tests/:id
GET  /api/admin/submissions
GET  /api/admin/submissions/:id
POST /api/admin/submissions/:id/retry
```

Example submission:

```bash
curl --request POST http://127.0.0.1:8080/api/submissions \
  --header 'Content-Type: application/json' \
  --data '{"problemId":"a-plus-b","language":"python","code":"a, b = map(int, input().split())\nprint(a + b)"}'
```

Submission and profile routes require authentication. Administrative routes additionally require
the administrator role. Hidden test cases are available only through authorized administrator
routes; public problem and submission responses never expose them. Sandbox internals and host paths
are never returned through the API.

## Architecture

- `frontend/`: React, TypeScript, Vite, and Monaco Editor
- `backend/`: C++20 Drogon API, authentication, PostgreSQL repositories, judge queue, isolated worker,
  executor abstraction, and Catch2 tests
- `deployment/algorithm-trainer.service`: production-oriented systemd unit with cgroup delegation
- `compose.yaml`: unprivileged frontend and persistent PostgreSQL container services
- `migrations/postgresql/`: production PostgreSQL schema migrations
- `migrations/001-*.sql` through `008-*.sql`: legacy SQLite migrations retained for transfer tests
- `scripts/`: local lifecycle, database transfer, sandbox-test, and HTTP-test helpers
- `docs/database.md`: PostgreSQL setup, persistence, migration, backup, and rollback details
- `docs/sandbox.md`: isolation model, exact limits, host requirements, and remaining security boundary

Python and C++20 submissions pass through the `Executor` abstraction to a separate, environment-free
judge-worker process over bounded IPC and then to NsJail. The backend does not execute submissions in
its own process. C++ source is compiled inside a restricted sandbox before the resulting binary is
executed; there is no in-process or unsandboxed fallback. Runtime-specific default-deny seccomp
allowlists are used for Python, C++ compilation, and compiled programs. Each hidden case runs in a
fresh cgroup v2 and temporary workspace with restricted filesystem, environment, networking,
processes, aggregate memory and swap, CPU, wall time, file size, and output. See
[`docs/sandbox.md`](docs/sandbox.md) for the complete security model.

## Current scope

The application is a single-host MVP with a small problem catalog, username/password authentication,
and Python and C++20 submissions. A persistent in-process scheduling queue dispatches work to
separately hardened judge-worker processes; it is not a distributed job system. Password recovery,
email verification, OAuth, additional submission languages, multi-host execution, and collaborative
or social features are intentionally outside the current scope.

For internet-facing deployment, run the backend as a dedicated unprivileged account with the
provided systemd hardening and cgroup delegation, terminate TLS in front of it, protect PostgreSQL
independently, and review the complete threat model in [`docs/sandbox.md`](docs/sandbox.md). The
sandbox is a security-sensitive subsystem and depends on the host kernel enforcing namespaces,
seccomp, and cgroup v2 correctly.
