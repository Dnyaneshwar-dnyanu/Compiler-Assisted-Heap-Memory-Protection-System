import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import process from 'node:process';
import { spawn } from 'node:child_process';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Resolve repository root: from backend/src/services/compiler -> project root
const REPO_ROOT = path.resolve(__dirname, '../../../../');
const BACKEND_ROOT = path.resolve(__dirname, '../../../');

/**
 * Non-negotiable, immutable Docker security restrictions for the compiler container.
 * These flags are strictly enforced and can NEVER be overridden by any HTTP client or request.
 */
export const IMMUTABLE_COMPILER_SECURITY = Object.freeze({
  network: 'none',
  readOnly: true,
  capDrop: 'ALL',
  securityOpt: 'no-new-privileges',
  userUidGid: '10001:10001',
  memoryLimit: '512m',
  cpuLimit: '1.0',
  pidsLimit: 64,
  tmpfsMount: '/tmp:rw,noexec,nosuid,size=64m',
  init: true,
  rm: true
});

/**
 * Convert a Windows path (e.g. D:\path\file) to a WSL path (/mnt/d/path/file).
 * If already a POSIX path, returns normalized path.
 */
export function toWslPath(inputPath) {
  if (!inputPath) return inputPath;
  const normalized = inputPath.replace(/\\/g, '/');
  const driveMatch = normalized.match(/^([a-zA-Z]):\/(.*)$/);
  if (driveMatch) {
    const driveLetter = driveMatch[1].toLowerCase();
    return `/mnt/${driveLetter}/${driveMatch[2]}`;
  }
  return normalized;
}

/**
 * Remove internal workspace and filesystem paths from compiler output to prevent leaking host paths.
 */
export function sanitizeDiagnostics(rawText, workspaceDir = '') {
  if (!rawText || typeof rawText !== 'string') return '';
  let sanitized = rawText;

  // Sanitize container-internal mount prefixes
  sanitized = sanitized
    .replace(/\/src\/source\.(c|cpp)/g, 'source.$1')
    .replace(/\/out\/program\.bin/g, 'program.bin')
    .replace(/\/out\//g, '')
    .replace(/\/src\//g, '')
    .replace(/\/opt\/memoryguard\/include[\\/]/g, '')
    .replace(/\/opt\/memoryguard\/lib[\\/]/g, '');

  if (workspaceDir) {
    const winDir = workspaceDir.replace(/\\/g, '/');
    const wslDir = toWslPath(workspaceDir);

    sanitized = sanitized
      .split(workspaceDir).join('<workspace>')
      .split(winDir).join('<workspace>')
      .split(wslDir).join('<workspace>');

    // Normalize path separator following <workspace>
    sanitized = sanitized.replace(/<workspace>[\\/]/g, '<workspace>/');
  }

  // Also sanitize repo root occurrences
  const repoWin = REPO_ROOT.replace(/\\/g, '/');
  const repoWsl = toWslPath(REPO_ROOT);
  sanitized = sanitized
    .split(REPO_ROOT).join('<project_root>')
    .split(repoWin).join('<project_root>')
    .split(repoWsl).join('<project_root>');

  sanitized = sanitized.replace(/<project_root>[\\/]/g, '<project_root>/');

  return sanitized;
}

/**
 * Determines whether WSL runner should be used.
 * Defaults to true on Windows (since LLVM 18 and Linux ELF .so/.a run in WSL).
 */
function resolveUseWsl() {
  if (process.env.MG_USE_WSL !== undefined) {
    return process.env.MG_USE_WSL === 'true' || process.env.MG_USE_WSL === '1';
  }
  return process.platform === 'win32';
}

/**
 * Resolve compiler Docker and toolchain paths with sensible defaults.
 */
export function getCompilerConfig() {
  const useWsl = resolveUseWsl();

  // Docker Compiler Sandbox Configuration (Step 26)
  const dockerBin = process.env.MG_COMPILER_DOCKER_BIN || process.env.MG_DOCKER_BIN || 'docker';
  const imageName = process.env.MG_COMPILER_DOCKER_IMAGE || 'memoryguard-compiler:latest';
  const timeoutMs = parseInt(process.env.MG_COMPILER_TIMEOUT_MS || '45000', 10);
  const memoryLimit = process.env.MG_COMPILER_MEMORY || IMMUTABLE_COMPILER_SECURITY.memoryLimit;
  const cpuLimit = process.env.MG_COMPILER_CPUS || IMMUTABLE_COMPILER_SECURITY.cpuLimit;
  const pidsLimit = parseInt(process.env.MG_COMPILER_PIDS_LIMIT || String(IMMUTABLE_COMPILER_SECURITY.pidsLimit), 10);
  const userUidGid = process.env.MG_COMPILER_USER || IMMUTABLE_COMPILER_SECURITY.userUidGid;
  const tmpfsMount = process.env.MG_COMPILER_TMPFS || IMMUTABLE_COMPILER_SECURITY.tmpfsMount;

  // Resource limits
  const maxSourceBytes = parseInt(process.env.MG_MAX_SOURCE_BYTES || '524288', 10); // 512 KB
  const maxOutputBytes = parseInt(process.env.MG_MAX_OUTPUT_BYTES || '262144', 10); // 256 KB

  // Legacy/Host tool definitions for fallback and testing
  const clangTool = process.env.MG_CLANG || 'clang-18';
  const clangxxTool = process.env.MG_CLANGXX || 'clang++-18';
  const optTool = process.env.MG_OPT || 'opt-18';

  const defaultPlugin = path.resolve(REPO_ROOT, 'build/compiler/MemoryGuardPass.so');
  const defaultRuntimeLib = path.resolve(REPO_ROOT, 'build/libmemoryguard.a');
  const defaultInclude = path.resolve(REPO_ROOT, 'runtime/include');

  const pluginPath = process.env.MG_PASS_PLUGIN
    ? path.resolve(REPO_ROOT, process.env.MG_PASS_PLUGIN)
    : defaultPlugin;

  const runtimeLibPath = process.env.MG_RUNTIME_LIBRARY
    ? path.resolve(REPO_ROOT, process.env.MG_RUNTIME_LIBRARY)
    : defaultRuntimeLib;

  const runtimeIncludePath = process.env.MG_RUNTIME_INCLUDE
    ? path.resolve(REPO_ROOT, process.env.MG_RUNTIME_INCLUDE)
    : defaultInclude;

  const workspaceRoot = process.env.MG_WORKSPACE_ROOT
    ? path.resolve(BACKEND_ROOT, process.env.MG_WORKSPACE_ROOT)
    : path.resolve(BACKEND_ROOT, '.workspaces');

  return {
    useWsl,
    dockerBin,
    imageName,
    timeoutMs,
    memoryLimit,
    cpuLimit,
    pidsLimit,
    userUidGid,
    tmpfsMount,
    clangTool,
    clangxxTool,
    optTool,
    pluginPath,
    runtimeLibPath,
    runtimeIncludePath,
    workspaceRoot,
    maxSourceBytes,
    maxOutputBytes,
    repoRoot: REPO_ROOT
  };
}

/**
 * Verifies if the Docker CLI and daemon are reachable for the compiler.
 * Returns { available: boolean, reason?: string, serverVersion?: string }.
 */
export function checkCompilerDockerAvailability(cfg = getCompilerConfig()) {
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
        reason: 'Compiler Docker availability check timed out'
      });
    }, 3000);

    child.stdout?.on('data', (d) => {
      output += d.toString();
    });

    child.stderr?.on('data', (d) => {
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

/**
 * Validate that required tools, plugins, and libraries exist.
 */
export function validateToolchain(cfg = getCompilerConfig()) {
  const errors = [];

  if (!fs.existsSync(cfg.pluginPath)) {
    errors.push(`MemoryGuard pass plugin not found at: ${cfg.pluginPath}. Ensure project has been built.`);
  }

  if (!fs.existsSync(cfg.runtimeLibPath)) {
    errors.push(`MemoryGuard runtime library not found at: ${cfg.runtimeLibPath}. Ensure project has been built.`);
  }

  if (!fs.existsSync(cfg.runtimeIncludePath)) {
    errors.push(`MemoryGuard runtime include directory not found at: ${cfg.runtimeIncludePath}.`);
  }

  return {
    valid: errors.length === 0,
    errors
  };
}
