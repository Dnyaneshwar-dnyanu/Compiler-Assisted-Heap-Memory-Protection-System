import { describe, it, beforeEach, afterEach } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import {
  runContainer,
  setCustomSpawn,
  resetCustomSpawn
} from '../src/services/execution/dockerRunner.js';

describe('Docker Process Management & Security Flags (dockerRunner.js)', () => {
  afterEach(() => {
    resetCustomSpawn();
  });

  it('should enforce all mandatory security restrictions in docker run arguments', async () => {
    let capturedArgs = [];
    let capturedCommand = '';

    setCustomSpawn((cmd, args) => {
      capturedCommand = cmd;
      capturedArgs = args;

      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const result = await runContainer({
      executablePath: '/test/path/program.bin'
    });

    assert.equal(result.success, true);
    assert.equal(capturedCommand, 'docker');

    // Verify sub-command
    assert.equal(capturedArgs[0], 'run');

    // Verify container name is unique and prefixed
    const nameIndex = capturedArgs.indexOf('--name');
    assert.ok(nameIndex !== -1);
    assert.ok(capturedArgs[nameIndex + 1].startsWith('mg-exec-'));

    // Verify mandatory security restrictions
    assert.ok(capturedArgs.includes('--network'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--network') + 1], 'none');

    assert.ok(capturedArgs.includes('--read-only'));
    assert.ok(capturedArgs.includes('--rm'));
    assert.ok(capturedArgs.includes('--init'));

    assert.ok(capturedArgs.includes('--cap-drop'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--cap-drop') + 1], 'ALL');

    assert.ok(capturedArgs.includes('--security-opt'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--security-opt') + 1], 'no-new-privileges');

    assert.ok(capturedArgs.includes('--user'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--user') + 1], '10001:10001');

    assert.ok(capturedArgs.includes('--memory'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--memory') + 1], '64m');

    assert.ok(capturedArgs.includes('--cpus'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--cpus') + 1], '0.5');

    assert.ok(capturedArgs.includes('--pids-limit'));
    assert.equal(capturedArgs[capturedArgs.indexOf('--pids-limit') + 1], '32');

    assert.ok(capturedArgs.includes('--tmpfs'));

    // Verify read-only mount of single executable
    assert.ok(capturedArgs.includes('-v'));
    assert.equal(capturedArgs[capturedArgs.indexOf('-v') + 1], '/test/path/program.bin:/app/program.bin:ro');

    // Verify forbidden flags are completely absent
    assert.equal(capturedArgs.includes('--privileged'), false);
    assert.equal(capturedArgs.includes('/var/run/docker.sock'), false);
  });

  it('should truncate excessive stdout/stderr and flag outputTruncated', async () => {
    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        // Emit 200 bytes when max is 50
        emitter.stdout.emit('data', Buffer.alloc(200, 'X'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    const result = await runContainer({
      executablePath: '/test/program.bin',
      customConfig: { maxOutputBytes: 50 }
    });

    assert.equal(result.outputTruncated, true);
    assert.equal(result.stdout.length, 50);
  });

  it('should detect Docker daemon unavailable', async () => {
    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {};

      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from('Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?'));
        emitter.emit('close', 1, null);
      });
      return emitter;
    });

    const result = await runContainer({
      executablePath: '/test/program.bin'
    });

    assert.equal(result.success, false);
    assert.equal(result.errorCategory, 'DOCKER_UNAVAILABLE');
    assert.ok(result.message.includes('daemon is not running'));
  });

  it('should detect missing execution image', async () => {
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

    const result = await runContainer({
      executablePath: '/test/program.bin'
    });

    assert.equal(result.success, false);
    assert.equal(result.errorCategory, 'IMAGE_NOT_FOUND');
    assert.ok(result.message.includes('not available locally'));
  });

  it('should handle client disconnect abort signal', async () => {
    const abortController = new AbortController();

    setCustomSpawn(() => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      emitter.kill = () => {
        emitter.emit('close', 137, 'SIGKILL');
      };
      return emitter;
    });

    const promise = runContainer({
      executablePath: '/test/program.bin',
      abortSignal: abortController.signal
    });

    // Abort execution
    abortController.abort();
    const result = await promise;

    assert.equal(result.aborted, true);
    assert.equal(result.errorCategory, 'ABORTED');
  });
});
