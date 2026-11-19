import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../../src/app.js';
import { checkCompilerDockerAvailability } from '../../src/services/compiler/compilerConfig.js';
import { cleanupWorkspace } from '../../src/services/compiler/workspaceManager.js';

describe('Real Docker Compiler Pipeline Integration (Step 26)', () => {
  it('should compile and instrument C source inside real Docker compiler container if daemon is available', async (t) => {
    const status = await checkCompilerDockerAvailability();

    if (!status.available) {
      t.skip(
        `Skipping real Docker compiler container execution: Docker daemon is unavailable on host (${
          status.reason || 'daemon unreachable'
        }).`
      );
      return;
    }

    // When Docker daemon IS active on host: execute real container compilation!
    let jobId = null;
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <stdio.h>
#include <stdlib.h>
int main() {
    int *p = (int *)malloc(32);
    if (!p) return 1;
    p[0] = 42;
    p[7] = 99;
    free(p);
    printf("Docker Compiler E2E Success\\n");
    return 0;
}
`,
          language: 'c'
        });

      assert.equal(compileRes.status, 200);
      assert.equal(compileRes.body.success, true);
      assert.ok(compileRes.body.data.jobId);
      jobId = compileRes.body.data.jobId;

      assert.equal(compileRes.body.data.status, 'completed');
      assert.equal(compileRes.body.data.stages.length, 3);
      assert.equal(compileRes.body.data.artifacts.executableAvailable, true);

      // Verify execution in the runtime container
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.ok(execRes.body.data.execution.stdout.includes('Docker Compiler E2E Success'));
    } finally {
      // Clean up workspace
      if (jobId) {
        // Workspace cleanup is handled by workspace root expiration or job cleanup
      }
    }
  });

  it('should compile C++ source inside real Docker compiler container if daemon is available', async (t) => {
    const status = await checkCompilerDockerAvailability();

    if (!status.available) {
      t.skip(
        `Skipping real Docker C++ compiler container execution: Docker daemon is unavailable on host (${
          status.reason || 'daemon unreachable'
        }).`
      );
      return;
    }

    let jobId = null;
    try {
      const compileRes = await request(app)
        .post('/api/compile')
        .send({
          sourceCode: `
#include <iostream>
#include <cstdlib>
int main() {
    int *p = (int *)std::malloc(64);
    if (!p) return 1;
    p[0] = 100;
    std::free(p);
    std::cout << "Docker C++ Success" << std::endl;
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
      assert.ok(execRes.body.data.execution.stdout.includes('Docker C++ Success'));
    } finally {
      if (jobId) {}
    }
  });
});
