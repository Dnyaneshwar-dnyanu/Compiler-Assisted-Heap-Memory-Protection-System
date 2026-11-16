import { spawn } from 'node:child_process';
import { getCompilerConfig } from './compilerConfig.js';

/**
 * Executes a compiler tool via child_process.spawn with timeout and buffer limits.
 * Guarantees argument-array invocation (no shell injection possible).
 */
export function runTool(command, args, options = {}) {
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
    let killed = false;

    let child;
    try {
      child = spawn(spawnCommand, spawnArgs, {
        cwd: options.cwd || process.cwd(),
        env: { ...process.env, ...(options.env || {}) },
        stdio: ['ignore', 'pipe', 'pipe']
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
      killed = true;
      child.kill('SIGTERM');

      // Escalate to SIGKILL after 1 second if still unresponsive
      const killTimer = setTimeout(() => {
        try {
          child.kill('SIGKILL');
        } catch {
          // Process already terminated
        }
      }, 1000);
      killTimer.unref();
    }, timeoutMs);

    child.stdout.on('data', (chunk) => {
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

    child.stderr.on('data', (chunk) => {
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
      const durationMs = Date.now() - startTime;
      const stdout = Buffer.concat(stdoutChunks).toString('utf8');
      const stderr = Buffer.concat(stderrChunks).toString('utf8');

      resolve({
        success: code === 0 && !timedOut,
        command,
        args,
        exitCode: code,
        signal,
        timedOut,
        durationMs,
        stdout,
        stderr,
        outputTruncated
      });
    });
  });
}
