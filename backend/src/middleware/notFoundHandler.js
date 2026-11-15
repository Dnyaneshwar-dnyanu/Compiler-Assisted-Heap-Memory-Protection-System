import { sendError } from '../utils/apiResponse.js';

/**
 * 404 Not Found Middleware
 * Intercepts requests that do not match any defined route and returns a structured JSON 404 error.
 */
export function notFoundHandler(req, res) {
  return sendError(
    res,
    `Route not found: ${req.method} ${req.originalUrl}`,
    404,
    'ROUTE_NOT_FOUND',
    { method: req.method, path: req.originalUrl }
  );
}
