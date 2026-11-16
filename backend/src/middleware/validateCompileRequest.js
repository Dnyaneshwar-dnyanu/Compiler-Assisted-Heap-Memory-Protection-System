import { sendError } from '../utils/apiResponse.js';
import { getCompilerConfig } from '../services/compiler/compilerConfig.js';

const SUPPORTED_LANGUAGES = ['c', 'cpp'];

/**
 * Middleware to validate incoming compile requests.
 * Ensures the payload contains a non-empty source code string and a valid supported language.
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

  const cfg = getCompilerConfig();
  const sourceBytes = Buffer.byteLength(rawSource, 'utf8');
  if (sourceBytes > cfg.maxSourceBytes) {
    return sendError(
      res,
      `Source code exceeds maximum allowed size of ${cfg.maxSourceBytes} bytes.`,
      400,
      'PAYLOAD_TOO_LARGE',
      { field: 'sourceCode', sizeBytes: sourceBytes, maxAllowedBytes: cfg.maxSourceBytes }
    );
  }

  // Validate language
  let language = 'c';
  if (req.body.language !== undefined && req.body.language !== null) {
    if (typeof req.body.language !== 'string') {
      return sendError(
        res,
        'Language must be a string.',
        400,
        'VALIDATION_ERROR',
        { field: 'language', reason: 'Expected string, received ' + typeof req.body.language }
      );
    }

    const norm = req.body.language.toLowerCase().trim();
    if (!SUPPORTED_LANGUAGES.includes(norm)) {
      return sendError(
        res,
        `Unsupported language '${req.body.language}'. Supported languages are: ${SUPPORTED_LANGUAGES.join(', ')}.`,
        400,
        'VALIDATION_ERROR',
        { field: 'language', supported: SUPPORTED_LANGUAGES, received: req.body.language }
      );
    }
    language = norm;
  }

  // Attach sanitized source and language to request object
  req.validatedSourceCode = rawSource;
  req.validatedLanguage = language;
  next();
}
