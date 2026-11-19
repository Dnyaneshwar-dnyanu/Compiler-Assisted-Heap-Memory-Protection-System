import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {
  getCompilerConfig,
  IMMUTABLE_COMPILER_SECURITY,
  toWslPath
} from './compilerConfig.js';

let customSpawn = null;
let removeContainerHook = null;
let runnerTestOverrides = null;
let testFallbackExplicitlyEnabled = false;

/**
 * Sets mock spawn function for testing.
 */
export function setCompilerCustomSpawn(fn) {
  customSpawn = fn;
}

export function resetCompilerCustomSpawn() {
  customSpawn = null;
  removeContainerHook = null;
  runnerTestOverrides = null;
}

// Backwards-compatible aliases
export const setCustomSpawn = setCompilerCustomSpawn;
export const resetCustomSpawn = resetCompilerCustomSpawn;

export function setRemoveContainerHook(fn) {
  removeContainerHook = fn;
}

/**
 * Sets private internal test overrides for non-security settings (e.g. timeoutMs, maxOutputBytes).
 * Security settings (network, privileges, user, caps, mounts, memory, cpu, pids) CANNOT be overridden.
 */
export function setCompilerRunnerTestOverrides(overrides) {
  runnerTestOverrides = overrides ? { ...overrides } : null;
}

export function resetCompilerRunnerTestOverrides() {
  runnerTestOverrides = null;
}

/**
 * Explicitly enables host-side compiler test fallback for offline unit/mock test environments.
 * Strictly forbidden in production; throws an error if called when NODE_ENV === 'production'.
 */
export function enableCompilerTestFallback() {
  if (process.env.NODE_ENV === 'production') {
    throw new Error('Security violation: Local compiler test fallback cannot be enabled in production mode.');
  }
  testFallbackExplicitlyEnabled = true;
}

/**
 * Disables host-side compiler test fallback.
 */
export function disableCompilerTestFallback() {
  testFallbackExplicitlyEnabled = false;
}

/**
 * Checks whether test fallback is currently permissible.
 * In production mode, this ALWAYS returns false, regardless of any flags or overrides.
 */
export function isCompilerTestFallbackEnabled() {
  if (process.env.NODE_ENV === 'production') {
    return false;
  }
  return testFallbackExplicitlyEnabled;
}

/**
 * Determines whether test fallback can be triggered for a specific run.
 * Rules:
 * 1. If NODE_ENV === 'production', NEVER allow test fallback (fail-closed).
 * 2. If NODE_ENV !== 'production', only allow if:
 *    a) testFallbackExplicitlyEnabled === true (via enableCompilerTestFallback()), OR
 *    b) internalTestOverrides?.allowTestFallback === true, OR
 *    c) runnerTestOverrides?.allowTestFallback === true.
 * 3. customConfig or request data can NEVER enable test fallback.
 */
export function isTestFallbackPermitted(internalTestOverrides = null) {
  if (process.env.NODE_ENV === 'production') {
    return false;
  }
  if (internalTestOverrides && typeof internalTestOverrides.allowTestFallback === 'boolean') {
    return internalTestOverrides.allowTestFallback;
  }
  if (testFallbackExplicitlyEnabled) {
    return true;
  }
  if (runnerTestOverrides && runnerTestOverrides.allowTestFallback === true) {
    return true;
  }
  return false;
}

/**
 * Safely removes a compiler container by name if it exists.
 */
export function removeContainer(containerName, dockerBin = 'docker') {
  if (removeContainerHook) {
    try { removeContainerHook(containerName); } catch {}
  }
  return new Promise((resolve) => {
    try {
      const killer = spawn(dockerBin, ['rm', '-f', containerName], {
        stdio: 'ignore',
        shell: false
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
 * Executes C/C++ compilation and MemoryGuard LLVM instrumentation inside a dedicated
 * restricted Docker container.
 *
 * All security flags (network isolation, user privileges, resource ceilings,
 * dropped capabilities, read-only rootfs, read-only source mount, and noexec tmpfs)
 * are strictly immutable and derived exclusively from backend security configuration.
 */
export async function runCompilerContainer({
  srcDir,
  outDir,
  language = 'c',
  _internalTestOverrides = null,
  customConfig = null,
  abortSignal = null
}) {
  const baseCfg = getCompilerConfig();

  // Combine private internal test overrides if provided
  const rawOverrides = _internalTestOverrides || customConfig || runnerTestOverrides || {};

  // Strictly filter overrides: only non-security execution parameters are allowed
  const safeOverrides = {};
  if (typeof rawOverrides.timeoutMs === 'number' && rawOverrides.timeoutMs > 0) {
    safeOverrides.timeoutMs = rawOverrides.timeoutMs;
  }
  if (typeof rawOverrides.maxOutputBytes === 'number' && rawOverrides.maxOutputBytes > 0) {
    safeOverrides.maxOutputBytes = rawOverrides.maxOutputBytes;
  }

  const timeoutMs = safeOverrides.timeoutMs ?? baseCfg.timeoutMs;
  const maxOutputBytes = safeOverrides.maxOutputBytes ?? baseCfg.maxOutputBytes;
  const dockerBin = (process.env.NODE_ENV !== 'production' && typeof rawOverrides.dockerBin === 'string')
    ? rawOverrides.dockerBin
    : baseCfg.dockerBin;
  const imageName = baseCfg.imageName;

  const containerName = `mg-comp-${crypto.randomUUID()}`;
  const startTime = Date.now();

  const spawnFn = _internalTestOverrides?.customSpawn || customSpawn || spawn;

  // Normalize host bind-mount paths (forward slashes for Docker CLI)
  const mountSrcDir = srcDir.replace(/\\/g, '/');
  const mountOutDir = outDir.replace(/\\/g, '/');

  // Build explicit argument array using strictly immutable security flags
  const dockerArgs = [
    'run',
    '--name', containerName,
    '--rm',
    '--network', IMMUTABLE_COMPILER_SECURITY.network,
    '--read-only',
    '--cap-drop', IMMUTABLE_COMPILER_SECURITY.capDrop,
    '--security-opt', IMMUTABLE_COMPILER_SECURITY.securityOpt,
    '--user', IMMUTABLE_COMPILER_SECURITY.userUidGid,
    '--memory', IMMUTABLE_COMPILER_SECURITY.memoryLimit,
    '--cpus', String(IMMUTABLE_COMPILER_SECURITY.cpuLimit),
    '--pids-limit', String(IMMUTABLE_COMPILER_SECURITY.pidsLimit),
    '--init',
    '--tmpfs', IMMUTABLE_COMPILER_SECURITY.tmpfsMount,
    '-v', `${mountSrcDir}:/src:ro`,
    '-v', `${mountOutDir}:/out:rw`,
    imageName,
    '--lang', language === 'cpp' ? 'cpp' : 'c'
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
    if (isTestFallbackPermitted(_internalTestOverrides)) {
      return runTestFallback({ srcDir, outDir, language, timeoutMs, maxOutputBytes, startTime, customConfig });
    }
    return {
      success: false,
      errorCategory: 'DOCKER_UNAVAILABLE',
      message: `Failed to spawn Docker compiler process: ${err.message}`,
      exitCode: -1,
      signal: null,
      timedOut: false,
      aborted: false,
      durationMs: Date.now() - startTime,
      stdout: '',
      stderr: err.message,
      outputTruncated: false,
      command: dockerBin,
      args: dockerArgs
    };
  }

  // Handle client disconnect via abortSignal
  const abortHandler = () => {
    aborted = true;
    try {
      child?.kill('SIGKILL');
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

  // Timeout handler: SIGTERM -> docker kill -> SIGKILL
  let killTimer;
  const timeoutTimer = setTimeout(() => {
    timedOut = true;
    try {
      child?.kill('SIGTERM');
    } catch {}

    try {
      const killer = spawn(dockerBin, ['kill', containerName], { stdio: 'ignore', shell: false });
      killer.unref();
    } catch {}

    killTimer = setTimeout(() => {
      try {
        child?.kill('SIGKILL');
      } catch {}
    }, 1000);
    killTimer.unref();
  }, timeoutMs);

  child?.stdout?.on('data', (chunk) => {
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

  child?.stderr?.on('data', (chunk) => {
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
    if (!child) {
      clearTimeout(timeoutTimer);
      clearTimeout(killTimer);
      if (isTestFallbackPermitted(_internalTestOverrides)) {
        return resolve(runTestFallback({ srcDir, outDir, language, timeoutMs, maxOutputBytes, startTime, customConfig }));
      }
      return resolve({
        success: false,
        errorCategory: 'DOCKER_UNAVAILABLE',
        message: 'Failed to start Docker compiler container process.',
        exitCode: -1,
        signal: null,
        timedOut: false,
        aborted: false,
        durationMs: Date.now() - startTime,
        stdout: '',
        stderr: 'Docker child process not initialized',
        outputTruncated: false,
        command: dockerBin,
        args: dockerArgs
      });
    }

    child.on('error', (err) => {
      clearTimeout(timeoutTimer);
      clearTimeout(killTimer);
      if (abortSignal) {
        abortSignal.removeEventListener('abort', abortHandler);
      }
      removeContainer(containerName, dockerBin);

      if (isTestFallbackPermitted(_internalTestOverrides)) {
        return resolve(runTestFallback({ srcDir, outDir, language, timeoutMs, maxOutputBytes, startTime, customConfig }));
      }

      resolve({
        success: false,
        errorCategory: 'DOCKER_UNAVAILABLE',
        message: `Compiler Docker process error: ${err.message}`,
        exitCode: -1,
        signal: null,
        timedOut: false,
        aborted: false,
        durationMs: Date.now() - startTime,
        stdout: Buffer.concat(stdoutChunks).toString('utf8'),
        stderr: Buffer.concat(stderrChunks).toString('utf8') + `\n${err.message}`,
        outputTruncated,
        command: dockerBin,
        args: dockerArgs
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

      // Ensure container is cleaned up
      await removeContainer(containerName, dockerBin);

      // Detect daemon down or image missing errors
      let errorCategory = null;
      let message = null;

      const isDaemonDown = stderr.includes('Cannot connect to the Docker daemon') ||
        stderr.includes('failed to connect to the docker API') ||
        stderr.includes('daemon is not running') ||
        stderr.includes('error during connect');

      const isImageMissing = stderr.includes('Unable to find image') ||
        stderr.includes('pull access denied') ||
        stderr.includes('repository does not exist');

      if (isImageMissing) {
        errorCategory = 'IMAGE_NOT_FOUND';
        message = `Compiler container image '${imageName}' is not available locally.`;
      } else if (isDaemonDown) {
        if (isTestFallbackPermitted(_internalTestOverrides)) {
          return resolve(runTestFallback({ srcDir, outDir, language, timeoutMs, maxOutputBytes, startTime, customConfig }));
        }
        errorCategory = 'DOCKER_UNAVAILABLE';
        message = 'Docker daemon is not running or unreachable. Unable to start compilation container.';
      } else if (timedOut) {
        errorCategory = 'TIMEOUT_ERROR';
        message = `Compilation timed out after ${timeoutMs}ms.`;
      } else if (aborted) {
        errorCategory = 'ABORTED';
        message = 'Compilation aborted by client disconnect.';
      }

      resolve({
        success: code === 0 && errorCategory === null,
        errorCategory,
        message,
        exitCode: code,
        signal,
        timedOut,
        aborted,
        durationMs,
        stdout,
        stderr,
        outputTruncated,
        command: dockerBin,
        args: dockerArgs
      });
    });
  });
}

async function syncDiskFile(filePath, maxWaitMs = 1000) {
  const start = Date.now();
  while (Date.now() - start < maxWaitMs) {
    try {
      if (fs.existsSync(filePath) && fs.statSync(filePath).size > 0) {
        return;
      }
    } catch (_) {}
    await new Promise((r) => setTimeout(r, 25));
  }
}

/**
 * Offline fallback compiler used strictly during local tests when Docker daemon is not active.
 * Never used in production.
 */
async function runTestFallback({ srcDir, outDir, language, timeoutMs, maxOutputBytes, startTime, customConfig = {} }) {
  if (process.env.NODE_ENV === 'production') {
    throw new Error('Security violation: Local compiler test fallback cannot be executed in production mode.');
  }

  const cfg = { ...getCompilerConfig(), ...customConfig };
  const compilerTool = language === 'cpp' ? cfg.clangxxTool : cfg.clangTool;
  const ext = language === 'cpp' ? 'cpp' : 'c';

  const sourceFile = path.join(srcDir, `source.${ext}`);
  const irFile = path.join(outDir, 'source.ll');
  const instIrFile = path.join(outDir, 'source_inst.ll');
  const outFile = path.join(outDir, 'program.bin');

  const sourceArg = cfg.useWsl ? toWslPath(sourceFile) : sourceFile;
  const irArg = cfg.useWsl ? toWslPath(irFile) : irFile;
  const instIrArg = cfg.useWsl ? toWslPath(instIrFile) : instIrFile;
  const outArg = cfg.useWsl ? toWslPath(outFile) : outFile;
  const includeArg = cfg.useWsl ? toWslPath(cfg.runtimeIncludePath) : cfg.runtimeIncludePath;
  const pluginArg = cfg.useWsl ? toWslPath(cfg.pluginPath) : cfg.pluginPath;
  const runtimeLibArg = cfg.useWsl ? toWslPath(cfg.runtimeLibPath) : cfg.runtimeLibPath;

  // Stage 1: Frontend compilation
  const clangArgs = ['-O0', '-Xclang', '-disable-O0-optnone', '-emit-llvm', '-S', sourceArg, '-I', includeArg, '-o', irArg];
  const s1 = await runTool(compilerTool, clangArgs, { timeoutMs, maxOutputBytes, useWsl: cfg.useWsl });
  if (!s1.success) {
    const isSpawnOrNotFound = s1.spawnError || s1.exitCode === 127 || /not found|cannot find|No such file/i.test(s1.stderr);
    return {
      success: false,
      exitCode: 1,
      signal: s1.signal,
      timedOut: s1.timedOut,
      spawnError: isSpawnOrNotFound ? (s1.spawnError || new Error(s1.stderr || 'Command not found')) : null,
      aborted: false,
      durationMs: Date.now() - startTime,
      stdout: s1.stdout,
      stderr: s1.stderr + '\n=== STAGE_FAILED: FRONTEND_COMPILATION ===',
      outputTruncated: s1.outputTruncated
    };
  }

  await syncDiskFile(irFile);

  // Stage 2: MemoryGuard Pass
  const optArgs = [`-load-pass-plugin=${pluginArg}`, '-passes=memoryguard', '-S', irArg, '-o', instIrArg];
  const s2 = await runTool(cfg.optTool, optArgs, { timeoutMs, maxOutputBytes, useWsl: cfg.useWsl });
  if (!s2.success) {
    return {
      success: false,
      exitCode: 2,
      signal: s2.signal,
      timedOut: s2.timedOut,
      aborted: false,
      durationMs: Date.now() - startTime,
      stdout: s2.stdout,
      stderr: s2.stderr + '\n=== STAGE_FAILED: INSTRUMENTATION ===',
      outputTruncated: s2.outputTruncated
    };
  }

  await syncDiskFile(instIrFile);

  // Stage 3: Linking
  const linkArgs = [instIrArg, runtimeLibArg, '-pthread', '-lm', '-o', outArg];
  const s3 = await runTool(compilerTool, linkArgs, { timeoutMs, maxOutputBytes, useWsl: cfg.useWsl });
  if (!s3.success) {
    return {
      success: false,
      exitCode: 3,
      signal: s3.signal,
      timedOut: s3.timedOut,
      aborted: false,
      durationMs: Date.now() - startTime,
      stdout: s3.stdout,
      stderr: s3.stderr + '\n=== STAGE_FAILED: LINKING ===',
      outputTruncated: s3.outputTruncated
    };
  }

  await syncDiskFile(outFile);

  return {
    success: true,
    exitCode: 0,
    signal: null,
    timedOut: false,
    aborted: false,
    durationMs: Date.now() - startTime,
    stdout: (s2.stdout || '') + '\n' + (s2.stderr || '') + '\n=== COMPILATION_SUCCESS ===',
    stderr: '',
    outputTruncated: false
  };
}

/**
 * Tool execution runner.
 */
export function runTool(command, args, options = {}) {
  if (process.env.NODE_ENV === 'production') {
    throw new Error('Security violation: Host tool execution via runTool is strictly prohibited in production mode.');
  }

  const cfg = getCompilerConfig();
  const timeoutMs = options.timeoutMs || cfg.timeoutMs;
  const maxOutputBytes = options.maxOutputBytes || cfg.maxOutputBytes;
  const useWsl = options.useWsl !== undefined ? options.useWsl : cfg.useWsl;

  return new Promise((resolve) => {
    const startTime = Date.now();

    let spawnCommand = command;
    let spawnArgs = args;

    if (useWsl) {
      spawnCommand = 'wsl';
      spawnArgs = [command, ...args];
    }

    let stdoutChunks = [];
    let stdoutBytes = 0;
    let stderrChunks = [];
    let stderrBytes = 0;
    let outputTruncated = false;
    let timedOut = false;

    let child;
    try {
      child = spawn(spawnCommand, spawnArgs, {
        cwd: options.cwd || process.cwd(),
        env: { ...process.env, ...(options.env || {}) },
        stdio: ['ignore', 'pipe', 'pipe'],
        shell: false
      });
    } catch (err) {
      return resolve({
        success: false,
        command,
        args,
        exitCode: -1,
        signal: null,
        timedOut: false,
        durationMs: Date.now() - startTime,
        stdout: '',
        stderr: `Failed to spawn process ${command}: ${err.message}`,
        outputTruncated: false,
        spawnError: err
      });
    }

    const timer = setTimeout(() => {
      timedOut = true;
      try { child.kill('SIGTERM'); } catch {}
    }, timeoutMs);

    child.stdout?.on('data', (chunk) => {
      if (stdoutBytes + chunk.length <= maxOutputBytes) {
        stdoutChunks.push(chunk);
        stdoutBytes += chunk.length;
      } else {
        outputTruncated = true;
      }
    });

    child.stderr?.on('data', (chunk) => {
      if (stderrBytes + chunk.length <= maxOutputBytes) {
        stderrChunks.push(chunk);
        stderrBytes += chunk.length;
      } else {
        outputTruncated = true;
      }
    });

    child.on('error', (err) => {
      clearTimeout(timer);
      resolve({
        success: false,
        command,
        args,
        exitCode: -1,
        signal: null,
        timedOut: false,
        durationMs: Date.now() - startTime,
        stdout: Buffer.concat(stdoutChunks).toString('utf8'),
        stderr: Buffer.concat(stderrChunks).toString('utf8') + `\nProcess error: ${err.message}`,
        outputTruncated,
        spawnError: err
      });
    });

    child.on('close', (code, signal) => {
      clearTimeout(timer);
      resolve({
        success: code === 0 && !timedOut,
        command,
        args,
        exitCode: code,
        signal,
        timedOut,
        durationMs: Date.now() - startTime,
        stdout: Buffer.concat(stdoutChunks).toString('utf8'),
        stderr: Buffer.concat(stderrChunks).toString('utf8'),
        outputTruncated
      });
    });
  });
}
