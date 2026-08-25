CREATE TABLE submissions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  problem_id TEXT NOT NULL,
  language TEXT NOT NULL,
  source_code TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'completed', 'failed')),
  verdict TEXT CHECK (
    verdict IS NULL OR verdict IN (
      'Accepted',
      'Wrong Answer',
      'Runtime Error',
      'Time Limit Exceeded'
    )
  ),
  created_at TEXT NOT NULL,
  completed_at TEXT,
  CHECK (
    (status = 'pending' AND verdict IS NULL AND completed_at IS NULL) OR
    (status = 'completed' AND verdict IS NOT NULL AND completed_at IS NOT NULL) OR
    (status = 'failed' AND verdict IS NULL AND completed_at IS NOT NULL)
  )
);
