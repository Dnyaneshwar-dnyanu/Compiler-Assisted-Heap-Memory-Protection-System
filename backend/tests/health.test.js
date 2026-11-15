import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../src/app.js';

describe('GET /api/health', () => {
  it('should return 200 with healthy operational status', async () => {
    const res = await request(app).get('/api/health');

    assert.equal(res.status, 200);
    assert.equal(res.headers['content-type'].includes('application/json'), true);
    assert.equal(res.body.success, true);
    assert.equal(typeof res.body.message, 'string');

    const data = res.body.data;
    assert.ok(data);
    assert.equal(data.status, 'healthy');
    assert.equal(data.application, 'MemoryGuard Backend API');
    assert.equal(data.version, '1.0.0');
    assert.equal(typeof data.timestamp, 'string');
    assert.equal(typeof data.uptimeSeconds, 'number');
    assert.ok(data.uptimeSeconds >= 0);
  });

  it('should include helmet security headers', async () => {
    const res = await request(app).get('/api/health');
    assert.ok(res.headers['x-content-type-options']);
    assert.equal(res.headers['x-content-type-options'], 'nosniff');
  });
});
