import { apiRequest, ApiError } from './api.js';

/**
 * Service for triggering isolated Docker execution of compiled MemoryGuard binaries.
 */
export async function executeJob(jobId) {
  if (!jobId || typeof jobId !== 'string' || !jobId.trim()) {
    throw new ApiError('Valid Job ID is required to execute program.', {
      code: 'INVALID_JOB_ID',
      statusCode: 400
    });
  }

  const response = await apiRequest('/api/execute', {
    method: 'POST',
    body: {
      jobId: jobId.trim()
    }
  });

  return response.data;
}

/**
 * Health check specifically for execution sandbox readiness.
 */
export async function checkExecutionHealth() {
  try {
    const response = await apiRequest('/api/execute/health');
    return response.data;
  } catch (err) {
    return {
      available: false,
      error: err.message
    };
  }
}
