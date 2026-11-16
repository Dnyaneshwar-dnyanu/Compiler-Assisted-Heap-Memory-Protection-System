import { describe, it, expect, vi, beforeEach } from 'vitest';
import { compileSource } from '../src/services/compilerService.js';
import { executeJob, checkExecutionHealth } from '../src/services/executionService.js';
import { apiRequest, ApiError } from '../src/services/api.js';

describe('Frontend API Services', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  describe('compilerService: compileSource', () => {
    it('should send correct POST payload and return data', async () => {
      const mockResponse = {
        success: true,
        data: {
          jobId: '11111111-2222-3333-4444-555555555555',
          status: 'completed',
          artifacts: { executableAvailable: true }
        }
      };

      global.fetch = vi.fn().mockResolvedValue({
        ok: true,
        headers: { get: () => 'application/json' },
        json: async () => mockResponse
      });

      const result = await compileSource({
        sourceCode: 'int main() { return 0; }',
        language: 'c'
      });

      expect(global.fetch).toHaveBeenCalledTimes(1);
      const [url, options] = global.fetch.mock.calls[0];
      expect(url).toContain('/api/compile');
      expect(options.method).toBe('POST');
      expect(JSON.parse(options.body)).toEqual({
        sourceCode: 'int main() { return 0; }',
        language: 'c'
      });
      expect(result.jobId).toBe('11111111-2222-3333-4444-555555555555');
    });

    it('should reject empty source code before sending network request', async () => {
      global.fetch = vi.fn();

      await expect(compileSource({ sourceCode: '   ' })).rejects.toThrow('Source code cannot be empty');
      expect(global.fetch).not.toHaveBeenCalled();
    });

    it('should handle compiler errors with structured ApiError', async () => {
      global.fetch = vi.fn().mockResolvedValue({
        ok: false,
        status: 422,
        headers: { get: () => 'application/json' },
        json: async () => ({
          success: false,
          error: {
            code: 'COMPILATION_ERROR',
            message: 'Syntax error at line 3',
            stage: 'FRONTEND_COMPILATION',
            diagnostics: 'error: expected expression'
          }
        })
      });

      try {
        await compileSource({ sourceCode: 'invalid code' });
        expect.unreachable('Should have thrown ApiError');
      } catch (err) {
        expect(err).toBeInstanceOf(ApiError);
        expect(err.statusCode).toBe(422);
        expect(err.code).toBe('COMPILATION_ERROR');
        expect(err.message).toBe('Syntax error at line 3');
        expect(err.diagnostics).toBe('error: expected expression');
      }
    });
  });

  describe('executionService: executeJob', () => {
    it('should send correct POST payload with jobId', async () => {
      const mockResponse = {
        success: true,
        data: {
          jobId: '22222222-3333-4444-5555-666666666666',
          status: 'no_violations_detected',
          execution: { exitCode: 0 }
        }
      };

      global.fetch = vi.fn().mockResolvedValue({
        ok: true,
        headers: { get: () => 'application/json' },
        json: async () => mockResponse
      });

      const result = await executeJob('22222222-3333-4444-5555-666666666666');

      expect(global.fetch).toHaveBeenCalledTimes(1);
      const [url, options] = global.fetch.mock.calls[0];
      expect(url).toContain('/api/execute');
      expect(options.method).toBe('POST');
      expect(JSON.parse(options.body)).toEqual({
        jobId: '22222222-3333-4444-5555-666666666666'
      });
      expect(result.status).toBe('no_violations_detected');
    });

    it('should reject missing or blank jobId before request', async () => {
      global.fetch = vi.fn();
      await expect(executeJob('')).rejects.toThrow('Valid Job ID is required');
      expect(global.fetch).not.toHaveBeenCalled();
    });

    it('should handle Docker unavailable error (HTTP 503)', async () => {
      global.fetch = vi.fn().mockResolvedValue({
        ok: false,
        status: 503,
        headers: { get: () => 'application/json' },
        json: async () => ({
          success: false,
          error: {
            code: 'DOCKER_UNAVAILABLE',
            message: 'Docker daemon is not running or unreachable.'
          }
        })
      });

      try {
        await executeJob('12345678-1234-1234-1234-123456789abc');
        expect.unreachable('Should have thrown ApiError');
      } catch (err) {
        expect(err.code).toBe('DOCKER_UNAVAILABLE');
        expect(err.statusCode).toBe(503);
      }
    });
  });
});
