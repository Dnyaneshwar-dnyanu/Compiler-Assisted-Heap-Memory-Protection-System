import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import fs from 'node:fs';
import path from 'node:path';
import EventEmitter from 'node:events';
import app from '../../src/app.js';
import { getCompilerConfig } from '../../src/services/compiler/compilerConfig.js';
import { createWorkspace, cleanupWorkspace, cleanupExpiredWorkspaces } from '../../src/services/compiler/workspaceManager.js';
import { setCustomSpawn, resetCustomSpawn } from '../../src/services/execution/dockerRunner.js';

describe('API and Workspace Job Isolation Suite', () => {

  // ---------------------------------------------------------------------------
  // 1. Invalid UUID Rejection
  // ---------------------------------------------------------------------------
  it('should reject malformed UUIDs with 400 INVALID_JOB_ID', async () => {
    const invalidIds = [
      '123',
      'not-a-valid-uuid',
      '../../../etc/passwd',
      '00000000-0000-0000-0000-00000000000G', // invalid hex
      '7f3b891a-4d2c-4e89-9a1b-123456789abc/extra',
      '; rm -rf / ;'
    ];

    for (const badId of invalidIds) {
      const res = await request(app)
        .post('/api/execute')
        .send({ jobId: badId });

      assert.equal(res.status, 400, `Expected 400 for bad jobId: "${badId}"`);
      assert.equal(res.body.success, false);
      assert.equal(res.body.error.code, 'INVALID_JOB_ID');
    }
  });

  // ---------------------------------------------------------------------------
  // 2. Unknown Job Rejection
  // ---------------------------------------------------------------------------
  it('should return 404 JOB_NOT_FOUND for non-existent job UUID', async () => {
    const nonExistentUuid = 'ffffffff-ffff-4fff-8fff-ffffffffffff';
    const res = await request(app)
      .post('/api/execute')
      .send({ jobId: nonExistentUuid });

    assert.equal(res.status, 404);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'JOB_NOT_FOUND');
    assert.ok(res.body.error.message.includes(nonExistentUuid));
  });

  // ---------------------------------------------------------------------------
  // 3. Executable Missing
  // ---------------------------------------------------------------------------
  it('should return 404 EXECUTABLE_NOT_FOUND when workspace exists but program.bin is missing', async () => {
    const ws = createWorkspace();
    try {
      const res = await request(app)
        .post('/api/execute')
        .send({ jobId: ws.jobId });

      assert.equal(res.status, 404);
      assert.equal(res.body.success, false);
      assert.equal(res.body.error.code, 'EXECUTABLE_NOT_FOUND');
      assert.ok(res.body.error.message.includes('program.bin'));
    } finally {
      cleanupWorkspace(ws.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 4. Concurrent Compile Requests
  // ---------------------------------------------------------------------------
  it('should process concurrent compile requests without interference', async () => {
    const codeA = `int main(void) { return 10; }`;
    const codeB = `int main(void) { return 20; }`;
    const codeC = `int main(void) { return 30; }`;

    const [resA, resB, resC] = await Promise.all([
      request(app).post('/api/compile').send({ sourceCode: codeA, language: 'c' }),
      request(app).post('/api/compile').send({ sourceCode: codeB, language: 'c' }),
      request(app).post('/api/compile').send({ sourceCode: codeC, language: 'c' })
    ]);

    assert.equal(resA.status, 200);
    assert.equal(resB.status, 200);
    assert.equal(resC.status, 200);

    const idA = resA.body.data.jobId;
    const idB = resB.body.data.jobId;
    const idC = resC.body.data.jobId;

    // All job IDs must be unique
    assert.notEqual(idA, idB);
    assert.notEqual(idB, idC);
    assert.notEqual(idA, idC);

    // Verify all 3 executables exist in separate directories
    const cfg = getCompilerConfig();
    assert.ok(fs.existsSync(path.join(cfg.workspaceRoot, idA, 'program.bin')));
    assert.ok(fs.existsSync(path.join(cfg.workspaceRoot, idB, 'program.bin')));
    assert.ok(fs.existsSync(path.join(cfg.workspaceRoot, idC, 'program.bin')));

    // Cleanup
    cleanupWorkspace(path.join(cfg.workspaceRoot, idA));
    cleanupWorkspace(path.join(cfg.workspaceRoot, idB));
    cleanupWorkspace(path.join(cfg.workspaceRoot, idC));
  });

  // ---------------------------------------------------------------------------
  // 5. Concurrent Execution Requests
  // ---------------------------------------------------------------------------
  it('should handle concurrent execution requests on different jobs', async () => {
    const ws1 = createWorkspace();
    const ws2 = createWorkspace();

    fs.writeFileSync(path.join(ws1.workspaceDir, 'program.bin'), 'binary1');
    fs.writeFileSync(path.join(ws2.workspaceDir, 'program.bin'), 'binary2');

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }

      // Check which mount was passed
      const vIdx = args.indexOf('-v');
      const mountArg = vIdx !== -1 ? args[vIdx + 1] : '';

      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from(`Output from mount: ${mountArg}\n`));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    try {
      const [exec1, exec2] = await Promise.all([
        request(app).post('/api/execute').send({ jobId: ws1.jobId }),
        request(app).post('/api/execute').send({ jobId: ws2.jobId })
      ]);

      assert.equal(exec1.status, 200);
      assert.equal(exec2.status, 200);
      assert.equal(exec1.body.data.jobId, ws1.jobId);
      assert.equal(exec2.body.data.jobId, ws2.jobId);

      // Verify each received its own mounted path
      assert.ok(exec1.body.data.execution.stdout.includes(ws1.jobId));
      assert.ok(exec2.body.data.execution.stdout.includes(ws2.jobId));
    } finally {
      resetCustomSpawn();
      cleanupWorkspace(ws1.workspaceDir);
      cleanupWorkspace(ws2.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 6. No Arbitrary Executable Path Accepted from Client
  // ---------------------------------------------------------------------------
  it('should ignore arbitrary client executable paths and strictly resolve via jobId', async () => {
    const ws = createWorkspace();
    fs.writeFileSync(path.join(ws.workspaceDir, 'program.bin'), 'real binary');

    let capturedMount = null;
    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }

      const vIdx = args.indexOf('-v');
      if (vIdx !== -1) {
        capturedMount = args[vIdx + 1];
      }

      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    });

    try {
      // Pass a malicious path in the request body trying to execute /bin/sh or cmd.exe
      const res = await request(app)
        .post('/api/execute')
        .send({
          jobId: ws.jobId,
          executablePath: '/bin/bash',
          command: 'rm -rf /'
        });

      assert.equal(res.status, 200);
      // The mount MUST be the workspace program.bin, never the injected path
      assert.ok(capturedMount.includes(ws.jobId));
      assert.ok(capturedMount.includes('program.bin'));
      assert.equal(capturedMount.includes('/bin/bash'), false);
    } finally {
      resetCustomSpawn();
      cleanupWorkspace(ws.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 7. Repeated Execution Behavior
  // ---------------------------------------------------------------------------
  it('should allow executing the same job multiple times consistently', async () => {
    const ws = createWorkspace();
    fs.writeFileSync(path.join(ws.workspaceDir, 'program.bin'), 'persistent binary');

    let runCount = 0;
    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      runCount++;
      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from(`Run ${runCount}\n`));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    try {
      const res1 = await request(app).post('/api/execute').send({ jobId: ws.jobId });
      const res2 = await request(app).post('/api/execute').send({ jobId: ws.jobId });

      assert.equal(res1.status, 200);
      assert.equal(res2.status, 200);
      assert.equal(res1.body.data.execution.stdout.trim(), 'Run 1');
      assert.equal(res2.body.data.execution.stdout.trim(), 'Run 2');

      // Binary must still exist intact
      assert.ok(fs.existsSync(path.join(ws.workspaceDir, 'program.bin')));
    } finally {
      resetCustomSpawn();
      cleanupWorkspace(ws.workspaceDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 8. Expired Workspace Cleanup
  // ---------------------------------------------------------------------------
  it('should clean up expired workspaces older than threshold without deleting active ones', () => {
    const oldWs = createWorkspace();
    const activeWs = createWorkspace();

    try {
      // Artificially age the old workspace directory mtime
      const pastTime = new Date(Date.now() - 7200000); // 2 hours ago
      fs.utimesSync(oldWs.workspaceDir, pastTime, pastTime);

      // Run cleanup with 1 hour threshold (3600000 ms)
      const cleaned = cleanupExpiredWorkspaces(3600000);

      assert.ok(cleaned >= 1);
      assert.equal(fs.existsSync(oldWs.workspaceDir), false, 'Old workspace should have been deleted');
      assert.equal(fs.existsSync(activeWs.workspaceDir), true, 'Active workspace should remain intact');
    } finally {
      cleanupWorkspace(oldWs.workspaceDir);
      cleanupWorkspace(activeWs.workspaceDir);
    }
  });
});
