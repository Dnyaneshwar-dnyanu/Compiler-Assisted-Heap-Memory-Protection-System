import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import request from 'supertest';
import app from '../../src/app.js';
import { checkCompilerDockerAvailability, getCompilerConfig } from '../../src/services/compiler/compilerConfig.js';
import { checkDockerAvailability as checkExecutionDockerAvailability } from '../../src/services/execution/executionConfig.js';
import { cleanupWorkspace } from '../../src/services/compiler/workspaceManager.js';

describe('Real Docker End-to-End Pipeline & Reliability (Step 27)', () => {
  // Helper to verify Docker daemon is online for both compiler and execution
  async function checkDockerOrSkip(t) {
    const [cStatus, eStatus] = await Promise.all([
      checkCompilerDockerAvailability(),
      checkExecutionDockerAvailability()
    ]);
    if (!cStatus.available || !eStatus.available) {
      t.skip(
        `Skipping real Docker E2E tests: Docker daemon is unavailable on host (${
          cStatus.reason || eStatus.reason || 'daemon unreachable'
        }).`
      );
      return false;
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // 1 & 2. Valid C Program Compilation & Isolated Execution
  // ---------------------------------------------------------------------------
  it('should compile valid C source with MemoryGuard instrumentation and execute in runtime container', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    const cfg = getCompilerConfig();
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *nums = (int *)malloc(3 * sizeof(int));
    if (!nums) return 1;
    nums[0] = 10;
    nums[1] = 20;
    nums[2] = 30;
    printf("C Sum: %d\\n", nums[0] + nums[1] + nums[2]);
    free(nums);
    return 0;
}
`,
          language: 'c'
        });

      assert.equal(compileRes.status, 200);
      assert.equal(compileRes.body.success, true);
      jobId = compileRes.body.data.jobId;
      assert.ok(jobId);

      // Verify stages
      assert.equal(compileRes.body.data.status, 'completed');
      assert.equal(compileRes.body.data.stages.length, 3);
      assert.equal(compileRes.body.data.artifacts.executableAvailable, true);

      // Verify textual LLVM IR artifacts were copied to output workspace
      const wsDir = path.join(cfg.workspaceRoot, jobId);
      const irPath = path.join(wsDir, 'out', 'source.ll');
      const instIrPath = path.join(wsDir, 'out', 'source_inst.ll');
      assert.ok(fs.existsSync(irPath), 'source.ll must exist in outDir');
      assert.ok(fs.existsSync(instIrPath), 'source_inst.ll must exist in outDir');
      const irContent = fs.readFileSync(irPath, 'utf8');
      const instIrContent = fs.readFileSync(instIrPath, 'utf8');
      assert.ok(instIrContent.includes('mg_check_access') || instIrContent.includes('mg_malloc'), 'Instrumented IR must contain MemoryGuard symbols');

      // Execute inside runtime container
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.success, true);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 0);
      assert.ok(execRes.body.data.execution.stdout.includes('C Sum: 60'));
      assert.equal(execRes.body.data.diagnostics.violation, null);
    } finally {
      if (jobId) cleanupWorkspace(path.join(cfg.workspaceRoot, jobId));
    }
  });

  // ---------------------------------------------------------------------------
  // 3 & 4. Valid C++ Program Compilation & Isolated Execution
  // ---------------------------------------------------------------------------
  it('should compile valid C++ source and execute in runtime container', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    const cfg = getCompilerConfig();
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <iostream>
#include <cstdlib>

int main() {
    int *p = (int *)std::malloc(32);
    if (!p) return 1;
    p[0] = 42;
    p[1] = 84;
    std::free(p);
    std::cout << "CPP Success: " << 42 << std::endl;
    return 0;
}
`,
          language: 'cpp'
        });

      assert.equal(compileRes.status, 200);
      assert.equal(compileRes.body.success, true);
      jobId = compileRes.body.data.jobId;

      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 0);
      assert.ok(execRes.body.data.execution.stdout.includes('CPP Success: 42'));
    } finally {
      if (jobId) cleanupWorkspace(path.join(cfg.workspaceRoot, jobId));
    }
  });

  // ---------------------------------------------------------------------------
  // 5. Invalid Source Code Handling
  // ---------------------------------------------------------------------------
  it('should reject invalid C source code with 422 and leave zero executable artifacts', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    const cfg = getCompilerConfig();
    const compileRes = await request(app)
      .post('/api/compile')
      .send({
        sourceCode: `
int main() {
    this_is_a_syntax_error_12345 = ;
    return 0;
}
`,
        language: 'c'
      });

    assert.equal(compileRes.status, 422);
    assert.equal(compileRes.body.success, false);
    assert.equal(compileRes.body.error.code, 'COMPILATION_ERROR');
    assert.equal(compileRes.body.error.stage, 'FRONTEND_COMPILATION');
    assert.ok(compileRes.body.error.diagnostics.includes('error:'));

    // Verify workspace leaves no executable
    if (compileRes.body.error.jobId) {
      const wsDir = path.join(cfg.workspaceRoot, compileRes.body.error.jobId);
      const binPath = path.join(wsDir, 'program.bin');
      assert.equal(fs.existsSync(binPath), false, 'Failed compilation must not leave program.bin');
      cleanupWorkspace(wsDir);
    }
  });

  // ---------------------------------------------------------------------------
  // 6. Out-of-Bounds Heap Write Detection
  // ---------------------------------------------------------------------------
  it('should detect out-of-bounds heap access at runtime and return violation diagnostics', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    const cfg = getCompilerConfig();
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <stdlib.h>

int main(void) {
    int *buf = (int *)malloc(16); // 16 bytes: indices 0..3
    if (!buf) return 1;
    buf[0] = 1;
    volatile int idx = 4;
    buf[idx] = 99; // Out-of-bounds access!
    free(buf);
    return 0;
}
`,
          language: 'c'
        });

      assert.equal(compileRes.status, 200);
      jobId = compileRes.body.data.jobId;
      assert.ok(compileRes.body.data.diagnostics.summary.checksInserted >= 1);

      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
      assert.equal(execRes.body.data.execution.exitCode, 134); // SIGABRT
      assert.ok(execRes.body.data.diagnostics.violation);
      assert.equal(execRes.body.data.diagnostics.violation.detected, true);
      assert.ok(execRes.body.data.diagnostics.violation.violatingPointer);
      assert.equal(execRes.body.data.diagnostics.violation.accessSizeBytes, 4);
    } finally {
      if (jobId) cleanupWorkspace(path.join(cfg.workspaceRoot, jobId));
    }
  });

  // ---------------------------------------------------------------------------
  // 7. Use-After-Free Detection
  // ---------------------------------------------------------------------------
  it('should detect use-after-free access at runtime and return violation diagnostics', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    const cfg = getCompilerConfig();
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <stdlib.h>

int main(void) {
    int *buf = (int *)malloc(16);
    if (!buf) return 1;
    buf[0] = 10;
    free(buf);
    int readVal = buf[0]; // Use after free!
    return readVal;
}
`,
          language: 'c'
        });

      assert.equal(compileRes.status, 200);
      jobId = compileRes.body.data.jobId;

      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
      assert.equal(execRes.body.data.execution.exitCode, 134);
      assert.ok(execRes.body.data.diagnostics.violation);
      assert.equal(execRes.body.data.diagnostics.violation.detected, true);
    } finally {
      if (jobId) cleanupWorkspace(path.join(cfg.workspaceRoot, jobId));
    }
  });

  // ---------------------------------------------------------------------------
  // 8. Execution Timeout & Safe Container Termination
  // ---------------------------------------------------------------------------
  it('should terminate an infinite loop and return 504 TIMEOUT_ERROR', async (t) => {
    if (!(await checkDockerOrSkip(t))) return;

    let jobId = null;
    const cfg = getCompilerConfig();
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
int main(void) {
    volatile int x = 0;
    while (1) {
        x++;
    }
    return 0;
}
`,
          language: 'c'
        });

      assert.equal(compileRes.status, 200);
      jobId = compileRes.body.data.jobId;

      // Execute with a short timeout override for testing
      const { setRunnerTestOverrides, resetRunnerTestOverrides } = await import('../../src/services/execution/dockerRunner.js');
      setRunnerTestOverrides({ executionTimeoutMs: 1500 });

      try {
        const execRes = await request(app)
          .post('/api/execute')
          .send({ jobId });

        assert.equal(execRes.status, 504);
        assert.equal(execRes.body.success, false);
        assert.equal(execRes.body.error.code, 'TIMEOUT_ERROR');
        assert.ok(execRes.body.error.message.includes('timed out'));
      } finally {
        resetRunnerTestOverrides();
      }
    } finally {
      if (jobId) cleanupWorkspace(path.join(cfg.workspaceRoot, jobId));
    }
  });
});
