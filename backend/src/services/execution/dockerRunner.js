import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import { getExecutionConfig } from './executionConfig.js';

let customSpawn = null;
let removeContainerHook = null;
let runnerTestOverrides = null;

/**
 * Non-negotiable, immutable Docker security restrictions.
 * These flags are strictly enforced and can NEVER be overridden by any caller.
 */
export const IMMUTABLE_SECURITY = Object.freeze({
  network: 'none',
  readOnly: true,
  capDrop: 'ALL',
  securityOpt: 'no-new-privileges',
  userUidGid: '10001:10001',
  memoryLimit: '64m',
  cpuLimit: '0.5',
  pidsLimit: 32,
  tmpfsMount: '/tmp:rw,noexec,nosuid,size=16m',
  init: true,
  rm: true
});

/**
 * For testing and mock injection.
 */
export function setCustomSpawn(fn) {
  customSpawn = fn;
}

export function setRemoveContainerHook(fn) {
  removeContainerHook = fn;
}

/**
 * Sets private internal test overrides for non-security settings (e.g. executionTimeoutMs, maxOutputBytes).
 * Security settings (network, privileges, user, caps, mounts, memory, cpu, pids) CANNOT be overridden.
 */
export function setRunnerTestOverrides(overrides) {
  runnerTestOverrides = overrides ? { ...overrides } : null;
}

export function resetRunnerTestOverrides() {
  runnerTestOverrides = null;
}

export function resetCustomSpawn() {
  customSpawn = null;
  removeContainerHook = null;
  runnerTestOverrides = null;
}

/**
 * Safely removes a container by name if it exists.
 */
export function removeContainer(containerName, dockerBin = 'docker') {
  if (removeContainerHook) {
    try { removeContainerHook(containerName); } catch {}
  }
  return new Promise((resolve) => {
    try {
      const killer = spawn(dockerBin, ['rm', '-f', containerName], {
        stdio: 'ignore'
      });
      killer.on('close', () => resolve());
      killer.on('error', () => resolve());
      setTimeout(() => {
        try { killer.kill('SIGKILL'); } catch {}
        resolve();
      }, 2000).unref();
    } catch {
      resolve();
    }
  });
}

/**
 * Runs the compiled binary in an isolated Docker container with strict security bounds.
 *
 * All security flags (network isolation, user privileges, resource ceilings,
 * dropped capabilities, read-only rootfs, read-only program mount, and noexec tmpfs)
 * are strictly immutable and derived exclusively from backend security configuration.
 *
 * Only non-security testing parameters (executionTimeoutMs, maxOutputBytes) may be
 * configured via internal test hooks or _internalTestOverrides.
 */
export async function runContainer({
  executablePath,
  _internalTestOverrides = null,
  customConfig = null,
  abortSignal = null
}) {
  const baseCfg = getExecutionConfig();

  // Combine private internal test overrides if provided (priority: parameter > module hook)
  const rawOverrides = _internalTestOverrides || customConfig || runnerTestOverrides || {};

  // Strictly filter overrides: only non-security execution parameters are allowed
  const safeOverrides = {};
  if (typeof rawOverrides.executionTimeoutMs === 'number' && rawOverrides.executionTimeoutMs > 0) {
    safeOverrides.executionTimeoutMs = rawOverrides.executionTimeoutMs;
  }
  if (typeof rawOverrides.maxOutputBytes === 'number' && rawOverrides.maxOutputBytes > 0) {
    safeOverrides.maxOutputBytes = rawOverrides.maxOutputBytes;
  }

  const executionTimeoutMs = safeOverrides.executionTimeoutMs ?? baseCfg.executionTimeoutMs;
  const maxOutputBytes = safeOverrides.maxOutputBytes ?? baseCfg.maxOutputBytes;
  const dockerBin = baseCfg.dockerBin;
  const imageName = baseCfg.imageName;

  const containerName = `mg-exec-${crypto.randomUUID()}`;
  const startTime = Date.now();

  const spawnFn = customSpawn || spawn;

  // Build explicit argument array for Docker using strictly immutable security flags.
  // Absolute host path must be mounted read-only to /app/program.bin
  const dockerArgs = [
    'run',
    '--name', containerName,
    '--rm',
    '--network', IMMUTABLE_SECURITY.network,
    '--read-only',
    '--cap-drop', IMMUTABLE_SECURITY.capDrop,
    '--security-opt', IMMUTABLE_SECURITY.securityOpt,
    '--user', IMMUTABLE_SECURITY.userUidGid,
    '--memory', IMMUTABLE_SECURITY.memoryLimit,
    '--cpus', String(IMMUTABLE_SECURITY.cpuLimit),
    '--pids-limit', String(IMMUTABLE_SECURITY.pidsLimit),
    '--init',
    '--tmpfs', IMMUTABLE_SECURITY.tmpfsMount,
    '-v', `${executablePath}:/app/program.bin:ro`,
    imageName
  ];

  let stdoutChunks = [];
  let stdoutBytes = 0;
  let stderrChunks = [];
  let stderrBytes = 0;
  let outputTruncated = false;
  let timedOut = false;
  let aborted = false;

  let child;
  try {
    child = spawnFn(dockerBin, dockerArgs, {
      stdio: ['ignore', 'pipe', 'pipe'],
      shell: false
    });
  } catch (err) {
    return {
      success: false,
      errorCategory: 'DOCKER_UNAVAILABLE',
      message: `Failed to spawn Docker process: ${err.message}`,
      exitCode: -1,
      signal: null,
      timedOut: false,
      durationMs: Date.now() - startTime,
      stdout: '',
      stderr: err.message,
      outputTruncated: false
    };
  }

  // Handle client disconnect via abortSignal
  const abortHandler = () => {
    aborted = true;
    try {
      child.kill('SIGKILL');
    } catch {}
    removeContainer(containerName, dockerBin);
  };

  if (abortSignal) {
    if (abortSignal.aborted) {
      abortHandler();
    } else {
      abortSignal.addEventListener('abort', abortHandler, { once: true });
    }
  }

  // Execution timeout handler
  let killTimer;
  const timeoutTimer = setTimeout(() => {
    timedOut = true;
    try {
      child.kill('SIGTERM');
    } catch {}

    // Escalate to docker kill to stop container inside daemon immediately
    try {
      const killer = spawn(dockerBin, ['kill', containerName], { stdio: 'ignore' });
      killer.unref();
    } catch {}

    killTimer = setTimeout(() => {
      try {
        child.kill('SIGKILL');
      } catch {}
      removeContainer(containerName, dockerBin);
    }, 1000);
    killTimer.unref();
  }, executionTimeoutMs);

  child.stdout?.on('data', (chunk) => {
    if (stdoutBytes + chunk.length <= maxOutputBytes) {
      stdoutChunks.push(chunk);
      stdoutBytes += chunk.length;
    } else if (stdoutBytes < maxOutputBytes) {
      const allowed = maxOutputBytes - stdoutBytes;
      stdoutChunks.push(chunk.subarray(0, allowed));
      stdoutBytes += allowed;
      outputTruncated = true;
    } else {
      outputTruncated = true;
    }
  });

  child.stderr?.on('data', (chunk) => {
    if (stderrBytes + chunk.length <= maxOutputBytes) {
      stderrChunks.push(chunk);
      stderrBytes += chunk.length;
    } else if (stderrBytes < maxOutputBytes) {
      const allowed = maxOutputBytes - stderrBytes;
      stderrChunks.push(chunk.subarray(0, allowed));
      stderrBytes += allowed;
      outputTruncated = true;
    } else {
      outputTruncated = true;
    }
  });

  return new Promise((resolve) => {
    child.on('error', (err) => {
      clearTimeout(timeoutTimer);
      clearTimeout(killTimer);
      if (abortSignal) {
        abortSignal.removeEventListener('abort', abortHandler);
      }
      removeContainer(containerName, dockerBin);

      resolve({
        success: false,
        errorCategory: 'DOCKER_UNAVAILABLE',
        message: `Docker execution error: ${err.message}`,
        exitCode: -1,
        signal: null,
        timedOut: false,
        durationMs: Date.now() - startTime,
        stdout: Buffer.concat(stdoutChunks).toString('utf8'),
        stderr: Buffer.concat(stderrChunks).toString('utf8') + `\n${err.message}`,
        outputTruncated
      });
    });

    child.on('close', async (code, signal) => {
      clearTimeout(timeoutTimer);
      clearTimeout(killTimer);
      if (abortSignal) {
        abortSignal.removeEventListener('abort', abortHandler);
      }

      const durationMs = Date.now() - startTime;
      const stdout = Buffer.concat(stdoutChunks).toString('utf8');
      const stderr = Buffer.concat(stderrChunks).toString('utf8');

      // Make sure container is completely cleaned up
      await removeContainer(containerName, dockerBin);

      // Check for image missing or daemon down errors in docker output
      let errorCategory = null;
      let message = null;

      if (stderr.includes('Unable to find image') || stderr.includes('pull access denied') || stderr.includes('repository does not exist')) {
        errorCategory = 'IMAGE_NOT_FOUND';
        message = `Execution container image '${imageName}' is not available locally.`;
      } else if (stderr.includes('Cannot connect to the Docker daemon') || stderr.includes('failed to connect to the docker API') || stderr.includes('daemon is not running')) {
        errorCategory = 'DOCKER_UNAVAILABLE';
        message = 'Docker daemon is not running or unreachable.';
      } else if (timedOut) {
        errorCategory = 'TIMEOUT_ERROR';
        message = `Execution timed out after ${executionTimeoutMs}ms.`;
      } else if (aborted) {
        errorCategory = 'ABORTED';
        message = 'Execution aborted by client disconnect.';
      }

      resolve({
        success: errorCategory === null,
        errorCategory,
        message,
        exitCode: code,
        signal,
        timedOut,
        aborted,
        durationMs,
        stdout,
        stderr,
        outputTruncated
      });
    });
  });
}
