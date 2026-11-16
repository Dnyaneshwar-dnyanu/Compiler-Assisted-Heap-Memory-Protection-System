import React from 'react';
import { Play, PlayCircle, RotateCcw, Cpu, Code2, Sparkles, Loader2 } from 'lucide-react';
import { SAMPLE_CODES } from '../Editor/sampleCodes.js';

export function Toolbar({
  language,
  onLanguageChange,
  onSelectSample,
  onResetCode,
  onCompile,
  onRun,
  isCompiling,
  isRunning,
  canRun,
  hasJob
}) {
  const samples = SAMPLE_CODES[language] || [];

  return (
    <div className="border-b border-zinc-800 bg-zinc-900/40 px-4 py-2 flex flex-wrap items-center justify-between gap-3 shrink-0">
      {/* Left controls: Language & Sample selectors */}
      <div className="flex items-center gap-2">
        {/* Language selector */}
        <div className="flex items-center gap-1.5 bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300">
          <Code2 className="w-3.5 h-3.5 text-zinc-400" />
          <span className="text-zinc-500 font-mono text-[11px]">Lang:</span>
          <select
            aria-label="Select Programming Language"
            data-testid="language-select"
            value={language}
            onChange={(e) => onLanguageChange(e.target.value)}
            disabled={isCompiling || isRunning}
            className="bg-transparent text-zinc-100 font-medium focus:outline-none cursor-pointer disabled:cursor-not-allowed"
          >
            <option value="c" className="bg-zinc-900 text-zinc-100">C (C11)</option>
            <option value="cpp" className="bg-zinc-900 text-zinc-100">C++ (C++17)</option>
          </select>
        </div>

        {/* Sample program dropdown */}
        <div className="hidden sm:flex items-center gap-1.5 bg-zinc-900 border border-zinc-800 rounded px-2 py-1 text-xs text-zinc-300">
          <Sparkles className="w-3.5 h-3.5 text-indigo-400" />
          <span className="text-zinc-500 font-mono text-[11px]">Sample:</span>
          <select
            aria-label="Select Sample Code"
            data-testid="sample-select"
            defaultValue=""
            onChange={(e) => {
              if (e.target.value) {
                onSelectSample(e.target.value);
                e.target.value = '';
              }
            }}
            disabled={isCompiling || isRunning}
            className="bg-transparent text-zinc-100 font-medium focus:outline-none cursor-pointer disabled:cursor-not-allowed max-w-[170px] truncate"
          >
            <option value="" disabled className="bg-zinc-900 text-zinc-500">Choose sample...</option>
            {samples.map((sample) => (
              <option key={sample.id} value={sample.id} className="bg-zinc-900 text-zinc-100">
                {sample.title}
              </option>
            ))}
          </select>
        </div>

        {/* Reset button */}
        <button
          onClick={onResetCode}
          disabled={isCompiling || isRunning}
          title="Reset code to default template"
          className="flex items-center gap-1 px-2.5 py-1 text-xs text-zinc-400 hover:text-zinc-200 bg-zinc-900/60 hover:bg-zinc-800 border border-zinc-800 rounded transition-colors disabled:opacity-50"
        >
          <RotateCcw className="w-3 h-3" />
          <span className="hidden sm:inline">Reset</span>
        </button>
      </div>

      {/* Right controls: Action buttons */}
      <div className="flex items-center gap-2">
        {/* Compile & Analyze Button */}
        <button
          onClick={onCompile}
          disabled={isCompiling || isRunning}
          className="flex items-center gap-2 px-3.5 py-1.5 rounded text-xs font-semibold bg-indigo-600 hover:bg-indigo-500 text-white shadow-sm shadow-indigo-900/40 border border-indigo-500/50 transition-all disabled:opacity-50 disabled:cursor-not-allowed active:scale-[0.98]"
        >
          {isCompiling ? (
            <>
              <Loader2 className="w-3.5 h-3.5 animate-spin" />
              <span>Compiling &amp; Analyzing...</span>
            </>
          ) : (
            <>
              <Cpu className="w-3.5 h-3.5" />
              <span>Compile &amp; Analyze</span>
            </>
          )}
        </button>

        {/* Run Button */}
        <button
          onClick={onRun}
          disabled={!canRun || isCompiling || isRunning}
          title={
            !hasJob
              ? 'Compile your program first to generate an executable'
              : !canRun
              ? 'Executable unavailable or compilation failed'
              : 'Run binary inside isolated Docker container'
          }
          className={`flex items-center gap-2 px-3.5 py-1.5 rounded text-xs font-semibold transition-all active:scale-[0.98] ${
            canRun && !isRunning && !isCompiling
              ? 'bg-emerald-600 hover:bg-emerald-500 text-white shadow-sm shadow-emerald-900/40 border border-emerald-500/50 cursor-pointer'
              : 'bg-zinc-800 text-zinc-500 border border-zinc-700/50 cursor-not-allowed opacity-60'
          }`}
        >
          {isRunning ? (
            <>
              <Loader2 className="w-3.5 h-3.5 animate-spin text-emerald-300" />
              <span>Running in Docker...</span>
            </>
          ) : (
            <>
              <Play className="w-3.5 h-3.5 fill-current" />
              <span>Run</span>
            </>
          )}
        </button>
      </div>
    </div>
  );
}
