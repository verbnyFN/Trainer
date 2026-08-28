import { FormEvent, useCallback, useEffect, useRef, useState } from "react";

type Example = { input: string; output: string };
type HiddenTest = { id: number; input: string; expectedOutput: string; position: number; enabled: boolean; revision: number };
type AdminProblem = {
  id: string; title: string; description: string; inputFormat: string; outputFormat: string;
  difficulty: "Easy" | "Medium" | "Hard"; tags: string[]; languages: string[];
  examples: Example[]; enabled: boolean; revision: number; tests: HiddenTest[];
};
type ProblemSummary = Pick<AdminProblem, "id" | "title" | "difficulty" | "enabled">;
type AdminSubmission = {
  id: number; problemId: string; language: string; code: string; status: string; verdict?: string;
  errorType?: string; userId: number | null; createdAt: string; retryOf?: number;
  diagnostic: { category: string; message: string };
};
type ErrorBody = { error?: string };

async function api<T>(url: string, init?: RequestInit): Promise<T> {
  const response = await fetch(url, { credentials: "include", ...init });
  const body = response.status === 204 ? undefined : await response.json() as T | ErrorBody;
  if (!response.ok) throw new Error((body as ErrorBody | undefined)?.error ?? "Request failed");
  return body as T;
}

const emptyProblem: AdminProblem = {
  id: "", title: "", description: "", inputFormat: "", outputFormat: "", difficulty: "Easy",
  tags: [], languages: ["python"], examples: [], enabled: false, revision: 1, tests: [],
};

export function AdminPanel() {
  const [section, setSection] = useState<"problems" | "submissions">("problems");
  return <main className="admin-page">
    <div className="admin-heading"><div><p className="eyebrow">Administration</p><h1>Control room</h1></div>
      <nav aria-label="Admin sections"><button className={section === "problems" ? "active" : "secondary-button"} onClick={() => setSection("problems")}>Problems</button><button className={section === "submissions" ? "active" : "secondary-button"} onClick={() => setSection("submissions")}>Submissions</button></nav>
    </div>
    {section === "problems" ? <ProblemAdmin /> : <SubmissionAdmin />}
  </main>;
}

function ProblemAdmin() {
  const [items, setItems] = useState<ProblemSummary[]>([]);
  const [problem, setProblem] = useState<AdminProblem>();
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string>();
  const [notice, setNotice] = useState<string>();

  const loadList = useCallback(async () => {
    setLoading(true); setError(undefined);
    try { setItems(await api<ProblemSummary[]>("/api/admin/problems")); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Problems could not be loaded"); }
    finally { setLoading(false); }
  }, []);
  useEffect(() => { void loadList(); }, [loadList]);

  async function select(id: string) {
    setLoading(true); setError(undefined); setNotice(undefined);
    try { setProblem(await api<AdminProblem>(`/api/admin/problems/${id}`)); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Problem could not be loaded"); }
    finally { setLoading(false); }
  }

  async function save(event: FormEvent) {
    event.preventDefault(); if (!problem) return;
    setSaving(true); setError(undefined); setNotice(undefined);
    const creating = !items.some((entry) => entry.id === problem.id);
    const problemToSave = { ...problem, tags: problem.tags.map((tag) => tag.trim()).filter(Boolean) };
    try {
      const saved = await api<AdminProblem>(creating ? "/api/admin/problems" : `/api/admin/problems/${problem.id}`, {
        method: creating ? "POST" : "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(problemToSave),
      });
      setProblem((current) => ({ ...saved, tests: current?.tests ?? [] }));
      setNotice(creating ? "Problem created." : "Problem saved."); await loadList();
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Problem could not be saved"); }
    finally { setSaving(false); }
  }

  return <div className="admin-problem-layout">
    <aside className="admin-card"><div className="admin-card-title"><h2>Problems</h2><button onClick={() => { setProblem({ ...emptyProblem }); setError(undefined); setNotice(undefined); }}>New</button></div>
      {loading && items.length === 0 ? <p role="status">Loading problems…</p> : error && !problem ? <p className="admin-error" role="alert">{error}</p> :
        <ul className="admin-list">{items.map((item) => <li key={item.id}><button onClick={() => void select(item.id)}><strong>{item.title}</strong><span>{item.id} · {item.enabled ? "Enabled" : "Disabled"}</span></button></li>)}</ul>}
    </aside>
    <section className="admin-card admin-editor">
      {!problem ? <p className="admin-empty">Select a problem or create a new one.</p> : <>
        <form onSubmit={save} className="admin-form">
          <div className="admin-card-title"><h2>{problem.title || "New problem"}</h2><label className="toggle"><input type="checkbox" checked={problem.enabled} onChange={(e) => setProblem({ ...problem, enabled: e.target.checked })} /> Enabled</label></div>
          <div className="form-grid"><label>Slug<input required maxLength={80} pattern="[a-z0-9]+(?:-[a-z0-9]+)*" disabled={items.some((entry) => entry.id === problem.id)} value={problem.id} onChange={(e) => setProblem({ ...problem, id: e.target.value })} /></label><label>Title<input required maxLength={200} value={problem.title} onChange={(e) => setProblem({ ...problem, title: e.target.value })} /></label><label>Difficulty<select value={problem.difficulty} onChange={(e) => setProblem({ ...problem, difficulty: e.target.value as AdminProblem["difficulty"] })}><option>Easy</option><option>Medium</option><option>Hard</option></select></label></div>
          <fieldset className="hashtag-editor">
            <legend>Hashtags</legend>
            {problem.tags.map((tag, index) => <div className="hashtag-row" key={index}>
              <label><span className="visually-hidden">Hashtag {index + 1}</span><input value={tag} onChange={(e) => setProblem({ ...problem, tags: problem.tags.map((item, itemIndex) => itemIndex === index ? e.target.value : item) })} /></label>
              <button type="button" className="danger-button" aria-label={`Remove hashtag ${tag || index + 1}`} onClick={() => setProblem({ ...problem, tags: problem.tags.filter((_, itemIndex) => itemIndex !== index) })}>Remove</button>
            </div>)}
            <button type="button" className="secondary-button add-hashtag-button" disabled={problem.tags.length >= 50} onClick={() => setProblem({ ...problem, tags: [...problem.tags, ""] })}>Add new hashtag</button>
          </fieldset>
          <label>Description<textarea required value={problem.description} onChange={(e) => setProblem({ ...problem, description: e.target.value })} /></label>
          <div className="form-grid"><label>Input format<textarea required value={problem.inputFormat} onChange={(e) => setProblem({ ...problem, inputFormat: e.target.value })} /></label><label>Output format<textarea required value={problem.outputFormat} onChange={(e) => setProblem({ ...problem, outputFormat: e.target.value })} /></label></div>
          {error && <p className="admin-error" role="alert">{error}</p>}{notice && <p className="admin-notice" role="status">{notice}</p>}
          <button disabled={saving}>{saving ? "Saving…" : "Save metadata"}</button>
        </form>
        {items.some((entry) => entry.id === problem.id) && <HiddenTests problem={problem} onChange={setProblem} onError={setError} />}
      </>}
    </section>
  </div>;
}

function HiddenTests({ problem, onChange, onError }: { problem: AdminProblem; onChange: (value: AdminProblem) => void; onError: (value?: string) => void }) {
  const [draft, setDraft] = useState<Omit<HiddenTest, "id" | "revision">>({ input: "", expectedOutput: "", position: problem.tests.length, enabled: true });
  const [editingId, setEditingId] = useState<number>();
  const [editingRevision, setEditingRevision] = useState<number>();
  const [saving, setSaving] = useState(false);
  const editorRef = useRef<HTMLFormElement>(null);
  function resetEditor() {
    setDraft({ input: "", expectedOutput: "", position: problem.tests.length, enabled: true });
    setEditingId(undefined);
    setEditingRevision(undefined);
  }
  useEffect(resetEditor, [problem.id]);
  function edit(test: HiddenTest) {
    setEditingId(test.id);
    setEditingRevision(test.revision);
    setDraft({ input: test.input, expectedOutput: test.expectedOutput, position: test.position, enabled: test.enabled });
    requestAnimationFrame(() => {
      editorRef.current?.scrollIntoView({ behavior: "smooth", block: "center" });
      editorRef.current?.querySelector("textarea")?.focus();
    });
  }
  async function createTest(event: FormEvent) {
    event.preventDefault(); setSaving(true); onError(undefined);
    try { const test = await api<HiddenTest>(editingId === undefined ? `/api/admin/problems/${problem.id}/tests` : `/api/admin/problems/${problem.id}/tests/${editingId}`, { method: editingId === undefined ? "POST" : "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ...draft, revision: editingRevision }) }); const tests = editingId === undefined ? [...problem.tests, test] : problem.tests.map((item) => item.id === editingId ? test : item); onChange({ ...problem, tests: tests.sort((a, b) => a.position - b.position) }); setDraft({ input: "", expectedOutput: "", position: Math.max(0, ...tests.map((item) => item.position)) + 1, enabled: true }); setEditingId(undefined); setEditingRevision(undefined); }
    catch (reason) { onError(reason instanceof Error ? reason.message : "Test could not be created"); }
    finally { setSaving(false); }
  }
  async function remove(test: HiddenTest) {
    try { await api<void>(`/api/admin/problems/${problem.id}/tests/${test.id}`, { method: "DELETE" }); onChange({ ...problem, tests: problem.tests.filter((item) => item.id !== test.id) }); }
    catch (reason) { onError(reason instanceof Error ? reason.message : "Test could not be deleted"); }
  }
  async function toggle(test: HiddenTest) {
    try { const changed = await api<HiddenTest>(`/api/admin/problems/${problem.id}/tests/${test.id}`, { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ ...test, enabled: !test.enabled }) }); onChange({ ...problem, tests: problem.tests.map((item) => item.id === test.id ? changed : item) }); }
    catch (reason) { onError(reason instanceof Error ? reason.message : "Test could not be updated"); }
  }
  return <section className="hidden-tests"><h3>Hidden tests</h3><p>Visible only to administrators and the judge.</p>
    <ul>{problem.tests.map((test) => <li key={test.id}><span>#{test.position} · {test.enabled ? "Enabled" : "Disabled"}</span><div><button type="button" className="secondary-button" onClick={() => edit(test)}>Edit</button><button type="button" className="secondary-button" onClick={() => void toggle(test)}>{test.enabled ? "Disable" : "Enable"}</button><button type="button" className="danger-button" onClick={() => void remove(test)}>Delete</button></div></li>)}</ul>
    <form ref={editorRef} className="admin-form hidden-test-editor" onSubmit={createTest}><h4>{editingId === undefined ? "Add hidden test" : `Edit hidden test #${draft.position}`}</h4><div className="form-grid"><label>Input<textarea required maxLength={1048576} value={draft.input} onChange={(e) => setDraft({ ...draft, input: e.target.value })} /></label><label>Expected output<textarea required maxLength={1048576} value={draft.expectedOutput} onChange={(e) => setDraft({ ...draft, expectedOutput: e.target.value })} /></label></div><label>Order<input type="number" min="0" required value={draft.position} onChange={(e) => setDraft({ ...draft, position: Number(e.target.value) })} /></label><div className="hidden-test-editor-actions"><button disabled={saving}>{saving ? "Saving…" : editingId === undefined ? "Add hidden test" : "Save hidden test"}</button>{editingId !== undefined && <button type="button" className="secondary-button" onClick={resetEditor}>Cancel editing</button>}</div></form>
  </section>;
}

function SubmissionAdmin() {
  const [items, setItems] = useState<AdminSubmission[]>([]); const [selected, setSelected] = useState<AdminSubmission>();
  const [loading, setLoading] = useState(false); const [error, setError] = useState<string>();
  async function filter(event?: FormEvent<HTMLFormElement>) { event?.preventDefault(); setLoading(true); setError(undefined); const form = event ? new FormData(event.currentTarget) : new FormData(); const query = new URLSearchParams(); form.forEach((value, key) => { if (value) query.set(key, String(value)); }); try { setItems(await api<AdminSubmission[]>(`/api/admin/submissions?${query}`)); setSelected(undefined); } catch (reason) { setError(reason instanceof Error ? reason.message : "Submissions could not be loaded"); } finally { setLoading(false); } }
  useEffect(() => { void filter(); }, []);
  async function retry(item: AdminSubmission) { setError(undefined); try { const queued = await api<AdminSubmission>(`/api/admin/submissions/${item.id}/retry`, { method: "POST" }); setItems((current) => [queued, ...current]); } catch (reason) { setError(reason instanceof Error ? reason.message : "Submission could not be retried"); } }
  return <section className="admin-card"><h2>Submission inspection</h2><form className="filter-grid" onSubmit={filter}><label>Status<select name="status"><option value="">Any</option><option>Failed</option><option>Completed</option><option>Queued</option><option>Running</option></select></label><label>Runtime error<input name="errorType" placeholder="e.g. memory-limit" /></label><label>Language<input name="language" /></label><label>Problem<input name="problemId" /></label><label>User ID<input name="userId" type="number" min="1" /></label><label>From<input name="from" type="datetime-local" /></label><label>To<input name="to" type="datetime-local" /></label><button disabled={loading}>{loading ? "Filtering…" : "Apply filters"}</button></form>{error && <p className="admin-error" role="alert">{error}</p>}
    <div className="submission-admin-layout"><ul className="admin-list">{items.map((item) => <li key={item.id}><button onClick={() => setSelected(item)}><strong>#{item.id} · {item.problemId}</strong><span>{item.errorType ?? item.verdict ?? item.status} · {item.language}</span></button></li>)}</ul>{selected && <article className="submission-detail"><h3>Submission #{selected.id}</h3><dl><div><dt>Status</dt><dd>{selected.status}</dd></div><div><dt>User</dt><dd>{selected.userId ?? "Anonymous"}</dd></div><div><dt>Diagnostic</dt><dd>{selected.diagnostic.category}: {selected.diagnostic.message}</dd></div></dl><h4>Submitted source</h4><pre><code>{selected.code}</code></pre>{(selected.status === "Failed" || selected.verdict === "Runtime Error") && !selected.retryOf && <button onClick={() => void retry(selected)}>Retry once</button>}</article>}</div>
  </section>;
}
