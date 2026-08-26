import Editor, { loader } from "@monaco-editor/react";
import * as monaco from "monaco-editor/esm/vs/editor/editor.api";
import "monaco-editor/esm/vs/basic-languages/python/python.contribution";
import { FormEvent, useEffect, useState } from "react";

loader.config({ monaco });

const starterCode = `a, b = map(int, input().split())
print(a + b)
`;

type SubmissionResponse = {
  error?: string;
  status?: string;
  verdict?: string;
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

    async function restoreSession() {
      try {
        const response = await fetch("/api/auth/me", {
          credentials: "include",
          signal: controller.signal,
        });
        if (response.ok) {
          setUser((await response.json()) as AuthUser);
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
    const form = new FormData(event.currentTarget);

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
      event.currentTarget.reset();
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
    } catch (error) {
      setMessage(error instanceof Error ? error.message : "Submission failed");
    } finally {
      setSubmitting(false);
    }
  }

  return (
    <div className="app-shell">
      <header className="auth-bar" aria-label="Account">
        <strong>Algorithm Trainer</strong>
        {authLoading ? (
          <span role="status">Checking session…</span>
        ) : user !== undefined ? (
          <div className="account-summary">
            <span>
              Signed in as <strong>{user.username}</strong>
            </span>
            <button type="button" className="secondary-button" onClick={logout} disabled={authSubmitting}>
              {authSubmitting ? "Signing out…" : "Logout"}
            </button>
          </div>
        ) : (
          <form className="auth-form" onSubmit={authenticate}>
            <label>
              <span className="visually-hidden">Username</span>
              <input name="username" type="text" autoComplete="username" placeholder="Username" required />
            </label>
            <label>
              <span className="visually-hidden">Password</span>
              <input
                name="password"
                type="password"
                autoComplete={authMode === "login" ? "current-password" : "new-password"}
                placeholder="Password"
                required
              />
            </label>
            <button type="submit" disabled={authSubmitting}>
              {authSubmitting ? "Please wait…" : authMode === "login" ? "Login" : "Register"}
            </button>
            <button
              type="button"
              className="text-button"
              onClick={() => {
                setAuthMode(authMode === "login" ? "register" : "login");
                setAuthError(undefined);
              }}
            >
              {authMode === "login" ? "Create account" : "Use login"}
            </button>
          </form>
        )}
        {authError !== undefined && <p className="auth-error" role="alert">{authError}</p>}
      </header>

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
    </div>
  );
}
