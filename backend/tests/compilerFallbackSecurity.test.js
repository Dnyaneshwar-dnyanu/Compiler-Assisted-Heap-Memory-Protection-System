import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import EventEmitter from 'node:events';
import path from 'node:path';
import fs from 'node:fs';
import request from 'supertest';
import app from '../src/app.js';
import {
  runCompilerContainer,
  runTool,
  setCompilerCustomSpawn,
  resetCompilerCustomSpawn,
  enableCompilerTestFallback,
  disableCompilerTestFallback,
  isCompilerTestFallbackEnabled,
  isTestFallbackPermitted
} from '../src/services/compiler/compilerRunner.js';
import { compileSource } from '../src/services/compiler/compilerService.js';
import { createWorkspace, writeSource, cleanupWorkspace } from '../src/services/compiler/workspaceManager.js';
import { getCompilerConfig } from '../src/services/compiler/compilerConfig.js';

describe('Compiler Production Isolation & Fail-Closed Fallback Security (Step 26)', () => {
  let testWorkspace;

  beforeEach(() => {
    testWorkspace = createWorkspace();
    resetCompilerCustomSpawn();
  });

  afterEach(() => {
    resetCompilerCustomSpawn();
    if (testWorkspace?.workspaceDir) {
      cleanupWorkspace(testWorkspace.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 1. Docker unavailable in production -> compilation fails
  // ---------------------------------------------------------------------------
  it('should fail with DOCKER_UNAVAILABLE in production when Docker is unavailable', async () => {
    const origEnv = process.env.NODE_ENV;
    try {
      process.env.NODE_ENV = 'production';

      // Mock Docker daemon unreachable
      const mockDaemonDown = () => {
        const emitter = new EventEmitter();
        emitter.stdout = new EventEmitter();
        emitter.stderr = new EventEmitter();
        process.nextTick(() => {
          emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?'));
          emitter.emit('close', 1, null);
        });
        return emitter;
      };

      const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
      const res = await runCompilerContainer({
        srcDir: paths.srcDir,
        outDir: paths.outDir,
        language: 'c',
        _internalTestOverrides: { customSpawn: mockDaemonDown }
      });

      assert.equal(res.success, false);
      assert.equal(res.errorCategory, 'DOCKER_UNAVAILABLE');
      assert.ok(res.message.includes('Docker daemon is not running'));

      // Also verify through compileSource service
      const serviceRes = await compileSource({
        sourceCode: 'int main() { return 0; }',
        language: 'c',
        _internalTestOverrides: { customSpawn: mockDaemonDown }
      });

      assert.equal(serviceRes.success, false);
      assert.equal(serviceRes.errorCategory, 'TOOLCHAIN_ERROR');
      assert.equal(serviceRes.stage, 'TOOLCHAIN_VALIDATION');
      assert.ok(serviceRes.message.includes('Docker daemon is not running'));
    } finally {
      process.env.NODE_ENV = origEnv;
    }
  });

  // ---------------------------------------------------------------------------
  // 2. Docker unavailable in test mode -> fallback occurs ONLY when explicitly enabled
  // ---------------------------------------------------------------------------
  it('should fail when Docker is unavailable in test mode unless fallback is explicitly enabled', async () => {
    const origEnv = process.env.NODE_ENV;
    try {
      process.env.NODE_ENV = 'test';

      // Simulate Docker daemon down
      const mockDaemonDown = () => {
        const emitter = new EventEmitter();
        emitter.stdout = new EventEmitter();
        emitter.stderr = new EventEmitter();
        process.nextTick(() => {
          emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon'));
          emitter.emit('close', 1, null);
        });
        return emitter;
      };

      const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');

      // 2a. With fallback disabled (allowTestFallback: false), Docker unavailable strictly fails
      const resDisabled = await runCompilerContainer({
        srcDir: paths.srcDir,
        outDir: paths.outDir,
        language: 'c',
        _internalTestOverrides: { customSpawn: mockDaemonDown, allowTestFallback: false }
      });

      assert.equal(resDisabled.success, false);
      assert.equal(resDisabled.errorCategory, 'DOCKER_UNAVAILABLE');

      // 2b. When explicitly enabled via internal test override, local fallback occurs
      const resEnabled = await runCompilerContainer({
        srcDir: paths.srcDir,
        outDir: paths.outDir,
        language: 'c',
        _internalTestOverrides: { customSpawn: mockDaemonDown, allowTestFallback: true }
      });

      assert.equal(resEnabled.success, true);
      assert.equal(resEnabled.exitCode, 0);
    } finally {
      process.env.NODE_ENV = origEnv;
    }
  });

  // ---------------------------------------------------------------------------
  // 3. Production configuration cannot activate the test fallback
  // ---------------------------------------------------------------------------
  it('should prevent production configuration or requests from activating test fallback', async () => {
    const origEnv = process.env.NODE_ENV;
    try {
      process.env.NODE_ENV = 'production';

      // 3a. enableCompilerTestFallback throws in production
      assert.throws(
        () => enableCompilerTestFallback(),
        /cannot be enabled in production mode/
      );

      // 3b. isCompilerTestFallbackEnabled returns false in production
      assert.equal(isCompilerTestFallbackEnabled(), false);

      // 3c. Even if internal overrides or customConfig passes allowTestFallback: true, it is rejected
      assert.equal(isTestFallbackPermitted({ allowTestFallback: true }), false);

      const mockDaemonDown = () => {
        const emitter = new EventEmitter();
        emitter.stdout = new EventEmitter();
        emitter.stderr = new EventEmitter();
        process.nextTick(() => {
          emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon'));
          emitter.emit('close', 1, null);
        });
        return emitter;
      };

      const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
      const res = await runCompilerContainer({
        srcDir: paths.srcDir,
        outDir: paths.outDir,
        language: 'c',
        _internalTestOverrides: { customSpawn: mockDaemonDown, allowTestFallback: true },
        customConfig: { allowTestFallback: true }
      });

      assert.equal(res.success, false);
      assert.equal(res.errorCategory, 'DOCKER_UNAVAILABLE');
    } finally {
      process.env.NODE_ENV = origEnv;
    }
  });

  // ---------------------------------------------------------------------------
  // 4. No production code path invokes host-side compiler executables
  // ---------------------------------------------------------------------------
  it('should verify that no production code path invokes host-side compiler executables', async () => {
    const origEnv = process.env.NODE_ENV;
    try {
      process.env.NODE_ENV = 'production';

      // 4a. Calling runTool directly in production throws
      assert.throws(
        () => runTool('clang-18', ['-v']),
        /strictly prohibited in production mode/
      );

      // 4b. HTTP compile request in production routes exclusively to Docker
      let executedBinary = null;
      let executedArgs = [];
      setCompilerCustomSpawn((bin, args) => {
        executedBinary = bin;
        executedArgs = args;
        const emitter = new EventEmitter();
        emitter.stdout = new EventEmitter();
        emitter.stderr = new EventEmitter();
        process.nextTick(() => {
          emitter.stdout.emit('data', Buffer.from('=== COMPILATION_SUCCESS ===\n'));
          emitter.emit('close', 0, null);
        });
        return emitter;
      });

      try {
        const compileRes = await request(app)
          .post('/api/compile')
          .send({
            sourceCode: 'int main() { return 0; }',
            language: 'c',
            allowTestFallback: true,
            customConfig: { allowTestFallback: true, clangTool: 'malicious-clang' }
          });

        assert.equal(compileRes.status, 200);
        assert.equal(executedBinary, 'docker');
        assert.ok(executedArgs.includes('run'));
        assert.ok(executedArgs.includes('--network'));
        assert.ok(executedArgs.includes('none'));
        assert.ok(executedArgs.includes('--read-only'));
        assert.ok(executedArgs.includes('--cap-drop'));
        assert.ok(executedArgs.includes('ALL'));
        assert.equal(executedBinary !== 'clang-18', true);
        assert.equal(executedBinary !== 'opt-18', true);
      } finally {
        resetCompilerCustomSpawn();
      }
    } finally {
      process.env.NODE_ENV = origEnv;
    }
  });

  // ---------------------------------------------------------------------------
  // 5. Artifact and Workspace Sanitization Audit
  // ---------------------------------------------------------------------------
  it('should ensure failed or cancelled compilation leaves zero stale executable artifacts', async () => {
    // 5a. Verify writeSource proactively purges any existing binary artifacts
    const paths = writeSource(testWorkspace, 'int main() { return 0; }', 'c');
    fs.writeFileSync(paths.executableFile, 'stale binary data', 'utf8');
    fs.writeFileSync(paths.outExecutableFile, 'stale out binary data', 'utf8');
    assert.equal(fs.existsSync(paths.executableFile), true);
    assert.equal(fs.existsSync(paths.outExecutableFile), true);

    writeSource(testWorkspace, 'int main() { return 1; }', 'c');
    assert.equal(fs.existsSync(paths.executableFile), false);
    assert.equal(fs.existsSync(paths.outExecutableFile), false);

    // 5b. Verify compilation failure cleans up any partial output artifacts in the job workspace
    setCompilerCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('syntax error\n=== STAGE_FAILED: FRONTEND_COMPILATION ==='));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await compileSource({
      sourceCode: 'int main() { syntax error; }',
      language: 'c'
    });

    assert.equal(res.success, false);
    assert.ok(res.jobId);

    const workspaceRoot = getCompilerConfig().workspaceRoot;
    const jobWorkspaceDir = path.join(workspaceRoot, res.jobId);
    const exePath = path.join(jobWorkspaceDir, 'program.bin');
    const outExePath = path.join(jobWorkspaceDir, 'out', 'program.bin');

    assert.equal(fs.existsSync(exePath), false);
    assert.equal(fs.existsSync(outExePath), false);

    // Clean up created job workspace
    cleanupWorkspace(jobWorkspaceDir);
  });
});
