import config from '../config/index.js';
import { logger } from '../utils/logger.js';

/**
 * Centralized Error Handling Middleware
 * Intercepts unhandled errors, formats a uniform JSON response, and suppresses stack traces in production.
 */
export function errorHandler(err, req, res, next) {
  let statusCode = 500;
  let code = err.code || 'INTERNAL_SERVER_ERROR';
  let message = err.message || 'An unexpected internal error occurred';
  let details = err.details || null;

  // Handle common Express / body-parser errors
  if (err.type === 'entity.too.large' || err.status === 413) {
    statusCode = 413;
    code = 'PAYLOAD_TOO_LARGE';
    message = `Request entity too large. Maximum allowed size is ${config.maxRequestSize}.`;
  } else if (err instanceof SyntaxError && err.status === 400 && 'body' in err) {
    statusCode = 400;
    code = 'INVALID_JSON';
    message = 'Malformed JSON in request body.';
  } else if (err.status && typeof err.status === 'number' && err.status >= 400 && err.status < 600) {
    statusCode = err.status;
    if (!err.code) {
      code = statusCode === 400 ? 'BAD_REQUEST' : 'REQUEST_ERROR';
    }
  } else if (err.statusCode && typeof err.statusCode === 'number') {
    statusCode = err.statusCode;
    if (!err.code) {
      code = 'REQUEST_ERROR';
    }
  }

  // Log 5xx errors
  if (statusCode >= 500) {
    logger.error(`[${statusCode}] ${req.method} ${req.originalUrl} - ${err.message}`, err.stack);
  }

  const errorPayload = {
    message,
    code
  };

  if (details) {
    errorPayload.details = details;
  }

  // Expose stack trace only when not in production and available
  const isProd = process.env.NODE_ENV === 'production' || config.isProduction;
  if (!isProd && err.stack) {
    errorPayload.stack = err.stack;
  }

  return res.status(statusCode).json({
    success: false,
    error: errorPayload
  });
}
