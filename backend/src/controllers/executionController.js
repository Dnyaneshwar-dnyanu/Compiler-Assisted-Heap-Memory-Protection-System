import { executeJob } from '../services/execution/executionService.js';
import { checkDockerAvailability } from '../services/execution/executionConfig.js';
import { sendSuccess, sendError } from '../utils/apiResponse.js';

/**
 * Controller for executing compiled MemoryGuard binaries inside Docker sandbox.
 */
export async function executeProgram(req, res, next) {
  try {
    // Only extract jobId from request body.
    // Client-supplied Docker configuration (e.g. customConfig) is strictly forbidden
    // to prevent tampering with container isolation, privileges, or security bounds.
    const { jobId } = req.body || {};

    if (!jobId) {
      return sendError(
        res,
        'Field "jobId" is required and must be a valid UUID string.',
        400,
        'INVALID_JOB_ID',
        { field: 'jobId' }
      );
    }

    // Set up client abort handling
    const abortController = new AbortController();
    res.on('close', () => {
      if (!res.writableEnded) {
        abortController.abort();
      }
    });

    const result = await executeJob(jobId, {
      abortSignal: abortController.signal
    });

    if (!result.success) {
      let statusCode = 500;
      if (result.errorCategory === 'INVALID_JOB_ID') {
        statusCode = 400;
      } else if (result.errorCategory === 'PATH_TRAVERSAL_DETECTED') {
        statusCode = 403;
      } else if (result.errorCategory === 'JOB_NOT_FOUND' || result.errorCategory === 'EXECUTABLE_NOT_FOUND') {
        statusCode = 404;
      } else if (result.errorCategory === 'DOCKER_UNAVAILABLE' || result.errorCategory === 'IMAGE_NOT_FOUND') {
        statusCode = 503;
      } else if (result.errorCategory === 'TIMEOUT_ERROR') {
        statusCode = 504;
      }

      return res.status(statusCode).json({
        success: false,
        error: {
          code: result.errorCategory || 'EXECUTION_FAILED',
          message: result.message,
          jobId: result.jobId || jobId,
          details: result.details || null
        }
      });
    }

    return sendSuccess(
      res,
      result,
      'Program execution completed inside secure Docker sandbox.',
      200
    );
  } catch (error) {
    next(error);
  }
}

/**
 * Health check specifically for Docker execution sandbox readiness.
 */
export async function checkExecutionHealth(req, res, next) {
  try {
    const status = await checkDockerAvailability();
    return sendSuccess(
      res,
      status,
      status.available
        ? 'Docker execution environment is available and operational.'
        : 'Docker execution daemon is not reachable.'
    );
  } catch (error) {
    next(error);
  }
}
