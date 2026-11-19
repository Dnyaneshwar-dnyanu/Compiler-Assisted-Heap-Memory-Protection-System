import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {
  compileSource,
  parseMemoryGuardDiagnostics
} from '../src/services/compiler/compilerService.js';
import {
  getCompilerConfig,
  validateToolchain,
  sanitizeDiagnostics
} from '../src/services/compiler/compilerConfig.js';
import {
  createWorkspace,
  writeSource,
  cleanupWorkspace,
  cleanupExpiredWorkspaces
} from '../src/services/compiler/workspaceManager.js';
import {
  runTool,
  enableCompilerTestFallback,
  disableCompilerTestFallback
} from '../src/services/compiler/compilerRunner.js';

describe('Compiler Toolchain Configuration & Diagnostics', () => {
  before(() => {
    enableCompilerTestFallback();
  });

  after(() => {
    disableCompilerTestFallback();
  });
  it('should detect missing pass plugin or runtime library in toolchain validation', () => {
    const invalidConfig = {
      ...getCompilerConfig(),
      pluginPath: '/invalid/path/MemoryGuardPass.so',
      runtimeLibPath: '/invalid/path/libmemoryguard.a'
    };

    const res = validateToolchain(invalidConfig);
    assert.equal(res.valid, false);
    assert.ok(res.errors.length >= 2);
    assert.ok(res.errors.some((e) => e.includes('pass plugin not found')));
    assert.ok(res.errors.some((e) => e.includes('runtime library not found')));
  });

  it('should fail compilation with TOOLCHAIN_ERROR when plugin is missing', async () => {
    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c',
      customConfig: {
        pluginPath: '/tmp/nonexistent_plugin_xyz.so'
      }
    });

    assert.equal(res.success, false);
    assert.equal(res.errorCategory, 'TOOLCHAIN_ERROR');
    assert.equal(res.stage, 'TOOLCHAIN_VALIDATION');
  });

  it('should fail with error when compiler binary does not exist', async () => {
    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c',
      customConfig: {
        clangTool: 'nonexistent-clang-binary-12345',
        dockerBin: 'nonexistent-docker-binary-12345'
      }
    });

    assert.equal(res.success, false);
    assert.ok(res.errorCategory === 'SPAWN_ERROR' || res.errorCategory === 'COMPILATION_ERROR');
    assert.equal(res.stage, 'FRONTEND_COMPILATION');
  });

  it('should handle compiler timeout gracefully', async () => {
    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c',
      customConfig: {
        timeoutMs: 1 // Extremely low timeout guaranteed to fire
      }
    });

    assert.equal(res.success, false);
    assert.equal(res.errorCategory, 'TIMEOUT_ERROR');
    assert.equal(res.stage, 'FRONTEND_COMPILATION');
    assert.ok(res.message.includes('timed out'));
  });

  it('should parse structured MemoryGuard diagnostics from opt output', () => {
    const sampleOptOutput = `
[MemoryGuard] Analyzing function @main
[MemoryGuard]   [ALLOC] function=@main api=malloc ptr=%3 size=32 bytes
[MemoryGuard]   [ALLOC REWRITE] function=@main malloc -> mg_intercept_malloc
[MemoryGuard]   [WRITE] function=@main type=i32 ptr=%5 size=4 bytes origin=HEAP_CANDIDATE
[MemoryGuard]   [ALLOC REWRITE] function=@main free -> mg_intercept_free
[MemoryGuard][Summary] Module Analysis Summary:
  Total load and store instructions examined:         9
  Heap-origin accesses identified:                    3
  Runtime checks inserted:                            3
  Volatile accesses instrumented:                     0
  Non-heap accesses skipped:                          6
  Unknown-origin accesses skipped:                    0
  Atomic operations skipped:                          0
  Unsupported address-space accesses skipped:         0
  Scalable-vector accesses skipped:                   0
  Allocator calls detected and successfully rewritten: 2
  Incompatible or unsupported allocator calls skipped: 0
`;

    const summary = parseMemoryGuardDiagnostics(sampleOptOutput);
    assert.ok(summary);
    assert.equal(summary.totalAccessesExamined, 9);
    assert.equal(summary.heapAccessesIdentified, 3);
    assert.equal(summary.checksInserted, 3);
    assert.equal(summary.allocatorCallsRewritten, 2);
    assert.equal(summary.nonHeapSkipped, 6);
    assert.equal(summary.atomicOperationsSkipped, 0);
    assert.deepEqual(summary.functionsAnalyzed, ['main']);
  });

  it('should return null when parsing output with no MemoryGuard diagnostics', () => {
    const unformattedOutput = 'Random compiler output with no MemoryGuard tags';
    const summary = parseMemoryGuardDiagnostics(unformattedOutput);
    assert.equal(summary, null);
  });

  it('should truncate excessive compiler output and flag outputTruncated', async () => {
    // Run tool with 20-byte buffer limit
    const result = await runTool('echo', ['This is a very long string that definitely exceeds twenty bytes of output'], {
      maxOutputBytes: 20
    });

    assert.ok(result.stdout.length <= 20);
    assert.equal(result.outputTruncated, true);
  });

  it('should sanitize host workspace paths from diagnostic messages', () => {
    const fakeWorkspace = 'D:\\Notes\\Project\\backend\\.workspaces\\1234-5678';
    const fakeLog = `Error in file ${fakeWorkspace}\\source.c: syntax error at line 5`;

    const sanitized = sanitizeDiagnostics(fakeLog, fakeWorkspace);
    assert.equal(sanitized.includes(fakeWorkspace), false);
    assert.ok(sanitized.includes('<workspace>/source.c'));
  });
});

describe('Workspace Management & Path Safety', () => {
  it('should generate distinct unpredictable workspace identifiers', () => {
    const ws1 = createWorkspace();
    const ws2 = createWorkspace();

    assert.notEqual(ws1.jobId, ws2.jobId);
    assert.notEqual(ws1.workspaceDir, ws2.workspaceDir);
    assert.ok(fs.existsSync(ws1.workspaceDir));
    assert.ok(fs.existsSync(ws2.workspaceDir));

    cleanupWorkspace(ws1.workspaceDir);
    cleanupWorkspace(ws2.workspaceDir);
    assert.equal(fs.existsSync(ws1.workspaceDir), false);
    assert.equal(fs.existsSync(ws2.workspaceDir), false);
  });

  it('should safely write source file with correct extension', () => {
    const ws = createWorkspace();
    const pathsC = writeSource(ws, 'int main() {}', 'c');
    assert.equal(pathsC.fileName, 'source.c');
    assert.ok(fs.existsSync(pathsC.filePath));
    assert.equal(fs.readFileSync(pathsC.filePath, 'utf8'), 'int main() {}');

    const pathsCpp = writeSource(ws, 'int main() {}', 'cpp');
    assert.equal(pathsCpp.fileName, 'source.cpp');
    assert.ok(fs.existsSync(pathsCpp.filePath));

    cleanupWorkspace(ws.workspaceDir);
  });

  it('should clean up expired workspaces', () => {
    const ws = createWorkspace();
    // Simulate expired workspace by modifying mtime
    const oldTime = new Date(Date.now() - 10000);
    fs.utimesSync(ws.workspaceDir, oldTime, oldTime);

    const cleaned = cleanupExpiredWorkspaces(5000); // Max age 5 seconds
    assert.ok(cleaned >= 1);
    assert.equal(fs.existsSync(ws.workspaceDir), false);
  });
});
