import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import fs from 'node:fs';
import path from 'node:path';
import {
  runCompilerContainer,
  setCompilerCustomSpawn,
  resetCompilerCustomSpawn
} from '../src/services/compiler/compilerRunner.js';
import {
  compileSource,
  parseMemoryGuardDiagnostics
} from '../src/services/compiler/compilerService.js';
import {
  createWorkspace,
  writeSource,
  cleanupWorkspace
} from '../src/services/compiler/workspaceManager.js';
import { IMMUTABLE_COMPILER_SECURITY } from '../src/services/compiler/compilerConfig.js';

describe('Docker Compiler Sandbox Runner & Security (Step 26)', () => {
  let testWorkspace;

  beforeEach(() => {
    testWorkspace = createWorkspace();
  });

  afterEach(() => {
    resetCompilerCustomSpawn();
    if (testWorkspace?.workspaceDir) {
      cleanupWorkspace(testWorkspace.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 1. Mandatory Security Arguments Verification
  // ---------------------------------------------------------------------------
  it('should enforce all mandatory security restrictions in docker run compiler arguments', async () => {
    let capturedCommand = '';
    let capturedArgs = [];
    let capturedOptions = null;

    setCompilerCustomSpawn((cmd, args, opts) => {
      capturedCommand = cmd;
      capturedArgs = args;
      capturedOptions = opts;

      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
    const result = await runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'c'
    });

    assert.equal(result.success, true);
    assert.equal(capturedCommand, 'docker');
    assert.equal(capturedArgs[0], 'run');

    // Safe execution options: no shell
    assert.ok(capturedOptions);
    assert.equal(capturedOptions.shell, false);

    // Verify container name is unique and prefixed
    const nameIndex = capturedArgs.indexOf('--name');
    assert.ok(nameIndex !== -1, '--name flag must be present');
    assert.ok(capturedArgs[nameIndex + 1].startsWith('mg-comp-'), 'Container name must start with mg-comp-');

    // Verify mandatory immutable security restrictions
    assert.ok(capturedArgs.includes('--rm'), '--rm must be present for auto-cleanup');
    assert.ok(capturedArgs.includes('--read-only'), '--read-only must be present to protect container rootfs');
    assert.ok(capturedArgs.includes('--init'), '--init must be present for zombie reaping');

    // Network isolation
    const netIdx = capturedArgs.indexOf('--network');
    assert.ok(netIdx !== -1, '--network flag must be present');
    assert.equal(capturedArgs[netIdx + 1], 'none', 'Network access must be disabled (none)');

    // Capabilities dropped
    const capIdx = capturedArgs.indexOf('--cap-drop');
    assert.ok(capIdx !== -1, '--cap-drop flag must be present');
    assert.equal(capturedArgs[capIdx + 1], 'ALL', 'All Linux capabilities must be dropped');

    // No privilege escalation
    const secIdx = capturedArgs.indexOf('--security-opt');
    assert.ok(secIdx !== -1, '--security-opt flag must be present');
    assert.equal(capturedArgs[secIdx + 1], 'no-new-privileges');

    // Unprivileged non-root user
    const userIdx = capturedArgs.indexOf('--user');
    assert.ok(userIdx !== -1, '--user flag must be present');
    assert.equal(capturedArgs[userIdx + 1], '10001:10001', 'Container must run as unprivileged UID/GID 10001:10001');

    // Resource ceilings
    const memIdx = capturedArgs.indexOf('--memory');
    assert.ok(memIdx !== -1, '--memory flag must be present');
    assert.equal(capturedArgs[memIdx + 1], '512m');

    const cpuIdx = capturedArgs.indexOf('--cpus');
    assert.ok(cpuIdx !== -1, '--cpus flag must be present');
    assert.equal(capturedArgs[cpuIdx + 1], '1.0');

    const pidsIdx = capturedArgs.indexOf('--pids-limit');
    assert.ok(pidsIdx !== -1, '--pids-limit flag must be present');
    assert.equal(capturedArgs[pidsIdx + 1], '64');

    // Ephemeral tmpfs mount
    const tmpfsIdx = capturedArgs.indexOf('--tmpfs');
    assert.ok(tmpfsIdx !== -1, '--tmpfs flag must be present');
    assert.equal(capturedArgs[tmpfsIdx + 1], '/tmp:rw,noexec,nosuid,size=64m');

    // Volume Mounts: source MUST be read-only (:ro), output MUST be writable (:rw)
    const vIndices = [];
    capturedArgs.forEach((arg, i) => {
      if (arg === '-v') vIndices.push(i);
    });
    assert.equal(vIndices.length, 2, 'Exactly two volume mounts expected: src and out');

    const mount1 = capturedArgs[vIndices[0] + 1];
    const mount2 = capturedArgs[vIndices[1] + 1];

    assert.ok(mount1.endsWith(':/src:ro'), `Source mount must be read-only: received ${mount1}`);
    assert.ok(mount2.endsWith(':/out:rw'), `Output mount must be writable: received ${mount2}`);

    // Verify forbidden mounts are absent
    for (const arg of capturedArgs) {
      assert.equal(arg.includes('/var/run/docker.sock'), false, 'Docker socket must never be mounted');
      assert.equal(arg.includes('docker.sock'), false);
    }
  });

  // ---------------------------------------------------------------------------
  // 2. Tamper Resistance: Cannot Override Immutable Security Settings
  // ---------------------------------------------------------------------------
  it('should ignore any client attempt to tamper with security flags', async () => {
    let capturedArgs = [];

    setCompilerCustomSpawn((cmd, args) => {
      capturedArgs = args;
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const maliciousOverrides = {
      network: 'host',
      readOnly: false,
      capDrop: 'NONE',
      securityOpt: 'unconfined',
      privileged: true,
      userUidGid: '0:0',
      memoryLimit: '16g',
      cpuLimit: '16',
      pidsLimit: 99999
    };

    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
    await runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'c',
      customConfig: maliciousOverrides
    });

    // Verify immutable restrictions were preserved
    const netIdx = capturedArgs.indexOf('--network');
    assert.equal(capturedArgs[netIdx + 1], 'none');

    const capIdx = capturedArgs.indexOf('--cap-drop');
    assert.equal(capturedArgs[capIdx + 1], 'ALL');

    const userIdx = capturedArgs.indexOf('--user');
    assert.equal(capturedArgs[userIdx + 1], '10001:10001');

    assert.equal(capturedArgs.includes('--privileged'), false);
  });

  // ---------------------------------------------------------------------------
  // 3. Language Selection & Compiler Arguments
  // ---------------------------------------------------------------------------
  it('should pass correct language flag to compiler container for C', async () => {
    let capturedArgs = [];

    setCompilerCustomSpawn((cmd, args) => {
      capturedArgs = args;
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
    await runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'c'
    });

    const langIdx = capturedArgs.indexOf('--lang');
    assert.ok(langIdx !== -1);
    assert.equal(capturedArgs[langIdx + 1], 'c');
  });

  it('should pass correct language flag to compiler container for C++', async () => {
    let capturedArgs = [];

    setCompilerCustomSpawn((cmd, args) => {
      capturedArgs = args;
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'cpp');
    await runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'cpp'
    });

    const langIdx = capturedArgs.indexOf('--lang');
    assert.ok(langIdx !== -1);
    assert.equal(capturedArgs[langIdx + 1], 'cpp');
  });

  // ---------------------------------------------------------------------------
  // 4. Stage Failure Identification & Error Handling
  // ---------------------------------------------------------------------------
  it('should identify frontend compilation failure (exit code 1)', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('/src/source.c:3:1: error: expected identifier\n=== STAGE_FAILED: FRONTEND_COMPILATION ==='));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'invalid C syntax !@#$',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.equal(res.stage, 'FRONTEND_COMPILATION');
    assert.equal(res.errorCategory, 'COMPILATION_ERROR');
    assert.ok(res.diagnostics.includes('error: expected identifier'));
    // Verify mount path was sanitized
    assert.equal(res.diagnostics.includes('/src/'), false);
  });

  it('should identify instrumentation pass failure (exit code 2)', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('opt-18: Pass MemoryGuard failed on module\n=== STAGE_FAILED: INSTRUMENTATION ==='));
        emitter.emit('close', 2, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.equal(res.stage, 'INSTRUMENTATION');
    assert.equal(res.errorCategory, 'INSTRUMENTATION_ERROR');
  });

  it('should identify linking failure (exit code 3)', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('undefined reference to `unresolved_symbol`\n=== STAGE_FAILED: LINKING ==='));
        emitter.emit('close', 3, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'extern void unresolved_symbol(); int main() { unresolved_symbol(); return 0; }',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.equal(res.stage, 'LINKING');
    assert.equal(res.errorCategory, 'LINKER_ERROR');
  });

  // ---------------------------------------------------------------------------
  // 5. Successful Compilation & Diagnostics Extraction
  // ---------------------------------------------------------------------------
  it('should report successful compilation, 3 completed stages, and parse diagnostics', async () => {
    const mockOptSummary = `
=== STAGE_START: FRONTEND_COMPILATION ===
=== STAGE_COMPLETED: FRONTEND_COMPILATION ===
=== STAGE_START: INSTRUMENTATION ===
[MemoryGuard] Analyzing function @main
[MemoryGuard]   [ALLOC] function=@main api=malloc ptr=%4 size=32 bytes
[MemoryGuard]   [WRITE] function=@main type=i32 ptr=%8 size=4 bytes origin=HEAP_CANDIDATE
[MemoryGuard]   [WRITE] function=@main type=i32 ptr=%12 size=4 bytes origin=HEAP_CANDIDATE
[MemoryGuard][Summary] Module Analysis Summary:
  Total load and store instructions examined:         10
  Heap-origin accesses identified:                    4
  Runtime checks inserted:                            4
  Volatile accesses instrumented:                     0
  Non-heap accesses skipped:                          6
  Unknown-origin accesses skipped:                    0
  Atomic operations skipped:                          0
  Unsupported address-space accesses skipped:         0
  Scalable-vector accesses skipped:                   0
  Allocator calls detected and successfully rewritten: 2
  Incompatible or unsupported allocator calls skipped: 0
=== STAGE_COMPLETED: INSTRUMENTATION ===
=== STAGE_START: LINKING ===
=== STAGE_COMPLETED: LINKING ===
=== COMPILATION_SUCCESS ===
`;

    setCompilerCustomSpawn((cmd, args) => {
      // Create mock output files in the outDir
      const vIndices = [];
      args.forEach((arg, i) => { if (arg === '-v') vIndices.push(i); });
      const outMount = args[vIndices[1] + 1];
      const hostOutDir = outMount.replace(':/out:rw', '');

      try {
        fs.writeFileSync(path.join(hostOutDir, 'program.bin'), 'mock-elf-binary', 'utf8');
        fs.writeFileSync(path.join(hostOutDir, 'source.ll'), '; textual LLVM IR', 'utf8');
        fs.writeFileSync(path.join(hostOutDir, 'source_inst.ll'), '; instrumented textual LLVM IR', 'utf8');
      } catch {}

      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from(mockOptSummary));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c'
    });

    assert.equal(res.success, true);
    assert.equal(res.status, 'completed');
    assert.equal(res.stages.length, 3);
    assert.equal(res.stages[0].name, 'FRONTEND_COMPILATION');
    assert.equal(res.stages[0].status, 'completed');
    assert.equal(res.stages[1].name, 'INSTRUMENTATION');
    assert.equal(res.stages[1].status, 'completed');
    assert.equal(res.stages[2].name, 'LINKING');
    assert.equal(res.stages[2].status, 'completed');

    // Diagnostics verified
    assert.ok(res.diagnostics.summary);
    assert.equal(res.diagnostics.summary.totalAccessesExamined, 10);
    assert.equal(res.diagnostics.summary.heapAccessesIdentified, 4);
    assert.equal(res.diagnostics.summary.checksInserted, 4);
    assert.equal(res.diagnostics.summary.allocatorCallsRewritten, 2);

    // Artifacts verified
    assert.ok(res.artifacts.executableAvailable);
    assert.ok(res.artifacts.irAvailable);
    assert.ok(res.artifacts.instrumentedIrAvailable);
  });

  // ---------------------------------------------------------------------------
  // 6. Reliability: Timeout, Cancellation, and Daemon Errors
  // ---------------------------------------------------------------------------
  it('should handle compiler timeout gracefully', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = (sig) => {
        emitter.emit('close', 137, sig);
      };
      // Do not emit close: let timeout fire
      return emitter;
    });

    const paths = writeSource(testWorkspace, 'int main() { while(1); }', 'c');
    const res = await runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'c',
      _internalTestOverrides: { timeoutMs: 50 }
    });

    assert.equal(res.timedOut, true);
    assert.equal(res.errorCategory, 'TIMEOUT_ERROR');
  });

  it('should detect when Docker daemon is unavailable', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?'));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.equal(res.errorCategory, 'TOOLCHAIN_ERROR');
    assert.ok(res.message.includes('Docker daemon is not running'));
  });

  it('should detect when compiler Docker image is missing locally', async () => {
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('Unable to find image "memoryguard-compiler:latest" locally'));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'int main() { return 0; }',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.equal(res.errorCategory, 'TOOLCHAIN_ERROR');
    assert.ok(res.message.includes('not available locally'));
  });

  it('should handle client abort signal during compilation', async () => {
    const controller = new AbortController();

    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {
        emitter.emit('close', 137, 'SIGKILL');
      };
      return emitter;
    });

    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
    const promise = runCompilerContainer({
      srcDir: paths.srcDir,
      outDir: paths.outDir,
      language: 'c',
      abortSignal: controller.signal
    });

    controller.abort();
    const res = await promise;

    assert.equal(res.aborted, true);
    assert.equal(res.errorCategory, 'ABORTED');
  });
});
