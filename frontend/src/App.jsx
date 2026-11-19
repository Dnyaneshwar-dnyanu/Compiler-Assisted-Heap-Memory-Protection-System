import React, { useState, useCallback } from 'react';
import { Header } from './components/Header/Header.jsx';
import { Toolbar } from './components/Toolbar/Toolbar.jsx';
import { CodeEditor } from './components/Editor/CodeEditor.jsx';
import { OutputPanel } from './components/OutputPanel/OutputPanel.jsx';
import { AnalysisSummary } from './components/AnalysisSummary/AnalysisSummary.jsx';
import { DiagnosticsPanel } from './components/DiagnosticsPanel/DiagnosticsPanel.jsx';
import { useBackendHealth } from './hooks/useBackendHealth.js';
import { compileSource } from './services/compilerService.js';
import { executeJob } from './services/executionService.js';
import { SAMPLE_CODES, getDefaultSample } from './components/Editor/sampleCodes.js';
import { Terminal, Activity, FileCode, CheckCircle2, AlertTriangle, ShieldCheck } from 'lucide-react';

export default function App() {
  const { isOnline, dockerStatus, isChecking, checkHealth } = useBackendHealth();

  // Selected language: 'c' | 'cpp'
  const [language, setLanguage] = useState('c');

  // Maintain separate code buffers per language to preserve user edits across language toggling
  const [codeBuffers, setCodeBuffers] = useState({
    c: getDefaultSample('c').code,
    cpp: getDefaultSample('cpp').code
  });

  // Active editor code
  const currentCode = codeBuffers[language];

  // Compilation state
  const [isCompiling, setIsCompiling] = useState(false);
  const [compileResult, setCompileResult] = useState(null);
  const [compileError, setCompileError] = useState(null);

  // Execution state
  const [isRunning, setIsRunning] = useState(false);
  const [executionResult, setExecutionResult] = useState(null);
  const [executionError, setExecutionError] = useState(null);

  // Active result tab: 'output' | 'analysis' | 'diagnostics'
  const [activeTab, setActiveTab] = useState('output');

  // Invalidate any active compilation and execution results when code or language changes
  const invalidateCompilationState = useCallback(() => {
    setCompileResult(null);
    setCompileError(null);
    setExecutionResult(null);
    setExecutionError(null);
  }, []);

  // Code change handler
  const handleCodeChange = useCallback((newCode) => {
    setCodeBuffers((prev) => {
      if (prev[language] === newCode) return prev;
      return {
        ...prev,
        [language]: newCode
      };
    });
    // Invalidate previous compilation so modified code must be recompiled before running
    invalidateCompilationState();
  }, [language, invalidateCompilationState]);

  // Language switch handler
  const handleLanguageChange = useCallback((newLang) => {
    if (newLang !== language) {
      setLanguage(newLang);
      invalidateCompilationState();
    }
  }, [language, invalidateCompilationState]);

  // Reset editor to default sample
  const handleResetCode = useCallback(() => {
    const defaultSample = getDefaultSample(language);
    setCodeBuffers((prev) => ({
      ...prev,
      [language]: defaultSample.code
    }));
    invalidateCompilationState();
  }, [language, invalidateCompilationState]);

  // Choose a specific sample program
  const handleSelectSample = useCallback((sampleId) => {
    const samples = SAMPLE_CODES[language] || [];
    const found = samples.find((s) => s.id === sampleId);
    if (found) {
      setCodeBuffers((prev) => ({
        ...prev,
        [language]: found.code
      }));
      invalidateCompilationState();
    }
  }, [language, invalidateCompilationState]);

  // Can the program be run?
  const hasJob = Boolean(compileResult?.jobId);
  const canRun = Boolean(
    compileResult?.jobId &&
    compileResult?.artifacts?.executableAvailable &&
    !compileError &&
    !isCompiling &&
    !isRunning
  );

  // Handle Compile & Analyze
  const handleCompile = useCallback(async () => {
    if (isCompiling || isRunning) return;

    setIsCompiling(true);
    // Invalidate previous compilation result immediately so failed runs cannot execute older binaries
    setCompileResult(null);
    setCompileError(null);
    setExecutionResult(null);
    setExecutionError(null);

    try {
      const data = await compileSource({
        sourceCode: currentCode,
        language
      });
      setCompileResult(data);
      setCompileError(null);
      // If checks were inserted, take user to the analysis dashboard
      if (data?.diagnostics?.summary) {
        setActiveTab('analysis');
      } else {
        setActiveTab('diagnostics');
      }
    } catch (err) {
      setCompileResult(null);
      setCompileError({
        message: err.message,
        code: err.code || 'COMPILATION_ERROR',
        stage: err.stage,
        diagnostics: err.diagnostics || err.message,
        stages: err.details?.stages || [],
        staticViolations: err.staticViolations || err.raw?.error?.staticViolations || []
      });
      setActiveTab('diagnostics');
    } finally {
      setIsCompiling(false);
    }
  }, [currentCode, language, isCompiling, isRunning]);

  // Handle Run
  const handleRun = useCallback(async () => {
    if (!canRun || isRunning || isCompiling) return;

    setIsRunning(true);
    setExecutionError(null);
    setActiveTab('output');

    try {
      const data = await executeJob(compileResult.jobId);
      setExecutionResult(data);
      setExecutionError(null);
    } catch (err) {
      setExecutionResult(null);
      setExecutionError({
        message: err.message,
        code: err.code || 'EXECUTION_FAILED',
        details: err.details
      });
    } finally {
      setIsRunning(false);
    }
  }, [canRun, isRunning, isCompiling, compileResult]);

  return (
    <div className="flex flex-col h-screen w-screen bg-zinc-950 text-zinc-100 overflow-hidden font-sans">
      {/* Global Header */}
      <Header
        isOnline={isOnline}
        dockerStatus={dockerStatus}
        isChecking={isChecking}
        onRefreshHealth={checkHealth}
      />

      {/* Main Action Toolbar */}
      <Toolbar
        language={language}
        onLanguageChange={handleLanguageChange}
        onSelectSample={handleSelectSample}
        onResetCode={handleResetCode}
        onCompile={handleCompile}
        onRun={handleRun}
        isCompiling={isCompiling}
        isRunning={isRunning}
        canRun={canRun}
        hasJob={hasJob}
      />

      {/* Split Workspace: Left = Editor, Right = Results */}
      <div className="flex-1 flex flex-col lg:flex-row min-h-0 overflow-hidden">
        {/* Left Column: Monaco Code Editor */}
        <div className="flex-1 flex flex-col min-h-0 border-b lg:border-b-0 lg:border-r border-zinc-800 bg-zinc-950">
          <div className="border-b border-zinc-800/80 bg-zinc-900/40 px-3 py-1.5 flex items-center justify-between text-xs text-zinc-400">
            <span className="flex items-center gap-1.5 font-mono text-[11px]">
              <FileCode className="w-3.5 h-3.5 text-zinc-500" />
              main.{language === 'cpp' ? 'cpp' : 'c'}
            </span>
            <span className="text-[10px] text-zinc-500 font-mono">
              {currentCode.length} bytes
            </span>
          </div>
          <div className="flex-1 min-h-[260px] lg:min-h-0 relative">
            <CodeEditor
              code={currentCode}
              onChange={handleCodeChange}
              language={language}
              readOnly={isCompiling || isRunning}
              staticViolations={compileError?.staticViolations || []}
            />
          </div>
        </div>

        {/* Right Column: Multi-tab Results & Analysis Area */}
        <div className="flex-1 flex flex-col min-h-0 bg-zinc-900/20">
          {/* Tab Navigation */}
          <div className="border-b border-zinc-800 bg-zinc-900/50 px-2 flex items-center gap-1 shrink-0">
            <button
              onClick={() => setActiveTab('output')}
              className={`flex items-center gap-1.5 px-3 py-2 text-xs font-medium border-b-2 transition-colors ${
                activeTab === 'output'
                  ? 'border-emerald-500 text-emerald-400 bg-emerald-500/5'
                  : 'border-transparent text-zinc-400 hover:text-zinc-200'
              }`}
            >
              <Terminal className="w-3.5 h-3.5" />
              <span>Runtime Output</span>
              {executionResult?.status === 'violation_detected' && (
                <span className="w-2 h-2 rounded-full bg-rose-400 animate-pulse" />
              )}
            </button>

            <button
              onClick={() => setActiveTab('analysis')}
              className={`flex items-center gap-1.5 px-3 py-2 text-xs font-medium border-b-2 transition-colors ${
                activeTab === 'analysis'
                  ? 'border-indigo-500 text-indigo-400 bg-indigo-500/5'
                  : 'border-transparent text-zinc-400 hover:text-zinc-200'
              }`}
            >
              <ShieldCheck className="w-3.5 h-3.5" />
              <span>Analysis Dashboard</span>
              {compileResult?.diagnostics?.summary && (
                <span className="text-[10px] px-1 py-0.2 rounded bg-indigo-500/20 text-indigo-300 font-mono">
                  {compileResult.diagnostics.summary.checksInserted ?? 0}
                </span>
              )}
            </button>

            <button
              onClick={() => setActiveTab('diagnostics')}
              className={`flex items-center gap-1.5 px-3 py-2 text-xs font-medium border-b-2 transition-colors ${
                activeTab === 'diagnostics'
                  ? 'border-indigo-500 text-indigo-400 bg-indigo-500/5'
                  : 'border-transparent text-zinc-400 hover:text-zinc-200'
              }`}
            >
              <Activity className="w-3.5 h-3.5" />
              <span>Compiler Stages &amp; Logs</span>
              {compileError && (
                <span className="w-2 h-2 rounded-full bg-rose-400" />
              )}
            </button>
          </div>

          {/* Tab Content Display */}
          <div className="flex-1 min-h-0 relative overflow-hidden bg-zinc-950/60">
            {activeTab === 'output' && (
              <OutputPanel
                executionResult={executionResult}
                executionError={executionError}
                isRunning={isRunning}
              />
            )}

            {activeTab === 'analysis' && (
              <AnalysisSummary
                diagnostics={compileResult?.diagnostics}
                metadata={compileResult?.metadata}
                stages={compileResult?.stages}
              />
            )}

            {activeTab === 'diagnostics' && (
              <DiagnosticsPanel
                compileResult={compileResult}
                compileError={compileError}
                isCompiling={isCompiling}
              />
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
