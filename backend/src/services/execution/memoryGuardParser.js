/**
 * MemoryGuard Runtime Diagnostic Parser
 * Inspects execution streams (stdout/stderr) and exit codes to extract structured violation diagnostics.
 */

const VIOLATION_REGEX = /MemoryGuard:\s*VIOLATION DETECTED!\s*Invalid access to ptr=(0x[0-9a-fA-F]+),\s*access_size=(\d+)\s*bytes/;

export function parseExecutionOutput({
  exitCode = 0,
  signal = null,
  stdout = '',
  stderr = '',
  durationMs = 0,
  timedOut = false,
  outputTruncated = false
} = {}) {
  const match = (stderr || '').match(VIOLATION_REGEX);

  let violation = null;
  if (match) {
    violation = {
      detected: true,
      violatingPointer: match[1],
      accessSizeBytes: parseInt(match[2], 10),
      rawMessage: match[0]
    };
  }

  let status;
  if (outputTruncated) {
    status = 'output_truncated';
  } else if (timedOut) {
    status = 'execution_failed';
  } else if (violation) {
    status = 'violation_detected';
  } else if (exitCode === 0) {
    // Zero exit code and no violation
    status = 'no_violations_detected';
  } else if (exitCode !== null && exitCode !== undefined) {
    // Nonzero exit code
    if (signal === 'SIGABRT' || signal === 'SIGSEGV' || exitCode === 134 || exitCode === 139) {
      status = 'execution_failed';
    } else {
      // Regular non-zero program exit (e.g. exit(1), return -1)
      status = 'no_violations_detected';
    }
  } else {
    status = 'unavailable';
  }

  return {
    status,
    exitCode: exitCode ?? null,
    terminationSignal: signal ?? null,
    timedOut: Boolean(timedOut),
    outputTruncated: Boolean(outputTruncated),
    durationMs,
    violation,
    stdout,
    stderr
  };
}
