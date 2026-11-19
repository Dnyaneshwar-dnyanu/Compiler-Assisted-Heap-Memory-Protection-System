import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { getCompilerConfig } from './compilerConfig.js';

const SAFE_ID_REGEX = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/;

/**
 * Validate that a target path is strictly contained within the allowed root directory.
 * Prevents directory traversal attacks and symlink traversal.
 */
function assertPathWithin(targetPath, allowedRoot) {
  const resolvedTarget = path.resolve(targetPath);
  const resolvedRoot = path.resolve(allowedRoot);

  if (!resolvedTarget.startsWith(resolvedRoot + path.sep) && resolvedTarget !== resolvedRoot) {
    throw new Error('Access denied: path traversal detected.');
  }

  // If path exists on disk, verify it is not a symlink escaping root
  if (fs.existsSync(resolvedTarget)) {
    try {
      const lstat = fs.lstatSync(resolvedTarget);
      if (lstat.isSymbolicLink()) {
        throw new Error('Access denied: symbolic link traversal detected.');
      }
      const realTarget = fs.realpathSync(resolvedTarget);
      const realRoot = fs.realpathSync(resolvedRoot);
      if (!realTarget.startsWith(realRoot + path.sep) && realTarget !== realRoot) {
        throw new Error('Access denied: realpath traversal detected.');
      }
    } catch (err) {
      if (err.message.startsWith('Access denied:')) throw err;
    }
  }
}

/**
 * Creates a unique compilation workspace with unpredictable ID.
 */
export function createWorkspace(customRoot) {
  const cfg = getCompilerConfig();
  const root = customRoot ? path.resolve(customRoot) : cfg.workspaceRoot;

  // Ensure root directory exists
  if (!fs.existsSync(root)) {
    fs.mkdirSync(root, { recursive: true });
  }

  const jobId = crypto.randomUUID();
  const workspaceDir = path.join(root, jobId);
  assertPathWithin(workspaceDir, root);

  fs.mkdirSync(workspaceDir, { recursive: true });

  return {
    jobId,
    workspaceDir,
    createdAt: Date.now()
  };
}

/**
 * Writes the source code into the workspace using a sanitized, language-specific filename
 * within a dedicated read-only source directory, alongside an output directory.
 */
export function writeSource(workspace, sourceCode, language = 'c') {
  assertPathWithin(workspace.workspaceDir, getCompilerConfig().workspaceRoot);

  const ext = language === 'cpp' ? 'cpp' : 'c';
  const fileName = `source.${ext}`;

  // Dedicated separate source and output directories for strict container mount isolation
  const srcDir = path.join(workspace.workspaceDir, 'src');
  const outDir = path.join(workspace.workspaceDir, 'out');
  fs.mkdirSync(srcDir, { recursive: true });
  fs.mkdirSync(outDir, { recursive: true });

  const filePath = path.join(srcDir, fileName);
  assertPathWithin(filePath, workspace.workspaceDir);
  fs.writeFileSync(filePath, sourceCode, 'utf8');

  // Also write legacy top-level source file for backwards compatibility
  const legacyFilePath = path.join(workspace.workspaceDir, fileName);
  try {
    fs.writeFileSync(legacyFilePath, sourceCode, 'utf8');
  } catch {}

  const executableFile = path.join(workspace.workspaceDir, 'program.bin');
  const outExecutableFile = path.join(outDir, 'program.bin');
  const manifestFile = path.join(workspace.workspaceDir, 'manifest.json');

  // Proactively purge any pre-existing binaries and manifests to prevent accidental execution of stale artifacts
  try {
    if (fs.existsSync(executableFile)) fs.rmSync(executableFile, { force: true });
    if (fs.existsSync(outExecutableFile)) fs.rmSync(outExecutableFile, { force: true });
    if (fs.existsSync(manifestFile)) fs.rmSync(manifestFile, { force: true });
  } catch {}

  return {
    srcDir,
    outDir,
    fileName,
    filePath,
    executableFile,
    outExecutableFile,
    // Accurate terminology for textual LLVM IR (.ll)
    irFile: path.join(outDir, 'source.ll'),
    instrumentedIrFile: path.join(outDir, 'source_inst.ll'),
    // Backwards-compatible aliases for existing assertions:
    bitcodeFile: path.join(outDir, 'source.ll'),
    instrumentedBitcodeFile: path.join(outDir, 'source_inst.ll')
  };
}

/**
 * Safely removes a specific workspace directory.
 */
export function cleanupWorkspace(workspaceDir) {
  const cfg = getCompilerConfig();
  try {
    assertPathWithin(workspaceDir, cfg.workspaceRoot);
    if (fs.existsSync(workspaceDir)) {
      fs.rmSync(workspaceDir, { recursive: true, force: true });
      return true;
    }
  } catch (err) {
    // Suppress cleanup errors to avoid crashing parent processes
  }
  return false;
}

/**
 * Scans workspace root and removes workspaces older than maxAgeMs (default: 1 hour).
 */
export function cleanupExpiredWorkspaces(maxAgeMs = 3600000) {
  const cfg = getCompilerConfig();
  const root = cfg.workspaceRoot;

  if (!fs.existsSync(root)) return 0;

  let cleaned = 0;
  const now = Date.now();

  try {
    const entries = fs.readdirSync(root, { withFileTypes: true });
    for (const entry of entries) {
      if (!entry.isSymbolicLink() && entry.isDirectory() && SAFE_ID_REGEX.test(entry.name)) {
        const fullPath = path.join(root, entry.name);
        try {
          const lstat = fs.lstatSync(fullPath);
          if (lstat.isSymbolicLink()) continue;
          assertPathWithin(fullPath, root);
          const stats = fs.statSync(fullPath);
          if (now - stats.mtimeMs > maxAgeMs) {
            fs.rmSync(fullPath, { recursive: true, force: true });
            cleaned++;
          }
        } catch {
          // Ignore individual entry stat/delete errors
        }
      }
    }
  } catch {
    // Ignore scan errors
  }

  return cleaned;
}
