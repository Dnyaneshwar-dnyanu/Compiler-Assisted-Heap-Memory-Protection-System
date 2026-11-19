import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import request from 'supertest';
import app from '../../src/app.js';
import { checkCompilerDockerAvailability, getCompilerConfig } from '../../src/services/compiler/compilerConfig.js';
import { checkDockerAvailability as checkExecutionDockerAvailability } from '../../src/services/execution/executionConfig.js';
import { cleanupWorkspace } from '../../src/services/compiler/workspaceManager.js';

describe('Compile-Time Heap Memory Violation Diagnostics', () => {
  const compilerCfg = getCompilerConfig();
  function cleanupJob(jobId) {
    if (jobId) {
      cleanupWorkspace(path.join(compilerCfg.workspaceRoot, jobId));
    }
  }

  async function checkDockerOrSkip(t) {
    const [cStatus, eStatus] = await Promise.all([
      checkCompilerDockerAvailability(),
      checkExecutionDockerAvailability()
    ]);
    if (!cStatus.available || !eStatus.available) {
      t.skip(
        `Skipping compile-time diagnostics tests: Docker daemon is unavailable (${
          cStatus.reason || eStatus.reason || 'daemon unreachable'
        }).`
      );
      return false;
    }
    return true;
  }

  // 1. Constant-index heap out-of-bounds write
  it('should detect constant-index heap out-of-bounds write at compile time and block compilation', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const oobWriteSource = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *arr = (int *)malloc(3 * sizeof(int));
    arr[3] = 40;
    free(arr);
    return 0;
}
`.trim();

      const res = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: oobWriteSource })
        .expect(422);

      assert.equal(res.body.success, false);
      assert.equal(res.body.error.code, 'STATIC_MEMORY_VIOLATION');
      assert.equal(res.body.error.stage, 'INSTRUMENTATION');
      jobId = res.body.error.jobId;

      const violations = res.body.error.staticViolations;
      assert.ok(Array.isArray(violations) && violations.length > 0, 'Must return static violations array');

      const v = violations[0];
      assert.equal(v.errorType, 'STATIC_HEAP_OUT_OF_BOUNDS_WRITE');
      assert.equal(v.severity, 'error');
      assert.equal(v.accessType, 'write');
      assert.equal(v.allocationSize, 12);
      assert.equal(v.accessSize, 4);
      assert.equal(v.offset, 12);
      assert.equal(v.bytesOutOfBounds, 4);
      assert.ok(v.message.includes('Definite heap out-of-bounds write'));

      // Verify no executable or manifest was created
      if (jobId) {
        const wsDir = path.join(compilerCfg.workspaceRoot, jobId);
        assert.equal(fs.existsSync(path.join(wsDir, 'program.bin')), false, 'program.bin must not exist');
        assert.equal(fs.existsSync(path.join(wsDir, 'out', 'program.bin')), false, 'out/program.bin must not exist');
        assert.equal(fs.existsSync(path.join(wsDir, 'manifest.json')), false, 'manifest.json must not exist');
      }
    } finally {
      cleanupJob(jobId);
    }
  });

  // 2. Constant-index heap out-of-bounds read
  it('should detect constant-index heap out-of-bounds read at compile time and block compilation', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const oobReadSource = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *arr = (int *)malloc(3 * sizeof(int));
    int val = arr[3];
    printf("%d\\n", val);
    free(arr);
    return 0;
}
`.trim();

      const res = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: oobReadSource })
        .expect(422);

      assert.equal(res.body.success, false);
      assert.equal(res.body.error.code, 'STATIC_MEMORY_VIOLATION');
      jobId = res.body.error.jobId;

      const violations = res.body.error.staticViolations;
      assert.ok(Array.isArray(violations) && violations.length > 0);

      const v = violations[0];
      assert.equal(v.errorType, 'STATIC_HEAP_OUT_OF_BOUNDS_READ');
      assert.equal(v.accessType, 'read');
      assert.equal(v.allocationSize, 12);
      assert.equal(v.accessSize, 4);
      assert.equal(v.offset, 12);
      assert.ok(v.message.includes('Definite heap out-of-bounds read'));
    } finally {
      cleanupJob(jobId);
    }
  });

  // 3. Valid heap array accesses
  it('should compile valid heap accesses without static errors and allow execution', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const validSource = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *arr = (int *)malloc(3 * sizeof(int));
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    int sum = arr[0] + arr[1] + arr[2];
    printf("Sum: %d\\n", sum);
    free(arr);
    return 0;
}
`.trim();

      const compRes = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: validSource })
        .expect(200);

      assert.equal(compRes.body.success, true);
      assert.equal(compRes.body.data.status, 'completed');
      jobId = compRes.body.data.jobId;

      // Execute program
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId })
        .expect(200);

      assert.equal(execRes.body.success, true);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 0);
      assert.ok(execRes.body.data.execution.stdout.includes('Sum: 60'));
    } finally {
      cleanupJob(jobId);
    }
  });

  // 4. Dynamic index that cannot be statically resolved
  it('should not report compile-time errors for dynamic indices that cannot be statically resolved', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const dynamicSource = `
#include <stdio.h>
#include <stdlib.h>

int get_idx(void) { return 1; }

int main(void) {
    int *arr = (int *)malloc(3 * sizeof(int));
    int i = get_idx();
    arr[i] = 100;
    printf("Val: %d\\n", arr[i]);
    free(arr);
    return 0;
}
`.trim();

      const compRes = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: dynamicSource })
        .expect(200);

      assert.equal(compRes.body.success, true);
      jobId = compRes.body.data.jobId;

      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId })
        .expect(200);

      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 0);
      assert.ok(execRes.body.data.execution.stdout.includes('Val: 100'));
    } finally {
      cleanupJob(jobId);
    }
  });

  // 5. Pointer arithmetic with known offsets
  it('should detect out-of-bounds write via pointer arithmetic with known offsets', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const ptrArithSource = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *arr = (int *)malloc(4 * sizeof(int)); // 16 bytes
    int *p = arr + 2;                          // offset 8 bytes
    p[2] = 99;                                 // offset 8 + 8 = 16 bytes (past 16-byte buffer)
    free(arr);
    return 0;
}
`.trim();

      const res = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: ptrArithSource })
        .expect(422);

      assert.equal(res.body.success, false);
      assert.equal(res.body.error.code, 'STATIC_MEMORY_VIOLATION');
      jobId = res.body.error.jobId;

      const v = res.body.error.staticViolations[0];
      assert.equal(v.errorType, 'STATIC_HEAP_OUT_OF_BOUNDS_WRITE');
      assert.equal(v.allocationSize, 16);
      assert.equal(v.offset, 16);
      assert.equal(v.accessSize, 4);
      assert.equal(v.bytesOutOfBounds, 4);
    } finally {
      cleanupJob(jobId);
    }
  });

  // 6. Correct source-line mapping
  it('should map the static violation to the exact source line and statement', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      const source = [
        '#include <stdlib.h>',
        'int main(void) {',
        '    int *arr = (int *)malloc(2 * sizeof(int));',
        '    arr[5] = 999;', // Line 4
        '    free(arr);',
        '    return 0;',
        '}'
      ].join('\n');

      const res = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: source })
        .expect(422);

      jobId = res.body.error.jobId;
      const v = res.body.error.staticViolations[0];
      assert.equal(v.line, 4, 'Violation must point to line 4');
      assert.ok(v.statement.includes('arr[5] = 999;'), `Statement must contain the offending line code: ${v.statement}`);
    } finally {
      cleanupJob(jobId);
    }
  });

  // 7. Stale executable prevention after failed compilation
  it('should prevent execution of previous binaries when compilation fails with static memory violation', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId1 = null;
    let jobId2 = null;
    try {
      // 1. Compile valid program first
      const validSource = 'int main(void) { return 0; }';
      const res1 = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: validSource })
        .expect(200);
      jobId1 = res1.body.data.jobId;

      // 2. Attempt compilation of invalid code
      const invalidSource = `
#include <stdlib.h>
int main(void) {
    int *arr = malloc(4);
    arr[2] = 10;
    return 0;
}
`.trim();
      const res2 = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: invalidSource })
        .expect(422);
      jobId2 = res2.body.error.jobId;

      // 3. Execution of jobId2 must fail with 404 (executable not found / unmanifested)
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId: jobId2 })
        .expect(404);

      assert.equal(execRes.body.success, false);
      assert.equal(execRes.body.error.code, 'EXECUTABLE_NOT_FOUND');
    } finally {
      cleanupJob(jobId1);
      cleanupJob(jobId2);
    }
  });

  // 8. Runtime detection still works for dynamically determined violations
  it('should fall through to runtime MemoryGuard enforcement when violation cannot be determined statically', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    try {
      // Index is computed through volatile pointer so compiler cannot statically prove it
      const dynamicOobSource = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *arr = (int *)malloc(3 * sizeof(int));
    int offset = 3;
    volatile int *v = &offset;
    arr[*v] = 42; // Dynamic out of bounds
    free(arr);
    return 0;
}
`.trim();

      // Compilation must succeed because compiler cannot prove violation statically
      const compRes = await request(app)
        .post('/api/compile')
        .send({ language: 'c', sourceCode: dynamicOobSource })
        .expect(200);

      assert.equal(compRes.body.success, true);
      jobId = compRes.body.data.jobId;

      // Execution must catch the violation at runtime via SIGABRT (134)
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId })
        .expect(200);

      assert.equal(execRes.body.data.status, 'violation_detected');
      assert.equal(execRes.body.data.execution.exitCode, 134);
      assert.ok(execRes.body.data.diagnostics.violation, 'Runtime must detect the violation');
      assert.equal(execRes.body.data.diagnostics.violation.detected, true);
      assert.ok(execRes.body.data.diagnostics.violation.violatingPointer);
      assert.equal(execRes.body.data.diagnostics.violation.accessSizeBytes, 4);
    } finally {
      cleanupJob(jobId);
    }
  });
});
