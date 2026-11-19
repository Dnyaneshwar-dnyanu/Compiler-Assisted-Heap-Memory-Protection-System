import { spawn } from 'node:child_process';
import process from 'node:process';

/**
 * Execution configuration for Docker-based runtime isolation.
 */
export function getExecutionConfig() {
  return {
    dockerBin: process.env.MG_DOCKER_BIN || 'docker',
    imageName: process.env.MG_DOCKER_IMAGE || 'memoryguard-runner:latest',
    executionTimeoutMs: parseInt(process.env.MG_EXECUTION_TIMEOUT_MS || '10000', 10),
    memoryLimit: process.env.MG_DOCKER_MEMORY || '64m',
    cpuLimit: process.env.MG_DOCKER_CPUS || '0.5',
    pidsLimit: parseInt(process.env.MG_DOCKER_PIDS_LIMIT || '32', 10),
    userUidGid: process.env.MG_DOCKER_USER || '10001:10001',
    maxOutputBytes: parseInt(process.env.MG_MAX_EXECUTION_OUTPUT_BYTES || '65536', 10), // 64 KB
    tmpfsMount: process.env.MG_DOCKER_TMPFS || '/tmp:rw,noexec,nosuid,size=16m',
    network: 'none',
    readOnly: true,
    capDrop: 'ALL',
    securityOpt: 'no-new-privileges',
    useInit: true,
    autoRemove: true
  };
}

/**
 * Verifies if the Docker CLI and daemon are reachable.
 * Returns { available: boolean, reason?: string }.
 */
export function checkDockerAvailability(cfg = getExecutionConfig()) {
  return new Promise((resolve) => {
    let output = '';
    let child;

    try {
      child = spawn(cfg.dockerBin, ['info', '--format', '{{.ServerVersion}}'], {
        stdio: ['ignore', 'pipe', 'pipe']
      });
    } catch (err) {
      return resolve({
        available: false,
        reason: `Docker CLI spawn failed: ${err.message}`
      });
    }

    const timer = setTimeout(() => {
      try {
        child.kill('SIGTERM');
      } catch {}
      resolve({
        available: false,
        reason: 'Docker availability check timed out'
      });
    }, 3000);

    child.stdout.on('data', (d) => {
      output += d.toString();
    });

    child.stderr.on('data', (d) => {
      output += d.toString();
    });

    child.on('error', (err) => {
      clearTimeout(timer);
      resolve({
        available: false,
        reason: `Docker daemon unreachable: ${err.message}`
      });
    });

    child.on('close', (code) => {
      clearTimeout(timer);
      if (code === 0) {
        resolve({
          available: true,
          serverVersion: output.trim()
        });
      } else {
        resolve({
          available: false,
          reason: output.trim() || `Docker check exited with code ${code}`
        });
      }
    });
  });
}
