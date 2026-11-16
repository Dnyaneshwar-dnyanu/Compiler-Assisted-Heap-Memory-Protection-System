import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../src/app.js';

describe('POST /api/compile (LLVM Toolchain Pipeline)', () => {
  it('should compile valid C source with MemoryGuard instrumentation and linking', async () => {
    const validPayload = {
      sourceCode: `
#include <stdio.h>
#include <stdlib.h>
int main() {
    int *p = (int *)malloc(32);
    if (!p) return 1;
    p[0] = 42;
    p[7] = 99;
    free(p);
    return 0;
}
`,
      language: 'c'
    };

    const res = await request(app)
      .post('/api/compile')
      .send(validPayload);

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.message, 'Compilation and MemoryGuard instrumentation succeeded.');

    const data = res.body.data;
    assert.ok(data);
    assert.ok(data.jobId);
    assert.equal(data.status, 'completed');
    assert.equal(data.language, 'c');

    // Verify 3 completed stages
    assert.equal(Array.isArray(data.stages), true);
    assert.equal(data.stages.length, 3);
    assert.equal(data.stages[0].name, 'FRONTEND_COMPILATION');
    assert.equal(data.stages[0].status, 'completed');
    assert.equal(data.stages[1].name, 'INSTRUMENTATION');
    assert.equal(data.stages[1].status, 'completed');
    assert.equal(data.stages[2].name, 'LINKING');
    assert.equal(data.stages[2].status, 'completed');

    // Verify artifacts
    assert.ok(data.artifacts);
    assert.equal(data.artifacts.executableAvailable, true);
    assert.equal(data.artifacts.bitcodeAvailable, true);
    assert.equal(data.artifacts.instrumentedBitcodeAvailable, true);

    // Verify diagnostics summary
    assert.ok(data.diagnostics);
    assert.ok(data.diagnostics.summary);
    assert.ok(data.diagnostics.summary.checksInserted >= 2);
    assert.ok(data.diagnostics.summary.allocatorCallsRewritten >= 2);
    assert.ok(data.diagnostics.summary.heapAccessesIdentified >= 2);

    // Verify metadata
    assert.ok(data.metadata);
    assert.equal(data.metadata.executed, false);

    // Verify no filesystem paths are leaked in the response
    const jsonStr = JSON.stringify(res.body);
    assert.equal(jsonStr.includes('/mnt/d/'), false);
    assert.equal(jsonStr.includes('D:\\'), false);
  });

  it('should compile valid C++ source with Clang++ and MemoryGuard instrumentation', async () => {
    const validCppPayload = {
      sourceCode: `
#include <cstdlib>
int main() {
    int *buf = static_cast<int *>(std::malloc(64));
    if (!buf) return 1;
    buf[0] = 123;
    std::free(buf);
    return 0;
}
`,
      language: 'cpp'
    };

    const res = await request(app)
      .post('/api/compile')
      .send(validCppPayload);

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'completed');
    assert.equal(res.body.data.language, 'cpp');
    assert.equal(res.body.data.artifacts.executableAvailable, true);
    assert.ok(res.body.data.diagnostics.summary.checksInserted >= 1);
  });

  it('should accept alternative "source" field name', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({ source: 'int main() { return 0; }', language: 'c' });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'completed');
  });

  it('should reject requests with missing sourceCode with 400 VALIDATION_ERROR', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({});

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
    assert.ok(res.body.error.message.includes('Source code is required'));
  });

  it('should reject requests with empty string sourceCode', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({ sourceCode: '' });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
    assert.ok(res.body.error.message.includes('Source code cannot be empty'));
  });

  it('should reject requests with whitespace-only sourceCode', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({ sourceCode: '   \n\t  \r\n   ' });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
  });

  it('should reject requests where sourceCode is not a string', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({ sourceCode: 12345 });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
    assert.ok(res.body.error.message.includes('must be a string'));
  });

  it('should reject unsupported programming language with 400 VALIDATION_ERROR', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({
        sourceCode: 'print("hello")',
        language: 'python'
      });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
    assert.ok(res.body.error.message.includes("Unsupported language 'python'"));
  });

  it('should reject non-string language field with 400 VALIDATION_ERROR', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({
        sourceCode: 'int main() { return 0; }',
        language: 99
      });

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'VALIDATION_ERROR');
    assert.ok(res.body.error.message.includes('Language must be a string'));
  });

  it('should return 422 for C compilation syntax error', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({
        sourceCode: 'int main() { invalid_syntax @@@ ; return 0; }',
        language: 'c'
      });

    assert.equal(res.status, 422);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.stage, 'FRONTEND_COMPILATION');
    assert.equal(res.body.error.code, 'COMPILATION_ERROR');
    assert.ok(res.body.error.diagnostics.length > 0);
  });

  it('should return 422 for C++ compilation syntax error', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({
        sourceCode: '#include <iostream>\nint main() { std::cout << ; }',
        language: 'cpp'
      });

    assert.equal(res.status, 422);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.stage, 'FRONTEND_COMPILATION');
    assert.equal(res.body.error.code, 'COMPILATION_ERROR');
  });
});
