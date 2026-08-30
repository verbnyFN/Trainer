# PostgreSQL database

PostgreSQL is the production source of truth for users, sessions, problems, hidden tests,
submissions, and administrative audit records. The backend requires an explicit libpq connection
string and fails before opening its HTTP listener if it cannot connect or migrate:

```bash
export ALGORITHM_TRAINER_DATABASE_URL='postgresql://algorithm_trainer:password@127.0.0.1/algorithm_trainer'
export ALGORITHM_TRAINER_ADMIN_USERNAME='admin'
export ALGORITHM_TRAINER_ADMIN_PASSWORD='replace-with-a-long-random-password'
nix develop --command ./build/backend/algorithm-trainer-backend
```

Use a dedicated database role with access only to the application database. Require TLS and managed
secret injection in production; do not put credentials in the repository, process arguments, or
logs. The configured administrator account is created or promoted and its password is synchronized
at startup, allowing a migrated development account to be rotated immediately.

## Local development

After building, one command initializes PostgreSQL, safely transfers the existing SQLite file, and
starts PostgreSQL, the backend, and the frontend:

```bash
nix develop --command ./scripts/local-app start
```

Stop every component with `nix develop --command ./scripts/local-app stop`. The controller also
supports `status` and `restart`, stores validated process IDs under ignored `data/run`, and writes
backend and frontend output to `data/backend.log` and `data/frontend.log`. A failed partial startup
is rolled back automatically.

The cluster lives under ignored `data/postgresql`, listens only on an ignored private Unix socket,
and uses local socket authentication. Generated administrator credentials are stored in
`data/local-postgresql.env` with mode 600. Lifecycle commands are:

```bash
nix develop --command ./scripts/local-postgresql status
nix develop --command ./scripts/local-postgresql start
nix develop --command ./scripts/local-postgresql stop
```

## Initial PostgreSQL setup

One local-development example is:

```bash
createdb algorithm_trainer
psql --set ON_ERROR_STOP=1 --dbname algorithm_trainer \
  --file migrations/postgresql/001-initial-schema.sql
```

The backend also applies this idempotent migration at startup. Applied versions are recorded in
`schema_migrations`. Future schema changes must use new numbered files and must not edit a migration
that has been deployed.

## Safe transfer from SQLite

Stop every backend process first so the SQLite source cannot change during transfer. Keep the SQLite
file as a rollback backup, create a fresh empty PostgreSQL database, and apply the PostgreSQL schema:

```bash
cp --preserve=timestamps data/algorithm-trainer.sqlite3 \
  data/algorithm-trainer.sqlite3.pre-postgresql
createdb algorithm_trainer
psql --set ON_ERROR_STOP=1 --dbname algorithm_trainer \
  --file migrations/postgresql/001-initial-schema.sql
./build/backend/algorithm-trainer-migrate-sqlite \
  data/algorithm-trainer.sqlite3 \
  'postgresql://algorithm_trainer:password@127.0.0.1/algorithm_trainer'
```

The C++ transfer tool opens SQLite read-only, refuses any non-empty PostgreSQL target, transfers all
tables in one PostgreSQL transaction, preserves identifiers and relationships, repairs identity
sequences, and rolls back the entire import after any error. It never deletes or edits the SQLite
source. Sessions are transferred as hashes, so users do not have to log in again.

Before directing production traffic to PostgreSQL, compare table counts on both databases, check
foreign-key consistency, log in, open a public and administrative problem, submit a known solution,
and verify its stored history. Retain the stopped SQLite file until the PostgreSQL backup and restore
procedure has also been tested.

Rollback means stopping the new backend and deploying the previous SQLite-capable application with
the preserved SQLite file. Writes made after switching to PostgreSQL are not automatically copied
back; therefore schedule a short maintenance window for the cutover and do not allow both versions
to accept traffic simultaneously.

## PostgreSQL integration tests

Tests require a disposable database whose name ends in `_test`; the suffix guard prevents accidental
truncation of a normal database:

```bash
createdb algorithm_trainer_test
ALGORITHM_TRAINER_TEST_DATABASE_URL='postgresql:///algorithm_trainer_test' \
  ./build/backend/algorithm-trainer-postgresql-tests
```

## Submission lifecycle

Valid requests are persisted before judging and follow these asynchronous transitions:

```text
queued → running → completed
                 ↘ failed
```

Workers atomically claim the oldest queued row, which prevents concurrent workers from processing the
same submission. Interrupted running rows are returned to the queue when the worker pool starts, so
work survives backend restarts. `completed` means judging produced a solution verdict. `failed` means
the judge or worker infrastructure failed, not that the submitted solution was wrong. Database
constraints and guarded SQL updates prevent completed or failed submissions from being transitioned
again.

Queue admission defaults to 100 active submissions globally and five active submissions per user;
anonymous requests share a single user bucket. At most one submission from the same user runs at a
time, so another user's queued work can use the remaining workers. Capacity and per-user rejections
are returned as HTTP 429 responses and are not inserted into the database.

Authenticated submissions store the owning user id. History queries require both that id and the
problem id, so records belonging to another account or problem are excluded by the database query.
Anonymous submissions remain supported but do not appear in user history.

Hidden cases, test inputs, expected outputs, NsJail configuration, stderr, and internal diagnostics are
never returned by public APIs or written to submission results and logs. Hidden test inputs and
expected outputs are stored separately in `problem_tests` and are loaded only through the judge's
internal repository method. The database contains the submitted source code because submission
retrieval and history need to reproduce what the user sent. Runtime failures store only a normalized
error category; raw stderr and tracebacks are discarded after judging.

## Migrations

Legacy SQLite migrations remain in `migrations/` solely for the old repository regression tests and
the transfer path. PostgreSQL migrations live in `migrations/postgresql/`, and applied versions are
recorded in `schema_migrations`.
`001-create-submissions.sql` creates the initial submissions table, `002-create-auth.sql` adds users
and sessions, and `003-link-submissions-to-users.sql` adds submission ownership and the history
index. `004-add-submission-error-type.sql` adds normalized runtime-error categories. Migrations are
applied in version order inside transactions. `005-add-submission-queue.sql` introduces the persistent
queued and running states and migrates legacy pending rows into the queue. Existing migrations should
never be edited after deployment; add a new numbered migration instead.

`006-add-admin-users.sql` adds the authorization flag used by protected admin functionality. The
backend bootstraps the initial `admin` account through the normal libsodium password-hashing path;
the bootstrap password is never stored as plaintext.

`007-create-problems.sql` adds stable text-keyed problems and their ordered hidden tests. Problem
metadata includes the title, slug/id, statement formats, difficulty, tags, supported languages,
examples, and enabled state. The built-in catalog is used only to seed missing rows on first startup;
afterward PostgreSQL is the runtime source of truth and existing rows are never overwritten. Disabling a
problem removes it from public lookup and new submissions while retaining its stable id, tests,
submission relationships, progress, and historical verdicts. Editing tests affects only judging that
occurs after the edit; completed submission verdicts are stored independently and are not recalculated.

## Admin API

All `/api/admin/*` routes require a valid session whose user has `is_admin = 1`. Missing sessions
receive HTTP 401 and authenticated non-admin users receive HTTP 403.

Problem administration uses `GET` and `POST /api/admin/problems` and `GET`, `PUT`, and `DELETE`
on `/api/admin/problems/:id`. A problem write accepts `id` (on create), `title`, `description`,
`inputFormat`, `outputFormat`, `difficulty`, `tags`, `languages`, `examples`, and `enabled`. The id is
stable and comes from the URL during updates.

Hidden tests are created with `POST /api/admin/problems/:id/tests` and changed or removed with `PUT`
or `DELETE /api/admin/problems/:id/tests/:testId`. Test writes accept `input`, `expectedOutput`, a
non-negative integer `position`, and `enabled`. Only the protected admin problem detail response
includes these fields; public problem, submission, history, and profile responses use separate
serializers that cannot include them.

Administrators can inspect stored submission records with `GET /api/admin/submissions` and `GET` on
`/api/admin/submissions/:id`. These endpoints report the verdict already stored on the submission and
never rerun it, so later problem or hidden-test changes cannot modify history.

The submission list accepts `status`, `errorType`, `language`, `problemId`, `userId`, `from`, and `to`
query parameters and returns at most 500 newest matching rows. Diagnostics contain only normalized
categories and generic descriptions. Submitted source is available to administrators, but raw stderr,
sandbox command lines, host paths, environment variables, hidden tests, and executor internals are
never persisted in or returned by these records.

`POST /api/admin/submissions/:id/retry` creates one new queued submission linked to a failed or
runtime-error submission. It never changes the original row. A partial unique index and one atomic
PostgreSQL statement prevent concurrent requests from creating duplicate retries; the retry and its
audit row commit together.

`008-add-admin-operations.sql` adds optimistic revision counters to problems and hidden tests, the
retry link, filter index, and `admin_audit_log`. Admin updates must send the revision they loaded and
receive HTTP 409 after a concurrent edit. Audit details contain identifiers and normalized action
metadata only, never problem-test bodies or submitted source.

PostgreSQL enforces foreign keys and check constraints. Queue claims use `FOR UPDATE SKIP LOCKED`, so
multiple backend workers or instances cannot claim the same row. SQL values are always passed through
libpq parameters rather than interpolated into commands, including hostile-looking source code.
