import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import EditorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";

import { App } from "./App";
import "./styles.css";

self.MonacoEnvironment = {
  getWorker: () => new EditorWorker(),
};

const root = document.getElementById("root");

if (root === null) {
  throw new Error("Root element was not found");
}

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
