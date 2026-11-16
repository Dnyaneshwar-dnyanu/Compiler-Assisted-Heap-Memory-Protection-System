import React, { useState } from 'react';
import { Terminal, ShieldAlert, ShieldCheck, AlertCircle, Clock, Hash, Activity, Shield, Box } from 'lucide-react';
import { getExecutionStatusConfig, formatDuration, formatPointer } from '../../utils/formatters.js';

export function OutputPanel({ executionResult, executionError, isRunning }) {
  const [activeStream, setActiveStream] = useState('all'); // 'all', 'stdout', 'stderr'

  if (isRunning) {
    return (
      <div className="w-full h-full flex flex-col items-center justify-center p-8 text-center text-zinc-500">
        <div className="w-6 h-6 border-2 border-emerald-500 border-t-transparent rounded-full animate-spin mb-3" />
        <p className="text-xs font-medium text-zinc-300">Executing Inside Docker Sandbox...</p>
        <p className="text-[11px] text-zinc-500 mt-1 max-w-xs">
          Running binary with unprivileged user, restricted CPU/memory, read-only filesystem, and no network.
        </p>
      </div>
    );
  }

  if (executionError) {
    return (
      <div className="w-full h-full overflow-y-auto p-4 space-y-4">
        <div className="p-4 rounded border border-rose-500/30 bg-rose-500/10 text-rose-300">
          <div className="flex items-center gap-2 font-semibold text-rose-200 text-xs mb-1">
            <AlertCircle className="w-4 h-4 text-rose-400" />
            Execution Error ({executionError.code || 'EXECUTION_FAILED'})
          </div>
          <p className="text-xs text-rose-300/90 leading-relaxed mb-2">
            {executionError.message}
          </p>
          {executionError.code === 'DOCKER_UNAVAILABLE' && (
            <div className="text-[11px] text-zinc-400 bg-zinc-950 p-2.5 rounded border border-zinc-800 font-mono">
              Note: Binaries are never executed directly on the host machine. Ensure Docker daemon is running and runner image 'memoryguard-runner:latest' is built.
            </div>
          )}
          {executionError.code === 'TIMEOUT_ERROR' && (
            <div className="text-[11px] text-zinc-400 bg-zinc-950 p-2.5 rounded border border-zinc-800 font-mono">
              Program exceeded maximum execution timeout limit (default 5000ms) and was terminated inside container.
            </div>
          )}
        </div>
      </div>
    );
  }

  if (!executionResult) {
    return (
      <div className="w-full h-full flex flex-col items-center justify-center p-8 text-center text-zinc-500">
        <Terminal className="w-8 h-8 text-zinc-600 mb-2 stroke-[1.5]" />
        <p className="text-xs font-medium text-zinc-400">Program Not Executed Yet</p>
        <p className="text-[11px] text-zinc-600 mt-1 max-w-sm">
          Compile your source code successfully, then click "Run" to launch the binary in the secure container.
        </p>
      </div>
    );
  }

  const { status, execution, diagnostics } = executionResult;
  const statusCfg = getExecutionStatusConfig(status);
  const violation = diagnostics?.violation;
  const exitCode = execution?.exitCode;
  const signal = execution?.terminationSignal;
  const durationMs = execution?.durationMs;
  const stdout = execution?.stdout || '';
  const stderr = execution?.stderr || '';

  return (
    <div className="w-full h-full overflow-y-auto p-4 space-y-4">
      {/* Status Header Banner */}
      <div className={`p-3.5 rounded border ${statusCfg.bg} ${statusCfg.border} flex flex-col sm:flex-row sm:items-center justify-between gap-3`}>
        <div className="flex items-start gap-2.5">
          {status === 'violation_detected' ? (
            <ShieldAlert className="w-5 h-5 text-rose-400 shrink-0 mt-0.5" />
          ) : status === 'no_violations_detected' ? (
            <ShieldCheck className="w-5 h-5 text-emerald-400 shrink-0 mt-0.5" />
          ) : (
            <AlertCircle className="w-5 h-5 text-amber-400 shrink-0 mt-0.5" />
          )}
          <div>
            <div className={`font-semibold text-xs ${statusCfg.text}`}>
              {statusCfg.label}
            </div>
            <div className="text-[11px] text-zinc-400 mt-0.5">
              {statusCfg.sublabel}
            </div>
          </div>
        </div>

        {/* Execution Metadata Pills */}
        <div className="flex flex-wrap items-center gap-2 font-mono text-[11px]">
          <span className="px-2 py-0.5 rounded bg-zinc-900 border border-zinc-800 text-zinc-300 flex items-center gap-1" title="Container Process Exit Code">
            <Hash className="w-3 h-3 text-zinc-500" />
            Exit: <span className={exitCode === 0 ? 'text-emerald-400' : 'text-amber-400'}>{exitCode ?? 'N/A'}</span>
          </span>

          {signal && (
            <span className="px-2 py-0.5 rounded bg-rose-950/50 border border-rose-800/60 text-rose-300">
              Signal: {signal}
            </span>
          )}

          <span className="px-2 py-0.5 rounded bg-zinc-900 border border-zinc-800 text-zinc-300 flex items-center gap-1" title="Execution Duration">
            <Clock className="w-3 h-3 text-zinc-500" />
            {formatDuration(durationMs)}
          </span>

          <span className="px-2 py-0.5 rounded bg-zinc-900 border border-zinc-800 text-zinc-400 flex items-center gap-1" title="Isolated Docker Container">
            <Box className="w-3 h-3 text-zinc-500" />
            Docker
          </span>
        </div>
      </div>

      {/* Prominent Violation Card (if violation detected) */}
      {violation?.detected && (
        <div className="p-3.5 rounded border border-rose-500/40 bg-rose-950/20 space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold uppercase tracking-wider text-rose-300 flex items-center gap-1.5">
              <ShieldAlert className="w-4 h-4 text-rose-400" />
              Runtime Violation Detail
            </span>
            <span className="text-[10px] px-2 py-0.5 rounded bg-rose-900/60 text-rose-200 border border-rose-700/60 font-mono">
              SIGABRT Triggered
            </span>
          </div>

          <div className="grid grid-cols-1 sm:grid-cols-2 gap-2 text-xs font-mono">
            <div className="p-2 rounded bg-zinc-950 border border-zinc-800 flex items-center justify-between">
              <span className="text-zinc-500">Offending Pointer:</span>
              <span className="text-rose-400 font-bold">{formatPointer(violation.violatingPointer)}</span>
            </div>
            <div className="p-2 rounded bg-zinc-950 border border-zinc-800 flex items-center justify-between">
              <span className="text-zinc-500">Access Size:</span>
              <span className="text-zinc-200 font-bold">{violation.accessSizeBytes} bytes</span>
            </div>
          </div>

          {violation.rawMessage && (
            <div className="p-2 rounded bg-zinc-950 border border-zinc-800 text-[11px] font-mono text-rose-300 break-all">
              {violation.rawMessage}
            </div>
          )}
        </div>
      )}

      {/* Safety Notice when no violations detected */}
      {status === 'no_violations_detected' && (
        <div className="p-2.5 rounded border border-zinc-800 bg-zinc-900/40 text-[11px] text-zinc-400 leading-relaxed flex items-center gap-2">
          <Shield className="w-4 h-4 text-emerald-400 shrink-0" />
          <span>
            <strong>Dynamic Heap Verification:</strong> No spatial or temporal boundary violations occurred along executed instructions. (Note: Dynamic analysis evaluates only traversed control paths).
          </span>
        </div>
      )}

      {/* Terminal Output Viewer with Stream Tabs */}
      <div className="space-y-1.5">
        <div className="flex items-center justify-between">
          <span className="text-[11px] font-semibold text-zinc-400 uppercase tracking-wider flex items-center gap-1.5">
            <Terminal className="w-3 h-3 text-zinc-500" />
            Container Console Output
          </span>

          {/* Stream selector */}
          <div className="flex items-center gap-1 bg-zinc-900 border border-zinc-800 rounded p-0.5 text-[10px] font-mono">
            <button
              onClick={() => setActiveStream('all')}
              className={`px-2 py-0.5 rounded transition-colors ${
                activeStream === 'all' ? 'bg-zinc-800 text-zinc-100' : 'text-zinc-400 hover:text-zinc-200'
              }`}
            >
              Combined
            </button>
            <button
              onClick={() => setActiveStream('stdout')}
              className={`px-2 py-0.5 rounded transition-colors ${
                activeStream === 'stdout' ? 'bg-zinc-800 text-zinc-100' : 'text-zinc-400 hover:text-zinc-200'
              }`}
            >
              Stdout {stdout ? `(${stdout.length}B)` : ''}
            </button>
            <button
              onClick={() => setActiveStream('stderr')}
              className={`px-2 py-0.5 rounded transition-colors ${
                activeStream === 'stderr' ? 'bg-zinc-800 text-zinc-100' : 'text-zinc-400 hover:text-zinc-200'
              }`}
            >
              Stderr {stderr ? `(${stderr.length}B)` : ''}
            </button>
          </div>
        </div>

        {/* Terminal Screen */}
        <div className="rounded border border-zinc-800 bg-zinc-950 p-3 font-mono text-xs leading-relaxed min-h-[160px] max-h-[360px] overflow-x-auto space-y-2">
          {activeStream === 'all' && (
            <>
              {stdout && (
                <div>
                  <div className="text-[10px] uppercase font-bold text-zinc-500 mb-0.5">--- Standard Output ---</div>
                  <pre className="text-zinc-200 whitespace-pre-wrap">{stdout}</pre>
                </div>
              )}
              {stderr && (
                <div>
                  <div className="text-[10px] uppercase font-bold text-rose-400/80 mb-0.5">--- Standard Error ---</div>
                  <pre className="text-rose-400 whitespace-pre-wrap">{stderr}</pre>
                </div>
              )}
              {!stdout && !stderr && (
                <div className="text-zinc-600 italic">Program produced no output to stdout or stderr.</div>
              )}
            </>
          )}

          {activeStream === 'stdout' && (
            <pre className="text-zinc-200 whitespace-pre-wrap">
              {stdout || <span className="text-zinc-600 italic">Stdout stream was empty.</span>}
            </pre>
          )}

          {activeStream === 'stderr' && (
            <pre className="text-rose-400 whitespace-pre-wrap">
              {stderr || <span className="text-zinc-600 italic">Stderr stream was empty.</span>}
            </pre>
          )}
        </div>
      </div>
    </div>
  );
}
