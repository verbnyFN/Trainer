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

Valid requests follow one synchronous state transition:

```text
pending → completed
        ↘ failed
```

`completed` means judging produced a solution verdict. `failed` means the judge infrastructure failed,
not that the submitted solution was wrong. Database constraints and guarded SQL updates prevent
completed or failed submissions from being transitioned again.

Authenticated submissions store the owning user id. History queries require both that id and the
problem id, so records belonging to another account or problem are excluded by the database query.
Anonymous submissions remain supported but do not appear in user history.

Hidden cases, test inputs, expected outputs, NsJail configuration, stderr, and internal diagnostics are
not stored. The database contains the submitted source code because submission retrieval and history
need to reproduce what the user sent.

## Migrations

Migrations live in `migrations/` and applied versions are recorded in `schema_migrations`.
`001-create-submissions.sql` creates the initial submissions table, `002-create-auth.sql` adds users
and sessions, and `003-link-submissions-to-users.sql` adds submission ownership and the history
index. Migrations are applied in version order inside transactions and should never be edited after
deployment; add a new numbered migration instead.

SQLite foreign-key enforcement is enabled and connections use a five-second busy timeout. SQL values
are always bound through prepared statements, including source code containing quotes or embedded NUL
bytes.
