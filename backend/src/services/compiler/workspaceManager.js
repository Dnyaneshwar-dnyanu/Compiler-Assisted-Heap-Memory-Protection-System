import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { getCompilerConfig } from './compilerConfig.js';

const SAFE_ID_REGEX = /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/;

/**
 * Validate that a target path is strictly contained within the allowed root directory.
 * Prevents directory traversal attacks.
 */
function assertPathWithin(targetPath, allowedRoot) {
  const resolvedTarget = path.resolve(targetPath);
  const resolvedRoot = path.resolve(allowedRoot);

  if (!resolvedTarget.startsWith(resolvedRoot + path.sep) && resolvedTarget !== resolvedRoot) {
    throw new Error('Access denied: path traversal detected.');
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
 * Writes the source code into the workspace using a sanitized, language-specific filename.
 */
export function writeSource(workspace, sourceCode, language = 'c') {
  assertPathWithin(workspace.workspaceDir, getCompilerConfig().workspaceRoot);

  const ext = language === 'cpp' ? 'cpp' : 'c';
  const fileName = `source.${ext}`;
  const filePath = path.join(workspace.workspaceDir, fileName);

  assertPathWithin(filePath, workspace.workspaceDir);
  fs.writeFileSync(filePath, sourceCode, 'utf8');

  return {
    fileName,
    filePath,
    bitcodeFile: path.join(workspace.workspaceDir, 'source.ll'),
    instrumentedBitcodeFile: path.join(workspace.workspaceDir, 'source_inst.ll'),
    executableFile: path.join(workspace.workspaceDir, 'program.bin')
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
      if (entry.isDirectory() && SAFE_ID_REGEX.test(entry.name)) {
        const fullPath = path.join(root, entry.name);
        try {
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
