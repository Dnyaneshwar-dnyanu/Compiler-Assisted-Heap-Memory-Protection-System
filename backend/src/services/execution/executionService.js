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
 * Note: External callers and HTTP clients CANNOT override Docker security settings.
 * Optional _internalTestOverrides is strictly for internal test harnesses (timeout / output limits).
 */
export async function executeJob(jobId, { abortSignal = null, _internalTestOverrides = null } = {}) {
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

  // 2. Locate and validate workspace containment & symlink safety
  const compilerCfg = getCompilerConfig();
  const workspaceRoot = compilerCfg.workspaceRoot;
  const workspaceDir = path.join(workspaceRoot, trimmedId);

  try {
    assertPathWithin(workspaceDir, workspaceRoot);
    if (fs.existsSync(workspaceDir)) {
      if (fs.lstatSync(workspaceDir).isSymbolicLink()) {
        return {
          success: false,
          errorCategory: 'PATH_TRAVERSAL_DETECTED',
          message: 'Symbolic links are not permitted for compilation workspaces.'
        };
      }
      const realWorkspace = fs.realpathSync(workspaceDir);
      const realRoot = fs.realpathSync(workspaceRoot);
      if (!realWorkspace.startsWith(realRoot + path.sep) && realWorkspace !== realRoot) {
        return {
          success: false,
          errorCategory: 'PATH_TRAVERSAL_DETECTED',
          message: 'Workspace directory escapes root containment boundary.'
        };
      }
    }
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

  // 3. Confirm compiled executable exists and is a valid non-symlink regular file
  const executablePath = path.join(workspaceDir, 'program.bin');
  if (!fs.existsSync(executablePath)) {
    return {
      success: false,
      errorCategory: 'EXECUTABLE_NOT_FOUND',
      message: `Compiled executable 'program.bin' for job '${trimmedId}' was not found. Please compile the program first.`
    };
  }

  const execLstat = fs.lstatSync(executablePath);
  if (execLstat.isSymbolicLink() || !execLstat.isFile()) {
    return {
      success: false,
      errorCategory: 'EXECUTABLE_NOT_FOUND',
      message: `Executable artifact for job '${trimmedId}' is a symbolic link or invalid file.`
    };
  }

  try {
    const realExec = fs.realpathSync(executablePath);
    const realWorkspace = fs.realpathSync(workspaceDir);
    if (!realExec.startsWith(realWorkspace + path.sep)) {
      return {
        success: false,
        errorCategory: 'PATH_TRAVERSAL_DETECTED',
        message: 'Executable artifact escapes workspace directory boundary.'
      };
    }
  } catch {
    return {
      success: false,
      errorCategory: 'EXECUTABLE_NOT_FOUND',
      message: 'Failed to resolve executable artifact path.'
    };
  }

  // 3b. Verify compilation manifest if present (validates executable belongs to successfully completed compilation)
  const manifestPath = path.join(workspaceDir, 'manifest.json');
  if (fs.existsSync(manifestPath)) {
    try {
      const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
      if (manifest.jobId !== trimmedId || manifest.status !== 'completed') {
        return {
          success: false,
          errorCategory: 'EXECUTABLE_NOT_FOUND',
          message: `Workspace metadata for job '${trimmedId}' indicates incomplete or failed compilation.`
        };
      }
    } catch {
      return {
        success: false,
        errorCategory: 'EXECUTABLE_NOT_FOUND',
        message: `Workspace manifest for job '${trimmedId}' is corrupted or unreadable.`
      };
    }
  }

  // Normalize host path for Docker bind mount
  const mountExecutablePath = executablePath.replace(/\\/g, '/');

  // 4. Run inside secure Docker sandbox (never on host)
  const runResult = await runContainer({
    executablePath: mountExecutablePath,
    _internalTestOverrides,
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
