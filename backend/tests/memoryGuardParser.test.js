import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { parseExecutionOutput } from '../src/services/execution/memoryGuardParser.js';

describe('MemoryGuard Diagnostic Parser (memoryGuardParser.js)', () => {
  it('should parse an out-of-bounds heap violation correctly', () => {
    const stderr = `
Some standard runtime debug output...
MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d8a920, access_size=4 bytes
Aborted
`;
    const result = parseExecutionOutput({
      exitCode: 134,
      signal: 'SIGABRT',
      stdout: '',
      stderr,
      durationMs: 45
    });

    assert.equal(result.status, 'violation_detected');
    assert.ok(result.violation);
    assert.equal(result.violation.detected, true);
    assert.equal(result.violation.violatingPointer, '0x55d8a920');
    assert.equal(result.violation.accessSizeBytes, 4);
    assert.ok(result.violation.rawMessage.includes('Invalid access to ptr=0x55d8a920'));
    assert.equal(result.exitCode, 134);
    assert.equal(result.terminationSignal, 'SIGABRT');
  });

  it('should parse a use-after-free heap violation correctly', () => {
    const stderr = `
MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x7ffe1200, access_size=8 bytes
`;
    const result = parseExecutionOutput({
      exitCode: 134,
      signal: 'SIGABRT',
      stdout: 'Program starting...\n',
      stderr,
      durationMs: 30
    });

    assert.equal(result.status, 'violation_detected');
    assert.equal(result.violation.violatingPointer, '0x7ffe1200');
    assert.equal(result.violation.accessSizeBytes, 8);
    assert.equal(result.stdout, 'Program starting...\n');
  });

  it('should report no_violations_detected on clean zero exit', () => {
    const result = parseExecutionOutput({
      exitCode: 0,
      signal: null,
      stdout: 'All computations succeeded!\n',
      stderr: '',
      durationMs: 25
    });

    assert.equal(result.status, 'no_violations_detected');
    assert.equal(result.violation, null);
    assert.equal(result.exitCode, 0);
    assert.equal(result.terminationSignal, null);
  });

  it('should report no_violations_detected for normal non-zero program exit', () => {
    const result = parseExecutionOutput({
      exitCode: 42,
      signal: null,
      stdout: 'Exiting with code 42 intentionally\n',
      stderr: '',
      durationMs: 15
    });

    assert.equal(result.status, 'no_violations_detected');
    assert.equal(result.violation, null);
    assert.equal(result.exitCode, 42);
  });

  it('should report execution_failed on timeout', () => {
    const result = parseExecutionOutput({
      exitCode: null,
      signal: 'SIGKILL',
      stdout: 'Starting infinite loop...',
      stderr: '',
      durationMs: 5000,
      timedOut: true
    });

    assert.equal(result.status, 'execution_failed');
    assert.equal(result.timedOut, true);
    assert.equal(result.violation, null);
  });

  it('should report output_truncated when output exceeds limits', () => {
    const result = parseExecutionOutput({
      exitCode: 0,
      stdout: 'Truncated logs...',
      stderr: '',
      durationMs: 100,
      outputTruncated: true
    });

    assert.equal(result.status, 'output_truncated');
    assert.equal(result.outputTruncated, true);
  });

  it('should report execution_failed on abnormal signal termination without MemoryGuard message', () => {
    const result = parseExecutionOutput({
      exitCode: 139,
      signal: 'SIGSEGV',
      stdout: '',
      stderr: 'Segmentation fault (core dumped)\n',
      durationMs: 20
    });

    assert.equal(result.status, 'execution_failed');
    assert.equal(result.violation, null);
    assert.equal(result.terminationSignal, 'SIGSEGV');
  });
});
