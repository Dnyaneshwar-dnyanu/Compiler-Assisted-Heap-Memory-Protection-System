import React, { Suspense, lazy, useRef, useEffect } from 'react';

// Lazy-load Monaco Editor
const MonacoEditor = lazy(() => import('@monaco-editor/react'));

function FallbackEditor({ value, onChange, language, readOnly = false, staticViolations = [] }) {
  return (
    <div className="relative w-full h-full bg-zinc-950 font-mono text-xs flex flex-col">
      {staticViolations && staticViolations.length > 0 && (
        <div data-testid="editor-static-violations-indicator" className="p-2.5 bg-rose-950/80 border-b border-rose-800 text-rose-300 text-xs font-mono space-y-1 shrink-0">
          <div className="font-semibold text-rose-200 flex items-center gap-1.5">
            <span>Compile-Time Memory Violation Detected:</span>
          </div>
          {staticViolations.map((v, i) => (
            <div key={i} data-testid={`static-violation-marker-line-${v.line}`} className="text-[11px] text-rose-300/90 pl-2 border-l border-rose-700">
              Line {v.line}{v.column ? `:${v.column}` : ''} &mdash; {v.message}
            </div>
          ))}
        </div>
      )}
      <textarea
        data-testid="code-editor-textarea"
        value={value}
        onChange={(e) => onChange && onChange(e.target.value)}
        readOnly={readOnly}
        spellCheck="false"
        className="w-full h-full p-4 bg-zinc-950 text-zinc-100 font-mono text-xs leading-relaxed resize-none focus:outline-none border-0"
        placeholder={`Enter your ${language.toUpperCase()} source code here...`}
      />
    </div>
  );
}

export function CodeEditor({
  code,
  onChange,
  language = 'c',
  readOnly = false,
  staticViolations = []
}) {
  const monacoLanguage = language === 'cpp' ? 'cpp' : 'c';
  const editorRef = useRef(null);
  const monacoRef = useRef(null);
  const decorationsRef = useRef([]);

  // Check if we are running in an environment without DOM or window.matchMedia (like jsdom in tests)
  const isTestEnv = typeof window === 'undefined' || !window.matchMedia;

  const handleEditorDidMount = (editor, monaco) => {
    editorRef.current = editor;
    monacoRef.current = monaco;
  };

  useEffect(() => {
    if (!editorRef.current || !monacoRef.current) return;
    const editor = editorRef.current;
    const monaco = monacoRef.current;
    const model = editor.getModel();
    if (!model) return;

    if (!staticViolations || staticViolations.length === 0) {
      decorationsRef.current = editor.deltaDecorations(decorationsRef.current, []);
      monaco.editor.setModelMarkers(model, 'memoryguard', []);
      return;
    }

    const markers = staticViolations
      .filter((v) => v.line > 0)
      .map((v) => ({
        severity: monaco.MarkerSeverity.Error,
        message: `[MemoryGuard Static Violation] ${v.message}`,
        startLineNumber: v.line,
        startColumn: v.column || 1,
        endLineNumber: v.line,
        endColumn: 1000
      }));

    monaco.editor.setModelMarkers(model, 'memoryguard', markers);

    const decorations = staticViolations
      .filter((v) => v.line > 0)
      .map((v) => ({
        range: new monaco.Range(v.line, 1, v.line, 1),
        options: {
          isWholeLine: true,
          className: 'bg-rose-950/40 border-l-4 border-rose-500',
          glyphMarginClassName: 'text-rose-500 font-bold',
          hoverMessage: { value: `**MemoryGuard Static Violation**:\n${v.message}` }
        }
      }));

    decorationsRef.current = editor.deltaDecorations(decorationsRef.current, decorations);
  }, [staticViolations]);

  if (isTestEnv) {
    return (
      <FallbackEditor
        value={code}
        onChange={onChange}
        language={monacoLanguage}
        readOnly={readOnly}
        staticViolations={staticViolations}
      />
    );
  }

  return (
    <div className="w-full h-full relative bg-zinc-950 overflow-hidden" data-testid="monaco-editor-container">
      <Suspense fallback={<FallbackEditor value={code} onChange={onChange} language={monacoLanguage} readOnly={readOnly} staticViolations={staticViolations} />}>
        <MonacoEditor
          height="100%"
          language={monacoLanguage}
          theme="vs-dark"
          value={code}
          onChange={(val) => onChange && onChange(val || '')}
          onMount={handleEditorDidMount}
          options={{
            readOnly,
            fontSize: 13,
            lineHeight: 20,
            fontFamily: "'JetBrains Mono', 'Fira Code', Consolas, monospace",
            minimap: { enabled: false },
            scrollBeyondLastLine: false,
            automaticLayout: true,
            tabSize: 4,
            bracketPairColorization: { enabled: true },
            lineNumbers: 'on',
            folding: true,
            renderWhitespace: 'selection',
            padding: { top: 12, bottom: 12 }
          }}
          loading={
            <div className="w-full h-full flex items-center justify-center bg-zinc-950 text-zinc-500 font-mono text-xs">
              Loading Monaco Editor...
            </div>
          }
        />
      </Suspense>
    </div>
  );
}
