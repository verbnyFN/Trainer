ALTER TABLE problems ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0);
ALTER TABLE problem_tests ADD COLUMN revision INTEGER NOT NULL DEFAULT 1 CHECK (revision > 0);

ALTER TABLE submissions ADD COLUMN retry_of INTEGER REFERENCES submissions(id) ON DELETE SET NULL;
CREATE UNIQUE INDEX submissions_retry_once_index
  ON submissions(retry_of) WHERE retry_of IS NOT NULL;

CREATE TABLE admin_audit_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  admin_user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE RESTRICT,
  action TEXT NOT NULL,
  entity_type TEXT NOT NULL,
  entity_id TEXT NOT NULL,
  details_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);

CREATE INDEX admin_audit_created_index ON admin_audit_log(created_at DESC, id DESC);
CREATE INDEX submissions_admin_filter_index
  ON submissions(status, error_type, language, problem_id, user_id, created_at DESC);
