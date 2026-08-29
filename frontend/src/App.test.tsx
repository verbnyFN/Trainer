import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, describe, expect, it, vi } from "vitest";

import { App } from "./App";

vi.mock("@monaco-editor/react", () => ({
  default: ({ value, onChange }: { value: string; onChange: (value: string) => void }) => (
    <textarea aria-label="Code editor" value={value} onChange={(event) => onChange(event.target.value)} />
  ),
  loader: { config: vi.fn() },
}));
vi.mock("monaco-editor/esm/vs/editor/editor.api", () => ({
  editor: { TrackedRangeStickiness: { NeverGrowsWhenTypingAtEdges: 0 } },
}));
vi.mock("monaco-editor/esm/vs/basic-languages/python/python.contribution", () => ({}));
vi.mock("monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution", () => ({}));

const problem = {
  id: "a-plus-b",
  title: "A + B",
  description: "Add two integers.",
  inputFormat: "Two integers.",
  outputFormat: "Their sum.",
  difficulty: "Easy",
  tags: ["math"],
  languages: ["python", "cpp"],
  examples: [{ input: "2 3", output: "5" }],
};

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

afterEach(() => vi.useRealTimers());

describe("App", () => {
  it("shows authentication failures without entering the application", async () => {
    const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
      const url = String(input);
      if (url === "/api/auth/me") return json({ error: "Authentication required" }, 401);
      if (url === "/api/problems/a-plus-b") return json(problem);
      if (url === "/api/auth/login") return json({ error: "Invalid username or password" }, 401);
      throw new Error(`Unexpected request: ${url}`);
    });
    vi.stubGlobal("fetch", fetchMock);
    const user = userEvent.setup();
    render(<App />);

    await screen.findByRole("heading", { name: "Welcome back" });
    await user.type(screen.getByLabelText("Username"), "learner");
    await user.type(screen.getByLabelText("Password"), "incorrect-password");
    await user.click(screen.getByRole("button", { name: "Login" }));

    expect(await screen.findByRole("alert")).toHaveTextContent("Invalid username or password");
  });

  it("loads a problem, submits edited code, and displays the terminal verdict", async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const fetchMock = vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const url = String(input);
      if (url === "/api/auth/me") {
        return json({ id: 7, username: "learner", createdAt: "2026-01-01", isAdmin: false });
      }
      if (url === "/api/profile") {
        return json({
          user: { id: 7, username: "learner", createdAt: "2026-01-01", isAdmin: false },
          activity: { totalSubmissions: 0, acceptedSubmissions: 0, completedProblems: 0, currentStreakDays: 0 },
          completedProblems: [],
          mostRecentSubmissionProblemId: null,
        });
      }
      if (url === "/api/problems") return json([problem]);
      if (url === "/api/problems/a-plus-b/submissions") return json([]);
      if (url === "/api/problems/a-plus-b") return json(problem);
      if (url === "/api/submissions" && init?.method === "POST") {
        expect(JSON.parse(String(init.body))).toEqual({
          problemId: "a-plus-b",
          language: "python",
          code: "print(sum(map(int, input().split())))",
        });
        return json({ id: 42, problemId: "a-plus-b", language: "python", status: "Queued", createdAt: "2026-01-02" }, 202);
      }
      if (url === "/api/submissions/42") {
        return json({ id: 42, problemId: "a-plus-b", language: "python", status: "Completed", verdict: "Accepted", createdAt: "2026-01-02" });
      }
      throw new Error(`Unexpected request: ${url}`);
    });
    vi.stubGlobal("fetch", fetchMock);
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    render(<App />);

    await screen.findByRole("heading", { name: "learner" });
    await user.click(screen.getByRole("button", { name: "Problems" }));
    await user.click(await screen.findByRole("button", { name: /A \+ B/ }));
    await screen.findByRole("heading", { name: "A + B" });
    const editor = screen.getByLabelText("Code editor");
    await user.clear(editor);
    await user.type(editor, "print(sum(map(int, input().split())))");
    await user.click(screen.getByRole("button", { name: "Submit" }));

    await waitFor(() => expect(screen.getByRole("status")).toHaveTextContent("Accepted"));
    expect(fetchMock).toHaveBeenCalledWith("/api/submissions/42", { credentials: "include" });
  });
});
