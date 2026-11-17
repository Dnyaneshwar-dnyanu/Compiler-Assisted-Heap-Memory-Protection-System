import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import fs from 'node:fs';
import path from 'node:path';
import EventEmitter from 'node:events';
import app from '../../src/app.js';
import { createWorkspace, cleanupWorkspace } from '../../src/services/compiler/workspaceManager.js';
import {
  runContainer,
  setCustomSpawn,
  resetCustomSpawn,
  setRemoveContainerHook,
  IMMUTABLE_SECURITY
} from '../../src/services/execution/dockerRunner.js';

describe('Docker Security Lockdown & Hardened Tamper Resistance (Step 24)', () => {
  let testWorkspace;

  beforeEach(() => {
    testWorkspace = createWorkspace();
    // Create dummy program.bin in the workspace
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy ELF binary for security testing');
  });

  afterEach(() => {
    resetCustomSpawn();
    if (testWorkspace?.workspaceDir) {
      cleanupWorkspace(testWorkspace.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 1. Immutable Security Constants Integrity
  // ---------------------------------------------------------------------------
  it('should expose strictly frozen IMMUTABLE_SECURITY configuration', () => {
    assert.ok(Object.isFrozen(IMMUTABLE_SECURITY));
    assert.equal(IMMUTABLE_SECURITY.network, 'none');
    assert.equal(IMMUTABLE_SECURITY.readOnly, true);
    assert.equal(IMMUTABLE_SECURITY.capDrop, 'ALL');
    assert.equal(IMMUTABLE_SECURITY.securityOpt, 'no-new-privileges');
    assert.equal(IMMUTABLE_SECURITY.userUidGid, '10001:10001');
    assert.equal(IMMUTABLE_SECURITY.memoryLimit, '64m');
    assert.equal(IMMUTABLE_SECURITY.cpuLimit, '0.5');
    assert.equal(IMMUTABLE_SECURITY.pidsLimit, 32);
    assert.equal(IMMUTABLE_SECURITY.tmpfsMount, '/tmp:rw,noexec,nosuid,size=16m');
    assert.equal(IMMUTABLE_SECURITY.init, true);
    assert.equal(IMMUTABLE_SECURITY.rm, true);

    // Attempting to mutate must fail or be a no-op
    assert.throws(() => {
      IMMUTABLE_SECURITY.network = 'host';
    }, /Cannot assign to read only property/);
  });

  // ---------------------------------------------------------------------------
  // 2. HTTP Injection Resistance: Network Isolation
  // ---------------------------------------------------------------------------
  it('should reject network override attempt in HTTP payload and enforce --network none', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          network: 'host'
        }
      });

    assert.equal(res.status, 200);
    assert.ok(capturedArgs.includes('--network'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--network') + 1], 'none');
    assert.equal(capturedArgs.includes('host'), false);
  });

  // ---------------------------------------------------------------------------
  // 3. HTTP Injection Resistance: Root / User Privilege Tampering
  // ---------------------------------------------------------------------------
  it('should reject user override attempt in HTTP payload and enforce unprivileged user', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          userUidGid: '0:0',
          user: 'root'
        }
      });

    assert.equal(res.status, 200);
    assert.ok(capturedArgs.includes('--user'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--user') + 1], '10001:10001');
    assert.equal(capturedArgs.includes('0:0'), false);
    assert.equal(capturedArgs.includes('root'), false);
  });

  // ---------------------------------------------------------------------------
  // 4. HTTP Injection Resistance: Read-Only Filesystem Tampering
  // ---------------------------------------------------------------------------
  it('should reject readOnly: false in HTTP payload and enforce --read-only rootfs', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          readOnly: false
        }
      });

    assert.equal(res.status, 200);
    assert.ok(capturedArgs.includes('--read-only'), 'Must enforce --read-only flag');
  });

  // ---------------------------------------------------------------------------
  // 5. HTTP Injection Resistance: Capability Dropping & Security Opts
  // ---------------------------------------------------------------------------
  it('should reject capDrop and securityOpt overrides in HTTP payload', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          capDrop: 'NONE',
          securityOpt: 'unconfined'
        }
      });

    assert.equal(res.status, 200);
    assert.ok(capturedArgs.includes('--cap-drop'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--cap-drop') + 1], 'ALL');
    assert.ok(capturedArgs.includes('--security-opt'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--security-opt') + 1], 'no-new-privileges');
    assert.equal(capturedArgs.includes('NONE'), false);
    assert.equal(capturedArgs.includes('unconfined'), false);
  });

  // ---------------------------------------------------------------------------
  // 6. HTTP Injection Resistance: Privileged Mode Execution
  // ---------------------------------------------------------------------------
  it('should never include --privileged flag even when explicitly requested in HTTP payload', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          privileged: true,
          '--privileged': true
        }
      });

    assert.equal(res.status, 200);
    assert.equal(capturedArgs.includes('--privileged'), false, 'Must never include --privileged');
  });

  // ---------------------------------------------------------------------------
  // 7. HTTP Injection Resistance: Memory, CPU, and PID Resource Quotas
  // ---------------------------------------------------------------------------
  it('should reject resource quota inflation attempts and enforce baseline ceilings', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          memoryLimit: '32g',
          cpuLimit: '16.0',
          pidsLimit: 99999
        }
      });

    assert.equal(res.status, 200);
    assert.ok(capturedArgs.includes('--memory'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--memory') + 1], '64m');

    assert.ok(capturedArgs.includes('--cpus'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--cpus') + 1], '0.5');

    assert.ok(capturedArgs.includes('--pids-limit'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--pids-limit') + 1], '32');

    assert.equal(capturedArgs.includes('32g'), false);
    assert.equal(capturedArgs.includes('16.0'), false);
    assert.equal(capturedArgs.includes('99999'), false);
  });

  // ---------------------------------------------------------------------------
  // 8. HTTP Injection Resistance: Volume & Socket Mount Hijacking
  // ---------------------------------------------------------------------------
  it('should reject arbitrary volume mounts and enforce single read-only binary mount', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: {
          mounts: [
            '/etc:/etc:rw',
            '/var/run/docker.sock:/var/run/docker.sock:rw'
          ],
          volumeMount: '/:/host:rw'
        }
      });

    assert.equal(res.status, 200);

    // Verify exactly one volume argument is passed
    const vIndices = [];
    capturedArgs.forEach((arg, idx) => {
      if (arg === '-v') vIndices.push(idx);
    });
    assert.equal(vIndices.length, 1, 'Only exactly one volume mount is permitted');
    assert.ok(capturedArgs[vIndices[0] + 1].endsWith(':/app/program.bin:ro'));
    assert.equal(capturedArgs.join(' ').includes('docker.sock'), false);
    assert.equal(capturedArgs.join(' ').includes('/etc:'), false);
  });

  // ---------------------------------------------------------------------------
  // 9. Direct Runner Invocation Tamper Resistance
  // ---------------------------------------------------------------------------
  it('should enforce immutable security bounds even when customConfig is passed directly to runContainer', async () => {
    let capturedArgs = [];

    setCustomSpawn((cmd, args = []) => {
      if (args[0] === 'run') {
        capturedArgs = [...args];
      }
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    // Malicious caller directly calls runContainer with security overrides
    await runContainer({
      executablePath: '/safe/app/program.bin',
      customConfig: {
        network: 'host',
        userUidGid: '0:0',
        memoryLimit: '128g',
        cpuLimit: '32.0',
        pidsLimit: 50000,
        readOnly: false,
        capDrop: 'NONE',
        securityOpt: 'unconfined',
        tmpfsMount: '/tmp:rw,exec,suid'
      }
    });

    // All security settings must remain completely unchanged
    assert.equal(capturedArgs[capturedArgs.indexOf('--network') + 1], 'none');
    assert.equal(capturedArgs[capturedArgs.indexOf('--user') + 1], '10001:10001');
    assert.equal(capturedArgs[capturedArgs.indexOf('--memory') + 1], '64m');
    assert.equal(capturedArgs[capturedArgs.indexOf('--cpus') + 1], '0.5');
    assert.equal(capturedArgs[capturedArgs.indexOf('--pids-limit') + 1], '32');
    assert.ok(capturedArgs.includes('--read-only'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--cap-drop') + 1], 'ALL');
    assert.equal(capturedArgs[capturedArgs.indexOf('--security-opt') + 1], 'no-new-privileges');
    assert.equal(capturedArgs[capturedArgs.indexOf('--tmpfs') + 1], '/tmp:rw,noexec,nosuid,size=16m');
    assert.equal(capturedArgs.includes('--privileged'), false);
  });

  // ---------------------------------------------------------------------------
  // 10. Client Disconnect and Container Cleanup With Tamper Payload
  // ---------------------------------------------------------------------------
  it('should abort cleanly and trigger container removal even when tamper payload is sent', async () => {
    let removedContainer = null;
    const abortController = new AbortController();

    setRemoveContainerHook((name) => {
      removedContainer = name;
    });

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {
        process.nextTick(() => emitter.emit('close', 137, 'SIGKILL'));
      };
      return emitter;
    });

    const promise = runContainer({
      executablePath: '/test/program.bin',
      customConfig: { network: 'host' },
      abortSignal: abortController.signal
    });

    // Trigger abort
    abortController.abort();
    const result = await promise;

    assert.equal(result.aborted, true);
    assert.equal(result.errorCategory, 'ABORTED');
    assert.ok(removedContainer, 'Container rm should have been invoked on abort');
  });

  // ---------------------------------------------------------------------------
  // 11. Normal Execution Success with Extraneous Body Fields
  // ---------------------------------------------------------------------------
  it('should ignore all extraneous body fields and execute normally with standard envelope', async () => {
    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};
      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from('Safe execution complete\n'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId,
        customConfig: { network: 'host', privileged: true },
        extraneousPayload: 'ignored',
        __proto__: { polluted: true }
      });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'no_violations_detected');
    assert.equal(res.body.data.execution.stdout, 'Safe execution complete\n');
    assert.equal(res.body.data.diagnostics.violation, null);
  });
});
