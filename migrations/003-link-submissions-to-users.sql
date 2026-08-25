ALTER TABLE submissions
  ADD COLUMN user_id INTEGER REFERENCES users(id) ON DELETE SET NULL;

CREATE INDEX submissions_user_problem_created_index
  ON submissions(user_id, problem_id, created_at DESC);
