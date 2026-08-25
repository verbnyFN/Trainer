import Editor, { loader } from "@monaco-editor/react";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import "monaco-editor/esm/vs/basic-languages/python/python.contribution";
import { FormEvent, useEffect, useState } from "react";

loader.config({ monaco });

const starterCode = `a, b = map(int, input().split())
print(a + b)
`;

const dateFormatter = new Intl.DateTimeFormat("en-GB", {
  day: "2-digit",
  month: "2-digit",
  year: "2-digit",
});

function formatDate(value: string) {
  return dateFormatter.format(new Date(value)).replaceAll("/", ":");
}

type SubmissionResponse = {
  id?: number;
  error?: string;
  status?: string;
  verdict?: string;
  language?: string;
  createdAt?: string;
};

type SubmissionHistoryEntry = {
  id: number;
  language: string;
  status: string;
  verdict?: string;
  createdAt: string;
};

type ProblemExample = {
  input: string;
  output: string;
};

type Problem = {
  id: string;
  title: string;
  description: string;
  inputFormat: string;
  outputFormat: string;
  languages: string[];
  examples: ProblemExample[];
};

type ErrorResponse = {
  error?: string;
};

type AuthUser = {
  id: number;
  username: string;
  createdAt: string;
};

type AuthMode = "login" | "register";
type View = "problem" | "profile";

type ProfileData = {
  user: AuthUser;
  activity: {
    totalSubmissions: number;
    acceptedSubmissions: number;
    completedProblems: number;
  };
  completedProblems: Array<{
    id: string;
    title: string;
    completedAt: string;
  }>;
};

export function App() {
  const [code, setCode] = useState(starterCode);
  const [message, setMessage] = useState<string>();
  const [submitting, setSubmitting] = useState(false);
  const [problem, setProblem] = useState<Problem>();
  const [problemError, setProblemError] = useState<string>();
  const [user, setUser] = useState<AuthUser>();
  const [authLoading, setAuthLoading] = useState(true);
  const [authSubmitting, setAuthSubmitting] = useState(false);
  const [authError, setAuthError] = useState<string>();
  const [authMode, setAuthMode] = useState<AuthMode>("login");
  const [history, setHistory] = useState<SubmissionHistoryEntry[]>([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [historyError, setHistoryError] = useState<string>();
  const [view, setView] = useState<View>("profile");
  const [profile, setProfile] = useState<ProfileData>();
  const [profileLoading, setProfileLoading] = useState(false);
  const [profileError, setProfileError] = useState<string>();

  useEffect(() => {
    const controller = new AbortController();

    async function loadProblem() {
      try {
        const response = await fetch("/api/problems/a-plus-b", {
          signal: controller.signal,
        });
        const result = (await response.json()) as Problem | ErrorResponse;

        if (!response.ok) {
          const error = result as ErrorResponse;
          throw new Error(error.error ?? "Problem could not be loaded");
        }

        setProblem(result as Problem);
      } catch (error) {
        if (error instanceof DOMException && error.name === "AbortError") {
          return;
        }
        setProblemError(error instanceof Error ? error.message : "Problem could not be loaded");
      }
    }

    void loadProblem();
    return () => controller.abort();
  }, []);

  useEffect(() => {
    const controller = new AbortController();
    if (view !== "profile" || user === undefined) {
      return () => controller.abort();
    }

    async function loadProfile() {
      setProfileLoading(true);
      setProfileError(undefined);
      try {
        const response = await fetch("/api/profile", {
          credentials: "include",
          signal: controller.signal,
        });
        const result = (await response.json()) as ProfileData | ErrorResponse;
        if (!response.ok) {
          throw new Error((result as ErrorResponse).error ?? "Profile could not be loaded");
        }
        setProfile(result as ProfileData);
      } catch (error) {
        if (!(error instanceof DOMException && error.name === "AbortError")) {
          setProfileError(error instanceof Error ? error.message : "Profile could not be loaded");
        }
      } finally {
        if (!controller.signal.aborted) {
          setProfileLoading(false);
        }
      }
    }

    void loadProfile();
    return () => controller.abort();
  }, [user, view]);

  useEffect(() => {
    const controller = new AbortController();
    if (user === undefined || problem === undefined) {
      setHistory([]);
      setHistoryError(undefined);
      return () => controller.abort();
    }

    async function loadHistory() {
      setHistoryLoading(true);
      setHistoryError(undefined);
      try {
        const response = await fetch(`/api/problems/${problem!.id}/submissions`, {
          credentials: "include",
          signal: controller.signal,
        });
        const result = (await response.json()) as SubmissionHistoryEntry[] | ErrorResponse;
        if (!response.ok) {
          throw new Error((result as ErrorResponse).error ?? "Submission history could not be loaded");
        }
        setHistory(result as SubmissionHistoryEntry[]);
      } catch (error) {
        if (!(error instanceof DOMException && error.name === "AbortError")) {
          setHistoryError(error instanceof Error ? error.message : "Submission history could not be loaded");
        }
      } finally {
        if (!controller.signal.aborted) {
          setHistoryLoading(false);
        }
      }
    }

    void loadHistory();
    return () => controller.abort();
  }, [problem, user]);

  useEffect(() => {
    const controller = new AbortController();

    async function restoreSession() {
      try {
        const response = await fetch("/api/auth/me", {
          credentials: "include",
          signal: controller.signal,
        });
        if (response.ok) {
          setUser((await response.json()) as AuthUser);
          setView("profile");
        } else if (response.status !== 401) {
          const result = (await response.json()) as ErrorResponse;
          setAuthError(result.error ?? "Authentication state could not be loaded");
        }
      } catch (error) {
        if (!(error instanceof DOMException && error.name === "AbortError")) {
          setAuthError("Authentication state could not be loaded");
        }
      } finally {
        if (!controller.signal.aborted) {
          setAuthLoading(false);
        }
      }
    }

    void restoreSession();
    return () => controller.abort();
  }, []);

  async function authenticate(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    setAuthSubmitting(true);
    setAuthError(undefined);
    const formElement = event.currentTarget;
    const form = new FormData(formElement);

    try {
      const response = await fetch(`/api/auth/${authMode}`, {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          username: form.get("username"),
          password: form.get("password"),
        }),
      });
      const result = (await response.json()) as AuthUser | ErrorResponse;
      if (!response.ok) {
        throw new Error((result as ErrorResponse).error ?? "Authentication failed");
      }
      setUser(result as AuthUser);
      setView("profile");
      formElement.reset();
    } catch (error) {
      setAuthError(error instanceof Error ? error.message : "Authentication failed");
    } finally {
      setAuthSubmitting(false);
    }
  }

  async function logout() {
    setAuthSubmitting(true);
    setAuthError(undefined);
    try {
      const response = await fetch("/api/auth/logout", {
        method: "POST",
        credentials: "include",
      });
      if (!response.ok) {
        const result = (await response.json()) as ErrorResponse;
        throw new Error(result.error ?? "Logout failed");
      }
      setUser(undefined);
      setProfile(undefined);
      setView("problem");
    } catch (error) {
      setAuthError(error instanceof Error ? error.message : "Logout failed");
    } finally {
      setAuthSubmitting(false);
    }
  }

  async function submit() {
    if (problem === undefined) {
      return;
    }

    setSubmitting(true);
    setMessage(undefined);

    try {
      const response = await fetch("/api/submissions", {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          problemId: problem.id,
          language: "python",
          code,
        }),
      });
      const result = (await response.json()) as SubmissionResponse;

      if (!response.ok) {
        throw new Error(result.error ?? "Submission was rejected");
      }

      setMessage(result.verdict ?? "Submission completed");
      if (user !== undefined && result.id !== undefined && result.language !== undefined && result.createdAt !== undefined) {
        setHistory((current) => [
          {
            id: result.id!,
            language: result.language!,
            status: result.status ?? "completed",
            verdict: result.verdict,
            createdAt: result.createdAt!,
          },
          ...current.filter((entry) => entry.id !== result.id),
        ]);
      }
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "Submission failed");
    } finally {
      setSubmitting(false);
    }
  }

  if (authLoading) {
    return (
      <main className="auth-page">
        <section className="auth-card" role="status">
          <p className="eyebrow">Algorithm Trainer</p>
          <h1>Welcome</h1>
          <p>Checking your session…</p>
        </section>
      </main>
    );
  }

  if (user === undefined) {
    return (
      <main className="auth-page">
        <section className="auth-card" aria-labelledby="auth-title">
          <p className="eyebrow">Algorithm Trainer</p>
          <h1 id="auth-title">{authMode === "login" ? "Welcome back" : "Create your account"}</h1>
          <p>
            {authMode === "login"
              ? "Sign in to continue learning and track your progress."
              : "Register to save submissions and completed problems."}
          </p>
          <form className="auth-form" onSubmit={authenticate}>
            <label>
              Username
              <input name="username" type="text" autoComplete="username" required />
            </label>
            <label>
              Password
              <input
                name="password"
                type="password"
                autoComplete={authMode === "login" ? "current-password" : "new-password"}
                required
              />
            </label>
            <button type="submit" disabled={authSubmitting}>
              {authSubmitting ? "Please wait…" : authMode === "login" ? "Login" : "Register"}
            </button>
          </form>
          {authError !== undefined && <p className="auth-error" role="alert">{authError}</p>}
          <div className="auth-switch">
            <span>{authMode === "login" ? "New to Algorithm Trainer?" : "Already registered?"}</span>
            <button
              type="button"
              className="text-button"
              onClick={() => {
                setAuthMode(authMode === "login" ? "register" : "login");
                setAuthError(undefined);
              }}
            >
              {authMode === "login" ? "Register" : "Back to login"}
            </button>
          </div>
        </section>
      </main>
    );
  }

  return (
    <div className="app-shell">
      <header className="auth-bar" aria-label="Account">
        <strong>Algorithm Trainer</strong>
        <div className="account-summary">
          <span>
            Signed in as <strong>{user.username}</strong>
          </span>
          <button
            type="button"
            className="text-button"
            onClick={() => setView(view === "problem" ? "profile" : "problem")}
          >
            {view === "problem" ? "Profile" : "Open problem"}
          </button>
          <button type="button" className="secondary-button" onClick={logout} disabled={authSubmitting}>
            {authSubmitting ? "Signing out…" : "Logout"}
          </button>
        </div>
        {authError !== undefined && <p className="auth-error" role="alert">{authError}</p>}
      </header>

      {view === "profile" ? (
        <main className="profile-page">
          {profileLoading && profile === undefined ? (
            <p role="status">Loading profile…</p>
          ) : profileError !== undefined ? (
            <p className="profile-error" role="alert">{profileError}</p>
          ) : profile !== undefined ? (
            <>
              <section className="profile-heading">
                <p className="eyebrow">Profile</p>
                <h1>{profile.user.username}</h1>
                <p>
                  Member since <time dateTime={profile.user.createdAt}>{formatDate(profile.user.createdAt)}</time>
                </p>
              </section>

              <section aria-labelledby="activity-title">
                <h2 id="activity-title">Activity overview</h2>
                <dl className="activity-grid">
                  <div><dt>Submissions</dt><dd>{profile.activity.totalSubmissions}</dd></div>
                  <div><dt>Accepted</dt><dd>{profile.activity.acceptedSubmissions}</dd></div>
                  <div><dt>Problems completed</dt><dd>{profile.activity.completedProblems}</dd></div>
                </dl>
              </section>

              <section aria-labelledby="completed-title">
                <h2 id="completed-title">Completed problems</h2>
                {profile.completedProblems.length === 0 ? (
                  <p className="profile-empty">No completed problems yet. Your first accepted solution will appear here.</p>
                ) : (
                  <ul className="completed-list">
                    {profile.completedProblems.map((completed) => (
                      <li key={completed.id}>
                        <div><strong>{completed.title}</strong><span>{completed.id}</span></div>
                        <div><span>Completed</span><time dateTime={completed.completedAt}>{formatDate(completed.completedAt)}</time></div>
                      </li>
                    ))}
                  </ul>
                )}
              </section>
            </>
          ) : null}
        </main>
      ) : (
      <main className="workspace">
      <section className="problem" aria-labelledby="problem-title">
        {problem === undefined ? (
          <div className="problem-state" role="status">
            <p className="eyebrow">Problem</p>
            <h1 id="problem-title">{problemError === undefined ? "Loading…" : "Unavailable"}</h1>
            <p>{problemError ?? "Loading the problem statement from the backend."}</p>
          </div>
        ) : (
          <>
            <p className="eyebrow">Problem</p>
            <h1 id="problem-title">{problem.title}</h1>
            <p>{problem.description}</p>

            <h2>Input</h2>
            <p>{problem.inputFormat}</p>

            <h2>Output</h2>
            <p>{problem.outputFormat}</p>

            {problem.examples.map((example, index) => (
              <div key={`${example.input}-${example.output}`}>
                <h2>{problem.examples.length > 1 ? `Example ${index + 1}` : "Example"}</h2>
                <pre>
                  <strong>Input</strong>{"\n"}
                  {example.input}
                  {"\n"}
                  <strong>Output</strong>{"\n"}
                  {example.output}
                </pre>
              </div>
            ))}

            {user !== undefined && (
              <section className="history" aria-labelledby="history-title">
                <h2 id="history-title">Your submission history</h2>
                {historyLoading ? (
                  <p role="status">Loading history…</p>
                ) : historyError !== undefined ? (
                  <p className="history-error" role="alert">{historyError}</p>
                ) : history.length === 0 ? (
                  <p>No submissions for this problem yet.</p>
                ) : (
                  <ul className="history-list">
                    {history.map((entry) => {
                      const result = entry.verdict ?? (entry.status === "failed" ? "Judge Error" : "Pending");
                      return (
                        <li key={entry.id}>
                          <div>
                            <strong className={result === "Accepted" ? "history-success" : "history-failure"}>
                              {result}
                            </strong>
                            <span>{entry.language === "python" ? "Python 3" : entry.language}</span>
                          </div>
                          <time dateTime={entry.createdAt}>{formatDate(entry.createdAt)}</time>
                        </li>
                      );
                    })}
                  </ul>
                )}
              </section>
            )}
          </>
        )}
      </section>

      <section className="solution" aria-labelledby="solution-title">
        <div className="solution-header">
          <h2 id="solution-title">Solution</h2>
          <span>
            {problem?.languages.includes("python")
              ? "Python 3"
              : problemError === undefined
                ? "Loading…"
                : "Unavailable"}
          </span>
        </div>

        <div className="editor" aria-label="Python solution editor">
          <Editor
            height="100%"
            language="python"
            theme="vs-dark"
            value={code}
            onChange={(value) => setCode(value ?? "")}
            options={{
              minimap: { enabled: false },
              fontSize: 15,
              padding: { top: 16 },
              scrollBeyondLastLine: false,
            }}
          />
        </div>

        <footer className="submission-bar">
          <p role="status" className={message === "Accepted" ? "accepted" : undefined}>
            {message ?? "Ready to submit"}
          </p>
          <button
            type="button"
            onClick={submit}
            disabled={submitting || code.length === 0 || problem === undefined}
          >
            {submitting ? "Submitting…" : "Submit"}
          </button>
        </footer>
      </section>
      </main>
      )}
    </div>
  );
}
