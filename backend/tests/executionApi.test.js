import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { EventEmitter } from 'node:events';
import request from 'supertest';
import app from '../src/app.js';
import { createWorkspace, writeSource, cleanupWorkspace } from '../src/services/compiler/workspaceManager.js';
import { setCustomSpawn, resetCustomSpawn, setRunnerTestOverrides } from '../src/services/execution/dockerRunner.js';

describe('POST /api/execute (Runtime Execution Sandbox API)', () => {
  let testWorkspace;

  beforeEach(() => {
    testWorkspace = createWorkspace();
  });

  afterEach(() => {
    resetCustomSpawn();
    if (testWorkspace?.workspaceDir) {
      cleanupWorkspace(testWorkspace.workspaceDir);
    }
  });

  it('should reject missing jobId with 400 INVALID_JOB_ID', async () => {
    const res = await request(app)
      .post('/api/execute')
      .send({});

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'INVALID_JOB_ID');
    assert.ok(res.body.error.message.includes('jobId'));
  });

  it('should reject invalid UUID format with 400 INVALID_JOB_ID', async () => {
    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: 'not-a-valid-uuid-123' });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'INVALID_JOB_ID');
    assert.ok(res.body.error.message.includes('Invalid Job ID format'));
  });

  it('should reject path traversal in jobId with 400 INVALID_JOB_ID', async () => {
    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: '../../etc/passwd' });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'INVALID_JOB_ID');
  });

  it('should return 404 for unknown job UUID', async () => {
    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: '00000000-0000-4000-8000-000000000000' });

    assert.equal(res.status, 404);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'JOB_NOT_FOUND');
    assert.ok(res.body.error.message.includes('does not exist'));
  });

  it('should return 404 when workspace exists but program.bin is missing', async () => {
    // Workspace exists from beforeEach, but program.bin has not been compiled
    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 404);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'EXECUTABLE_NOT_FOUND');
    assert.ok(res.body.error.message.includes('program.bin'));
  });

  it('should return 503 when Docker daemon is unavailable', async () => {
    // Create dummy program.bin
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy ELF executable');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon. Is the docker daemon running?'));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 503);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'DOCKER_UNAVAILABLE');
    assert.ok(res.body.error.message.includes('Docker daemon is not running'));
  });

  it('should return 503 when execution image is missing', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy ELF executable');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('Unable to find image "memoryguard-runner:latest" locally'));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 503);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'IMAGE_NOT_FOUND');
  });

  it('should execute valid C program successfully and return no_violations_detected', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy ELF binary');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from('Hello from sandboxed MemoryGuard C program!\n'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);

    const data = res.body.data;
    assert.equal(data.jobId, testWorkspace.jobId);
    assert.equal(data.status, 'no_violations_detected');
    assert.equal(data.execution.exitCode, 0);
    assert.equal(data.execution.stdout, 'Hello from sandboxed MemoryGuard C program!\n');
    assert.equal(data.diagnostics.violation, null);

    // Verify host paths are never exposed
    const bodyStr = JSON.stringify(res.body);
    assert.equal(bodyStr.includes('D:\\'), false);
    assert.equal(bodyStr.includes('/mnt/'), false);
  });

  it('should execute valid C++ program successfully', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy C++ binary');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from('std::cout from C++ container!\n'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'no_violations_detected');
    assert.equal(res.body.data.execution.stdout, 'std::cout from C++ container!\n');
  });

  it('should detect MemoryGuard out-of-bounds heap violation', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy binary');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x5b38d38ba2c0, access_size=4 bytes\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);

    const data = res.body.data;
    assert.equal(data.status, 'violation_detected');
    assert.equal(data.execution.exitCode, 134);
    assert.equal(data.execution.terminationSignal, 'SIGABRT');

    assert.ok(data.diagnostics.violation);
    assert.equal(data.diagnostics.violation.detected, true);
    assert.equal(data.diagnostics.violation.violatingPointer, '0x5b38d38ba2c0');
    assert.equal(data.diagnostics.violation.accessSizeBytes, 4);
  });

  it('should detect MemoryGuard use-after-free violation', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy binary');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x7ffce890, access_size=8 bytes\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'violation_detected');
    assert.equal(res.body.data.diagnostics.violation.violatingPointer, '0x7ffce890');
    assert.equal(res.body.data.diagnostics.violation.accessSizeBytes, 8);
  });

  it('should handle normal nonzero program exit', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy binary');

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from('Calculation completed with non-zero status\n'));
        emitter.emit('close', 7, null);
      });
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: testWorkspace.jobId });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'no_violations_detected');
    assert.equal(res.body.data.execution.exitCode, 7);
  });

  it('should handle execution timeout on infinite loop', async () => {
    const binPath = path.join(testWorkspace.workspaceDir, 'program.bin');
    fs.writeFileSync(binPath, 'dummy binary');

    // Use internal test runner hook instead of client-controlled request body
    setRunnerTestOverrides({ executionTimeoutMs: 15 });

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args && (args.includes('rm') || args.includes('kill'))) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      emitter.kill = () => {
        process.nextTick(() => {
          emitter.emit('close', 137, 'SIGKILL');
        });
      };
      return emitter;
    });

    const res = await request(app)
      .post('/api/execute')
      .send({
        jobId: testWorkspace.jobId
      });

    assert.equal(res.status, 504);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'TIMEOUT_ERROR');
    assert.ok(res.body.error.details.timedOut);
  });

  it('should handle concurrent execution requests without interference', async () => {
    const ws2 = createWorkspace();
    fs.writeFileSync(path.join(testWorkspace.workspaceDir, 'program.bin'), 'bin1');
    fs.writeFileSync(path.join(ws2.workspaceDir, 'program.bin'), 'bin2');

    try {
      setCustomSpawn((cmd, args) => {
        const emitter = new EventEmitter();
        emitter.stdout = new EventEmitter();
        emitter.stderr = new EventEmitter();
        emitter.kill = () => {};

        // Find which binary was mounted
        const vIndex = args.indexOf('-v');
        const mount = args[vIndex + 1];

        process.nextTick(() => {
          emitter.stdout.emit('data', Buffer.from(`Output from ${mount}\n`));
          emitter.emit('close', 0, null);
        });
        return emitter;
      });

      const [res1, res2] = await Promise.all([
        request(app).post('/api/execute').send({ jobId: testWorkspace.jobId }),
        request(app).post('/api/execute').send({ jobId: ws2.jobId })
      ]);

      assert.equal(res1.status, 200);
      assert.equal(res2.status, 200);
      assert.notEqual(res1.body.data.jobId, res2.body.data.jobId);
      assert.equal(res1.body.data.status, 'no_violations_detected');
      assert.equal(res2.body.data.status, 'no_violations_detected');
    } finally {
      cleanupWorkspace(ws2.workspaceDir);
    }
  });
});
