import fs from 'node:fs';
import path from 'node:path';
import { getCompilerConfig } from '../compiler/compilerConfig.js';
import { runContainer } from './dockerRunner.js';
import { parseExecutionOutput } from './memoryGuardParser.js';

const UUID_REGEX = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/;

/**
 * Asserts that a target path is strictly inside the allowed root directory.
 */
function assertPathWithin(targetPath, allowedRoot) {
  const resolvedTarget = path.resolve(targetPath);
  const resolvedRoot = path.resolve(allowedRoot);

  if (!resolvedTarget.startsWith(resolvedRoot + path.sep) && resolvedTarget !== resolvedRoot) {
    throw new Error('Access denied: path traversal detected.');
  }
}

/**
 * Coordinates secure Docker execution for a compiled job.
 */
export async function executeJob(jobId, { customConfig = {}, abortSignal = null } = {}) {
  // 1. Validate Job ID format
  if (!jobId || typeof jobId !== 'string') {
    return {
      success: false,
      errorCategory: 'INVALID_JOB_ID',
      message: 'Job ID is required and must be a valid UUID string.'
    };
  }

  const trimmedId = jobId.trim();
  if (!UUID_REGEX.test(trimmedId)) {
    return {
      success: false,
      errorCategory: 'INVALID_JOB_ID',
      message: `Invalid Job ID format: '${jobId}'. Expected standard UUID v4.`
    };
  }

  // 2. Locate and validate workspace containment
  const compilerCfg = getCompilerConfig();
  const workspaceRoot = compilerCfg.workspaceRoot;
  const workspaceDir = path.join(workspaceRoot, trimmedId);

  try {
    assertPathWithin(workspaceDir, workspaceRoot);
  } catch {
    return {
      success: false,
      errorCategory: 'PATH_TRAVERSAL_DETECTED',
      message: 'Path traversal attempt detected.'
    };
  }

  if (!fs.existsSync(workspaceDir) || !fs.statSync(workspaceDir).isDirectory()) {
    return {
      success: false,
      errorCategory: 'JOB_NOT_FOUND',
      message: `Compilation workspace for job '${trimmedId}' does not exist.`
    };
  }

  // 3. Confirm compiled executable exists
  const executablePath = path.join(workspaceDir, 'program.bin');
  if (!fs.existsSync(executablePath) || !fs.statSync(executablePath).isFile()) {
    return {
      success: false,
      errorCategory: 'EXECUTABLE_NOT_FOUND',
      message: `Compiled executable 'program.bin' for job '${trimmedId}' was not found. Please compile the program first.`
    };
  }

  // Normalize host path for Docker bind mount
  const mountExecutablePath = executablePath.replace(/\\/g, '/');

  // 4. Run inside secure Docker sandbox (never on host)
  const runResult = await runContainer({
    executablePath: mountExecutablePath,
    customConfig,
    abortSignal
  });

  // Handle fatal Docker orchestration errors (daemon down, image missing, timeout)
  if (!runResult.success) {
    return {
      success: false,
      jobId: trimmedId,
      errorCategory: runResult.errorCategory || 'EXECUTION_FAILED',
      message: runResult.message || 'Execution container failed to start.',
      details: {
        timedOut: runResult.timedOut || false,
        outputTruncated: runResult.outputTruncated || false
      }
    };
  }

  // 5. Parse MemoryGuard diagnostics from runtime output
  const parsed = parseExecutionOutput({
    exitCode: runResult.exitCode,
    signal: runResult.signal,
    stdout: runResult.stdout,
    stderr: runResult.stderr,
    durationMs: runResult.durationMs,
    timedOut: runResult.timedOut,
    outputTruncated: runResult.outputTruncated
  });

  return {
    success: true,
    jobId: trimmedId,
    status: parsed.status,
    execution: {
      exitCode: parsed.exitCode,
      terminationSignal: parsed.terminationSignal,
      durationMs: parsed.durationMs,
      timedOut: parsed.timedOut,
      outputTruncated: parsed.outputTruncated,
      stdout: parsed.stdout,
      stderr: parsed.stderr
    },
    diagnostics: {
      violation: parsed.violation
    }
  };
}
