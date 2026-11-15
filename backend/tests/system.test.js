import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../src/app.js';

describe('GET /api/system/info', () => {
  it('should return 200 with sanitized runtime diagnostic metrics', async () => {
    const res = await request(app).get('/api/system/info');

    assert.equal(res.status, 200);
    assert.equal(res.body.success, true);

    const data = res.body.data;
    assert.ok(data);
    assert.equal(typeof data.nodeVersion, 'string');
    assert.ok(data.nodeVersion.startsWith('v'));
    assert.equal(typeof data.platform, 'string');
    assert.equal(typeof data.arch, 'string');
    assert.equal(typeof data.environment, 'string');
    assert.equal(typeof data.uptimeSeconds, 'number');

    assert.ok(data.memoryUsageMB);
    assert.equal(typeof data.memoryUsageMB.rss, 'number');
    assert.equal(typeof data.memoryUsageMB.heapTotal, 'number');
    assert.equal(typeof data.memoryUsageMB.heapUsed, 'number');
  });

  it('must not expose sensitive environment variables or filesystem details', async () => {
    const res = await request(app).get('/api/system/info');
    const responseText = JSON.stringify(res.body);

    // Verify no process.env dump
    assert.equal(res.body.data.env, undefined);
    assert.equal(res.body.data.process, undefined);

    // Verify common sensitive keys are never exposed
    assert.equal(responseText.includes('PATH='), false);
    assert.equal(responseText.includes('NODE_ENV='), false);
    assert.equal(responseText.includes('HOME='), false);
    assert.equal(responseText.includes('USER='), false);
    assert.equal(responseText.includes('SECRET'), false);
    assert.equal(responseText.includes('TOKEN'), false);
  });
});
