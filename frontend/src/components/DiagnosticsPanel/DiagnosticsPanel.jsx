import React from 'react';
import { Terminal, AlertTriangle, Layers, Clock, CheckCircle2, XCircle, ShieldAlert } from 'lucide-react';
import { StageBadge } from '../StatusIndicator/StageBadge.jsx';
import { formatDuration, formatBytes } from '../../utils/formatters.js';

export function DiagnosticsPanel({ compileResult, compileError, isCompiling }) {
  if (isCompiling) {
    return (
      <div className="w-full h-full flex flex-col items-center justify-center p-8 text-center text-zinc-500">
        <div className="w-6 h-6 border-2 border-indigo-500 border-t-transparent rounded-full animate-spin mb-3" />
        <p className="text-xs font-medium text-zinc-300">Compiling and Instrumenting...</p>
        <p className="text-[11px] text-zinc-500 mt-1">
          Generating LLVM IR, running MemoryGuard pass, and linking runtime.
        </p>
      </div>
    );
  }

  const stages = compileResult?.stages || compileError?.stages || [];
  const rawDiagnostics = compileError?.diagnostics || compileResult?.diagnostics?.raw || '';
  const totalDuration = compileResult?.metadata?.totalDurationMs;
  const sourceBytes = compileResult?.metadata?.sourceBytes;
  const isFailed = Boolean(compileError);

  if (!compileResult && !compileError) {
    return (
      <div className="w-full h-full flex flex-col items-center justify-center p-8 text-center text-zinc-500">
        <Terminal className="w-8 h-8 text-zinc-600 mb-2 stroke-[1.5]" />
        <p className="text-xs font-medium text-zinc-400">No Compilation Performed Yet</p>
        <p className="text-[11px] text-zinc-600 mt-1 max-w-sm">
          Click "Compile &amp; Analyze" to build your C/C++ source code with MemoryGuard instrumentation.
        </p>
      </div>
    );
  }

  return (
    <div className="w-full h-full overflow-y-auto p-4 space-y-4">
      {/* Failure Banner if any */}
      {isFailed && (
        <div className="p-3.5 rounded border border-rose-500/30 bg-rose-500/10 text-rose-300 flex items-start gap-3">
          <AlertTriangle className="w-4 h-4 text-rose-400 mt-0.5 shrink-0" />
          <div className="text-xs space-y-1">
            <div className="font-semibold text-rose-200">
              Compilation Failed ({compileError.code || 'COMPILATION_ERROR'})
            </div>
            <p className="text-rose-300/90 leading-relaxed">
              {compileError.message}
            </p>
            {compileError.stage && (
              <span className="inline-block mt-1 font-mono text-[10px] px-1.5 py-0.5 rounded bg-rose-950/80 border border-rose-800 text-rose-300">
                Failed at stage: {compileError.stage}
              </span>
            )}
          </div>
        </div>
      )}

      {/* Compile-Time Static Memory Violation Cards */}
      {isFailed && compileError?.staticViolations && compileError.staticViolations.length > 0 && (
        <div className="space-y-3" data-testid="static-violations-container">
          <div className="text-[11px] font-semibold text-rose-400 uppercase tracking-wider flex items-center gap-1.5">
            <ShieldAlert className="w-3.5 h-3.5 text-rose-400" />
            Statically Proven Heap Violations ({compileError.staticViolations.length})
          </div>
          {compileError.staticViolations.map((v, i) => (
            <div
              key={i}
              data-testid="static-violation-card"
              className="p-3.5 rounded border border-rose-500/50 bg-rose-950/30 text-rose-200 space-y-2.5"
            >
              <div className="flex items-center justify-between border-b border-rose-800/60 pb-2">
                <div className="flex items-center gap-2">
                  <ShieldAlert className="w-4 h-4 text-rose-400 shrink-0" />
                  <span className="font-bold text-xs text-rose-200 uppercase tracking-wide">
                    Compile-Time Heap Violation: {v.accessType === 'write' ? 'Out-of-Bounds Write' : 'Out-of-Bounds Read'}
                  </span>
                </div>
                <span className="px-1.5 py-0.5 rounded text-[10px] font-mono bg-rose-900/60 text-rose-300 border border-rose-700">
                  {v.errorType}
                </span>
              </div>

              <p className="text-xs text-rose-200/90 leading-relaxed font-medium">
                {v.message}
              </p>

              {/* Memory Information Grid */}
              <div className="grid grid-cols-2 sm:grid-cols-4 gap-2 pt-1 font-mono text-[11px]">
                <div className="bg-zinc-900/80 p-2 rounded border border-zinc-800">
                  <span className="text-zinc-500 block text-[10px] uppercase">Location</span>
                  <span className="text-zinc-200 font-semibold">{v.file}:{v.line}{v.column ? `:${v.column}` : ''}</span>
                </div>
                <div className="bg-zinc-900/80 p-2 rounded border border-zinc-800">
                  <span className="text-zinc-500 block text-[10px] uppercase">Access Type</span>
                  <span className="text-zinc-200 font-semibold">{v.accessType.toUpperCase()} ({v.accessSize} bytes)</span>
                </div>
                <div className="bg-zinc-900/80 p-2 rounded border border-zinc-800">
                  <span className="text-zinc-500 block text-[10px] uppercase">Offset</span>
                  <span className="text-rose-400 font-semibold">{v.offset} bytes</span>
                </div>
                <div className="bg-zinc-900/80 p-2 rounded border border-zinc-800">
                  <span className="text-zinc-500 block text-[10px] uppercase">Allocation Size</span>
                  <span className="text-emerald-400 font-semibold">{v.allocationSize} bytes</span>
                </div>
              </div>

              {v.statement && (
                <div className="mt-1 bg-zinc-950 p-2.5 rounded border border-rose-900/60 font-mono text-xs flex items-center justify-between">
                  <span className="text-zinc-400 text-[11px]">Offending Statement (Line {v.line}):</span>
                  <code className="text-rose-300 font-bold bg-rose-950/80 px-2 py-0.5 rounded border border-rose-800 text-xs">
                    {v.statement}
                  </code>
                </div>
              )}
            </div>
          ))}
        </div>
      )}

      {/* Success Banner if successful */}
      {!isFailed && compileResult?.status === 'completed' && (
        <div className="p-3 rounded border border-emerald-500/20 bg-emerald-500/5 text-emerald-300 flex items-center justify-between text-xs">
          <div className="flex items-center gap-2">
            <CheckCircle2 className="w-4 h-4 text-emerald-400 shrink-0" />
            <span className="font-medium text-emerald-200">
              Compilation, Instrumentation, and Linking Succeeded
            </span>
          </div>
          {compileResult?.jobId && (
            <span className="font-mono text-[10px] text-zinc-400 bg-zinc-900 px-2 py-0.5 rounded border border-zinc-800" title="Workspace Job UUID">
              Job: {compileResult.jobId.slice(0, 8)}...
            </span>
          )}
        </div>
      )}

      {/* Compilation Pipeline Stages */}
      {stages.length > 0 && (
        <div className="space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-[11px] font-semibold text-zinc-400 uppercase tracking-wider flex items-center gap-1.5">
              <Layers className="w-3 h-3 text-zinc-500" />
              Compiler Pipeline Stages
            </span>
            {totalDuration !== undefined && (
              <span className="text-[11px] font-mono text-zinc-500 flex items-center gap-1">
                <Clock className="w-3 h-3" />
                Total: {formatDuration(totalDuration)}
              </span>
            )}
          </div>

          <div className="flex flex-wrap gap-2">
            {stages.map((stg, i) => (
              <StageBadge
                key={i}
                name={stg.name}
                status={stg.status}
                durationMs={stg.durationMs}
              />
            ))}
          </div>
        </div>
      )}

      {/* Compiler Output & Diagnostics Terminal */}
      <div className="space-y-1.5">
        <span className="text-[11px] font-semibold text-zinc-400 uppercase tracking-wider flex items-center gap-1.5">
          <Terminal className="w-3 h-3 text-zinc-500" />
          Toolchain Diagnostics &amp; Build Logs
        </span>

        <div className="rounded border border-zinc-800 bg-zinc-950 p-3 font-mono text-xs leading-relaxed overflow-x-auto min-h-[140px] max-h-[300px]">
          {rawDiagnostics ? (
            <pre className={`whitespace-pre-wrap ${isFailed ? 'text-rose-400' : 'text-zinc-300'}`}>
              {rawDiagnostics}
            </pre>
          ) : (
            <div className="text-zinc-600 italic">
              No warnings or diagnostic messages reported by Clang or the MemoryGuard pass.
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
