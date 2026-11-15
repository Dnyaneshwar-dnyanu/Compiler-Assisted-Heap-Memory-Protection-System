import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../src/app.js';

describe('POST /api/compile', () => {
  it('should accept valid sourceCode and return placeholder response', async () => {
    const validPayload = {
      sourceCode: '#include <stdlib.h>\nint main() { char *p = malloc(16); free(p); return 0; }',
      language: 'c'
    };

    const res = await request(app)
      .post('/api/compile')
      .send(validPayload);

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.ok(res.body.message.includes('Compiler execution is not implemented in Step 19'));

    const data = res.body.data;
    assert.ok(data);
    assert.equal(data.status, 'pending_compiler_integration');
    assert.equal(data.language, 'c');
    assert.equal(data.sourceMetadata.lineCount, 2);
    assert.ok(data.sourceMetadata.byteLength > 0);
  });

  it('should accept alternative "source" field name', async () => {
    const res = await request(app)
      .post('/api/compile')
      .send({ source: 'int x = 42;' });

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);
    assert.equal(res.body.data.status, 'pending_compiler_integration');
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
    assert.ok(res.body.error.message.includes('Source code cannot be empty'));
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
});
