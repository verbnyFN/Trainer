# Submission database

The backend stores submissions in SQLite. By default it creates:

```text
data/algorithm-trainer.sqlite3
```

Set `ALGORITHM_TRAINER_DATABASE` before launching the backend to use another file:

```bash
ALGORITHM_TRAINER_DATABASE=/var/lib/algorithm-trainer/database.sqlite3 \
  nix develop --command ./build/backend/algorithm-trainer-backend
```

The parent directory is created automatically. Database initialization and migrations run before the
HTTP listener starts; the backend exits instead of serving requests if initialization fails.

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

Migrations live in `migrations/` and applied versions are recorded in `schema_migrations`.
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
afterward SQLite is the runtime source of truth and existing rows are never overwritten. Disabling a
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
runtime-error submission. It never changes the original row. A partial unique index and an immediate
transaction prevent concurrent requests from creating duplicate retries; the retry and its audit row
commit atomically.

`008-add-admin-operations.sql` adds optimistic revision counters to problems and hidden tests, the
retry link, filter index, and `admin_audit_log`. Admin updates must send the revision they loaded and
receive HTTP 409 after a concurrent edit. Audit details contain identifiers and normalized action
metadata only, never problem-test bodies or submitted source.

SQLite foreign-key enforcement is enabled and connections use a five-second busy timeout. SQL values
are always bound through prepared statements, including source code containing quotes or embedded NUL
bytes.
