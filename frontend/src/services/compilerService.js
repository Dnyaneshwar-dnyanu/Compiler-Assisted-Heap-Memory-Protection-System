import { apiRequest, ApiError } from './api.js';

/**
 * Service for triggering compiler pipeline and MemoryGuard instrumentation pass.
 */
export async function compileSource({ sourceCode, language = 'c' }) {
  if (!sourceCode || typeof sourceCode !== 'string' || !sourceCode.trim()) {
    throw new ApiError('Source code cannot be empty.', {
      code: 'VALIDATION_ERROR',
      statusCode: 400
    });
  }

  const normalizedLang = language.toLowerCase() === 'cpp' ? 'cpp' : 'c';

  const response = await apiRequest('/api/compile', {
    method: 'POST',
    body: {
      sourceCode,
      language: normalizedLang
    }
  });

  return response.data;
}
