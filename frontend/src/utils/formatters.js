/**
 * Utilities for formatting durations, memory sizes, hex addresses, and status badges.
 */

export function formatDuration(ms) {
  if (ms === null || ms === undefined) return 'N/A';
  if (ms < 1000) return `${ms}ms`;
  return `${(ms / 1000).toFixed(2)}s`;
}

export function formatBytes(bytes) {
  if (bytes === null || bytes === undefined) return 'N/A';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export function formatPointer(ptr) {
  if (!ptr) return 'N/A';
  return String(ptr).toLowerCase();
}

export function getExecutionStatusConfig(status) {
  switch (status) {
    case 'no_violations_detected':
      return {
        label: 'No Violations Detected',
        sublabel: 'Dynamic checks passed on executed paths',
        color: 'emerald',
        bg: 'bg-emerald-500/10',
        text: 'text-emerald-400',
        border: 'border-emerald-500/30'
      };
    case 'violation_detected':
      return {
        label: 'MemoryGuard Violation Detected',
        sublabel: 'Safety assertion failed and execution was aborted',
        color: 'rose',
        bg: 'bg-rose-500/10',
        text: 'text-rose-400',
        border: 'border-rose-500/30'
      };
    case 'execution_failed':
      return {
        label: 'Execution Failed',
        sublabel: 'Abnormal termination, crash, or timeout',
        color: 'amber',
        bg: 'bg-amber-500/10',
        text: 'text-amber-400',
        border: 'border-amber-500/30'
      };
    case 'output_truncated':
      return {
        label: 'Output Truncated',
        sublabel: 'Execution generated output exceeding limits',
        color: 'amber',
        bg: 'bg-amber-500/10',
        text: 'text-amber-400',
        border: 'border-amber-500/30'
      };
    case 'unavailable':
    default:
      return {
        label: 'Unavailable',
        sublabel: 'Execution environment error or unreached state',
        color: 'zinc',
        bg: 'bg-zinc-500/10',
        text: 'text-zinc-400',
        border: 'border-zinc-500/30'
      };
  }
}
