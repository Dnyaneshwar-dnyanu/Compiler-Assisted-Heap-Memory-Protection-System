import React from 'react';
import { CheckCircle2, XCircle, Loader2, MinusCircle } from 'lucide-react';
import { formatDuration } from '../../utils/formatters.js';

const STAGE_LABELS = {
  FRONTEND_COMPILATION: 'Clang Frontend',
  INSTRUMENTATION: 'MemoryGuard Pass',
  LINKING: 'Runtime Linker'
};

export function StageBadge({ name, status, durationMs }) {
  const label = STAGE_LABELS[name] || name;

  const isCompleted = status === 'completed';
  const isFailed = status === 'failed';
  const isRunning = status === 'running';

  return (
    <div
      className={`inline-flex items-center gap-2 px-2.5 py-1 rounded border text-xs font-mono transition-colors ${
        isCompleted
          ? 'bg-emerald-500/10 border-emerald-500/20 text-emerald-400'
          : isFailed
          ? 'bg-rose-500/10 border-rose-500/20 text-rose-400'
          : isRunning
          ? 'bg-indigo-500/10 border-indigo-500/20 text-indigo-400 animate-pulse'
          : 'bg-zinc-900 border-zinc-800 text-zinc-500'
      }`}
    >
      {isCompleted && <CheckCircle2 className="w-3.5 h-3.5 text-emerald-400 shrink-0" />}
      {isFailed && <XCircle className="w-3.5 h-3.5 text-rose-400 shrink-0" />}
      {isRunning && <Loader2 className="w-3.5 h-3.5 text-indigo-400 animate-spin shrink-0" />}
      {!isCompleted && !isFailed && !isRunning && <MinusCircle className="w-3.5 h-3.5 text-zinc-600 shrink-0" />}

      <span className="font-medium text-zinc-200">{label}</span>

      {durationMs !== undefined && durationMs !== null && (
        <span className="text-[10px] text-zinc-400 ml-1">
          {formatDuration(durationMs)}
        </span>
      )}
    </div>
  );
}
