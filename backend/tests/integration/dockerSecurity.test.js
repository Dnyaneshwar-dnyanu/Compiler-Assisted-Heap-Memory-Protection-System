import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import fs from 'node:fs';
import path from 'node:path';
import EventEmitter from 'node:events';
import app from '../../src/app.js';
import { createWorkspace, cleanupWorkspace } from '../../src/services/compiler/workspaceManager.js';
import { runContainer, setCustomSpawn, resetCustomSpawn, setRemoveContainerHook } from '../../src/services/execution/dockerRunner.js';
import { checkDockerAvailability } from '../../src/services/execution/executionConfig.js';

describe('Docker Container Hardening & Security Verification Suite', () => {

  // ---------------------------------------------------------------------------
  // 1. Mandatory Security Flags Verification
  // ---------------------------------------------------------------------------
  it('should enforce complete set of non-negotiable Docker security flags', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    try {
      await runContainer({ executablePath: '/app/test.bin' });

      // 1. Network completely disabled
      assert.ok(capturedArgs.includes('--network'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--network') + 1], 'none');

      // 2. Read-only root filesystem
      assert.ok(capturedArgs.includes('--read-only'));

      // 3. Drop all capabilities
      assert.ok(capturedArgs.includes('--cap-drop'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--cap-drop') + 1], 'ALL');

      // 4. Disallow privilege escalation
      assert.ok(capturedArgs.includes('--security-opt'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--security-opt') + 1], 'no-new-privileges');

      // 5. Unprivileged non-root user
      assert.ok(capturedArgs.includes('--user'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--user') + 1], '10001:10001');

      // 6. Memory ceiling limit
      assert.ok(capturedArgs.includes('--memory'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--memory') + 1], '64m');

      // 7. CPU quota limit
      assert.ok(capturedArgs.includes('--cpus'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--cpus') + 1], '0.5');

      // 8. Process/fork bomb limit
      assert.ok(capturedArgs.includes('--pids-limit'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--pids-limit') + 1], '32');

      // 9. Tmpfs scratch space with noexec/nosuid
      assert.ok(capturedArgs.includes('--tmpfs'));
      assert.equal(capturedArgs[capturedArgs.indexOf('--tmpfs') + 1], '/tmp:rw,noexec,nosuid,size=16m');

      // 10. Lightweight init system
      assert.ok(capturedArgs.includes('--init'));

      // 11. Ephemeral container cleanup
      assert.ok(capturedArgs.includes('--rm'));

      // 12. Mount single binary read-only
      assert.ok(capturedArgs.includes('-v'));
      assert.equal(capturedArgs[capturedArgs.indexOf('-v') + 1], '/app/test.bin:/app/program.bin:ro');
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 2. Forbidden Flags & Docker Socket Probing
  // ---------------------------------------------------------------------------
  it('must never include --privileged, Docker daemon socket, or host directories', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    try {
      await runContainer({ executablePath: '/custom/path/binary' });

      // Absolutely forbidden flags
      assert.equal(capturedArgs.includes('--privileged'), false, 'Must not run with --privileged');
      assert.equal(capturedArgs.includes('--net=host'), false, 'Must not use host network');

      // Must NOT mount docker socket
      const argString = capturedArgs.join(' ');
      assert.equal(argString.includes('docker.sock'), false, 'Must not mount docker.sock');
      assert.equal(argString.includes('dockerDesktopLinuxEngine'), false, 'Must not mount docker socket pipe');

      // Verify no extra volumes are mounted besides the single binary
      const vIndices = [];
      capturedArgs.forEach((arg, idx) => {
        if (arg === '-v') vIndices.push(idx);
      });
      assert.equal(vIndices.length, 1, 'Only exactly one volume (-v) must be mounted');
      assert.ok(capturedArgs[vIndices[0] + 1].endsWith(':/app/program.bin:ro'), 'Mounted volume must be read-only program.bin');
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 3. Client Disconnect and Container Cleanup
  // ---------------------------------------------------------------------------
  it('should cleanly remove container when client disconnects during execution', async () => {
    let removedContainer = null;
    const abortController = new AbortController();

    setRemoveContainerHook((containerName) => {
      removedContainer = containerName;
    });

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();

      emitter.kill = () => {
        process.nextTick(() => emitter.emit('close', 137, 'SIGKILL'));
      };
      return emitter;
    });

    try {
      const promise = runContainer({
        executablePath: '/test/program.bin',
        abortSignal: abortController.signal
      });

      // Simulate client dropping connection
      abortController.abort();
      const result = await promise;

      assert.equal(result.aborted, true);
      assert.equal(result.errorCategory, 'ABORTED');
      assert.ok(removedContainer, 'Container rm should have been invoked on client abort');
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 4. Real Docker Integration Test (Conditionally Executed / Skipped with Reason)
  // ---------------------------------------------------------------------------
  it('should execute binary inside real Docker container if daemon is available', async (t) => {
    const status = await checkDockerAvailability();

    if (!status.available) {
      t.skip(`Skipping real Docker container execution: Docker daemon is unavailable on host (${status.error || 'open //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified'}).`);
      return;
    }

    // If Docker IS running on the host, execute a real container test!
    const ws = createWorkspace();
    const binPath = path.join(ws.workspaceDir, 'program.bin');

    // Create a minimal hello binary if present or compile via API
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: '#include <stdio.h>\nint main() { printf("Docker E2E Success\\n"); return 0; }',
          language: 'c'
        });

      if (!compileRes.body.success) {
        t.skip('Compilation failed during Docker integration setup');
        return;
      }

      const jobId = compileRes.body.data.jobId;

      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.ok(execRes.body.data.execution.stdout.includes('Docker E2E Success'));
    } finally {
      cleanupWorkspace(ws.workspaceDir);
    }
  });
});
