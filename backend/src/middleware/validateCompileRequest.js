import { sendError } from '../utils/apiResponse.js';

/**
 * Middleware to validate incoming compile requests.
 * Ensures the payload contains a non-empty source code string.
 */
export function validateCompileRequest(req, res, next) {
  if (!req.body || typeof req.body !== 'object') {
    return sendError(
      res,
      'Request body must be a JSON object',
      400,
      'VALIDATION_ERROR',
      { field: 'body', reason: 'Missing or non-object body' }
    );
  }

  // Accept sourceCode, source, or code for developer convenience
  const rawSource = req.body.sourceCode ?? req.body.source ?? req.body.code;

  if (rawSource === undefined || rawSource === null) {
    return sendError(
      res,
      'Source code is required. Please provide "sourceCode" or "source" as a non-empty string.',
      400,
      'VALIDATION_ERROR',
      { field: 'sourceCode', reason: 'Field is missing' }
    );
  }

  if (typeof rawSource !== 'string') {
    return sendError(
      res,
      'Source code must be a string.',
      400,
      'VALIDATION_ERROR',
      { field: 'sourceCode', reason: 'Expected string, received ' + typeof rawSource }
    );
  }

  if (rawSource.trim().length === 0) {
    return sendError(
      res,
      'Source code cannot be empty.',
      400,
      'VALIDATION_ERROR',
      { field: 'sourceCode', reason: 'String contains only whitespace or is empty' }
    );
  }

  // Attach sanitized source to request object
  req.validatedSourceCode = rawSource;
  next();
}
