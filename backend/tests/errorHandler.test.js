import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import express from 'express';
import app from '../src/app.js';
import { errorHandler } from '../src/middleware/errorHandler.js';

describe('Centralized Error Handling Middleware', () => {
  it('should catch malformed JSON syntax with 400 INVALID_JSON', async () => {
    const res = await request(app)
      .post('/api/compile')
      .set('Content-Type', 'application/json')
      .send('{ "sourceCode": "unterminated string ');

    assert.equal(res.status, 400);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.code, 'INVALID_JSON');
    assert.ok(res.body.error.message.includes('Malformed JSON'));
  });

  it('should format custom 500 errors uniformly with code and message', async () => {
    const testApp = express();
    testApp.get('/throw-error', (req, res, next) => {
      const err = new Error('Database connection failed');
      err.code = 'DB_ERROR';
      next(err);
    });
    testApp.use(errorHandler);

    const res = await request(testApp).get('/throw-error');

    assert.equal(res.status, 500);
    assert.equal(res.body.success, false);
    assert.equal(res.body.error.message, 'Database connection failed');
    assert.equal(res.body.error.code, 'DB_ERROR');
  });

  it('should suppress stack trace when NODE_ENV is production', async () => {
    const origEnv = process.env.NODE_ENV;
    process.env.NODE_ENV = 'production';

    try {
      // Re-import config or create a fresh test app with the handler
      const testApp = express();
      testApp.get('/fail', (req, res, next) => {
        next(new Error('Sensitive internal fault'));
      });
      testApp.use(errorHandler);

      const res = await request(testApp).get('/fail');

      assert.equal(res.status, 500);
      assert.equal(res.body.success, false);
      assert.equal(res.body.error.stack, undefined);
    } finally {
      process.env.NODE_ENV = origEnv;
    }
  });
});
