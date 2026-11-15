import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import app from '../src/app.js';

describe('Unknown Routes (404 Handler)', () => {
  it('should return 404 JSON for non-existent GET endpoints', async () => {
    const res = await request(app).get('/api/non-existent-endpoint');

    assert.equal(res.status, 404);
    assert.equal(res.headers['content-type'].includes('application/json'), true);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'ROUTE_NOT_FOUND');
    assert.ok(res.body.error.message.includes('/api/non-existent-endpoint'));
  });

  it('should return 404 JSON for non-existent POST endpoints', async () => {
    const res = await request(app).post('/random-url');

    assert.equal(res.status, 404);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'ROUTE_NOT_FOUND');
  });
});
