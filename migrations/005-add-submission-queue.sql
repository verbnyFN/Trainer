CREATE TABLE submissions_with_queue (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  problem_id TEXT NOT NULL,
  language TEXT NOT NULL,
  source_code TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('queued', 'running', 'completed', 'failed')),
  verdict TEXT CHECK (
    verdict IS NULL OR verdict IN (
      'Accepted', 'Wrong Answer', 'Runtime Error', 'Time Limit Exceeded'
    )
  ),
  created_at TEXT NOT NULL,
  completed_at TEXT,
  user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
  error_type TEXT,
  CHECK (
    (status IN ('queued', 'running') AND verdict IS NULL AND completed_at IS NULL) OR
    (status = 'completed' AND verdict IS NOT NULL AND completed_at IS NOT NULL) OR
    (status = 'failed' AND verdict IS NULL AND completed_at IS NOT NULL)
  )
);

INSERT INTO submissions_with_queue(
  id, problem_id, language, source_code, status, verdict, created_at,
  completed_at, user_id, error_type
)
SELECT
  id, problem_id, language, source_code,
  CASE WHEN status = 'pending' THEN 'queued' ELSE status END,
  verdict, created_at, completed_at, user_id, error_type
FROM submissions;

DROP TABLE submissions;
ALTER TABLE submissions_with_queue RENAME TO submissions;

CREATE INDEX submissions_user_problem_created_index
  ON submissions(user_id, problem_id, created_at DESC);
CREATE INDEX submissions_queue_index ON submissions(status, id);
