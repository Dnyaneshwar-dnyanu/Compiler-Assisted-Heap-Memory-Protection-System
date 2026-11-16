import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import { getExecutionConfig } from './executionConfig.js';

let customSpawn = null;

/**
 * For testing and mock injection.
 */
export function setCustomSpawn(fn) {
  customSpawn = fn;
}

export function resetCustomSpawn() {
  customSpawn = null;
}

/**
 * Safely removes a container by name if it exists.
 */
export function removeContainer(containerName, dockerBin = 'docker') {
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
 */
export async function runContainer({
  executablePath,
  customConfig = {},
  abortSignal = null
}) {
  const cfg = { ...getExecutionConfig(), ...customConfig };
  const containerName = `mg-exec-${crypto.randomUUID()}`;
  const startTime = Date.now();

  const spawnFn = customSpawn || spawn;

  // Build explicit argument array for Docker
  // Absolute host path must be mounted read-only to /app/program.bin
  const dockerArgs = [
    'run',
    '--name', containerName,
    '--rm',
    '--network', cfg.network,
    '--read-only',
    '--cap-drop', cfg.capDrop,
    '--security-opt', cfg.securityOpt,
    '--user', cfg.userUidGid,
    '--memory', cfg.memoryLimit,
    '--cpus', String(cfg.cpuLimit),
    '--pids-limit', String(cfg.pidsLimit),
    '--init',
    '--tmpfs', cfg.tmpfsMount,
    '-v', `${executablePath}:/app/program.bin:ro`,
    cfg.imageName
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
    child = spawnFn(cfg.dockerBin, dockerArgs, {
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
    removeContainer(containerName, cfg.dockerBin);
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
      const killer = spawn(cfg.dockerBin, ['kill', containerName], { stdio: 'ignore' });
      killer.unref();
    } catch {}

    killTimer = setTimeout(() => {
      try {
        child.kill('SIGKILL');
      } catch {}
      removeContainer(containerName, cfg.dockerBin);
    }, 1000);
    killTimer.unref();
  }, cfg.executionTimeoutMs);

  child.stdout?.on('data', (chunk) => {
    if (stdoutBytes + chunk.length <= cfg.maxOutputBytes) {
      stdoutChunks.push(chunk);
      stdoutBytes += chunk.length;
    } else if (stdoutBytes < cfg.maxOutputBytes) {
      const allowed = cfg.maxOutputBytes - stdoutBytes;
      stdoutChunks.push(chunk.subarray(0, allowed));
      stdoutBytes += allowed;
      outputTruncated = true;
    } else {
      outputTruncated = true;
    }
  });

  child.stderr?.on('data', (chunk) => {
    if (stderrBytes + chunk.length <= cfg.maxOutputBytes) {
      stderrChunks.push(chunk);
      stderrBytes += chunk.length;
    } else if (stderrBytes < cfg.maxOutputBytes) {
      const allowed = cfg.maxOutputBytes - stderrBytes;
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
      removeContainer(containerName, cfg.dockerBin);

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
      await removeContainer(containerName, cfg.dockerBin);

      // Check for image missing or daemon down errors in docker output
      let errorCategory = null;
      let message = null;

      if (stderr.includes('Unable to find image') || stderr.includes('pull access denied') || stderr.includes('repository does not exist')) {
        errorCategory = 'IMAGE_NOT_FOUND';
        message = `Execution container image '${cfg.imageName}' is not available locally.`;
      } else if (stderr.includes('Cannot connect to the Docker daemon') || stderr.includes('failed to connect to the docker API') || stderr.includes('daemon is not running')) {
        errorCategory = 'DOCKER_UNAVAILABLE';
        message = 'Docker daemon is not running or unreachable.';
      } else if (timedOut) {
        errorCategory = 'TIMEOUT_ERROR';
        message = `Execution timed out after ${cfg.executionTimeoutMs}ms.`;
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
