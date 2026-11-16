import React, { Suspense, lazy } from 'react';

// Lazy-load Monaco Editor
const MonacoEditor = lazy(() => import('@monaco-editor/react'));

function FallbackEditor({ value, onChange, language, readOnly = false }) {
  return (
    <div className="relative w-full h-full bg-zinc-950 font-mono text-xs flex flex-col">
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
  readOnly = false
}) {
  const monacoLanguage = language === 'cpp' ? 'cpp' : 'c';

  // Check if we are running in an environment without DOM or window.matchMedia (like jsdom in tests)
  const isTestEnv = typeof window === 'undefined' || !window.matchMedia;

  if (isTestEnv) {
    return (
      <FallbackEditor
        value={code}
        onChange={onChange}
        language={monacoLanguage}
        readOnly={readOnly}
      />
    );
  }

  return (
    <div className="w-full h-full relative bg-zinc-950 overflow-hidden" data-testid="monaco-editor-container">
      <Suspense fallback={<FallbackEditor value={code} onChange={onChange} language={monacoLanguage} readOnly={readOnly} />}>
        <MonacoEditor
          height="100%"
          language={monacoLanguage}
          theme="vs-dark"
          value={code}
          onChange={(val) => onChange && onChange(val || '')}
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
