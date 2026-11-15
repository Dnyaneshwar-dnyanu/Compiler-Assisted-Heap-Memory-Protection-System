/**
 * Standard API Response Utilities
 * Ensures consistent JSON response structure across all endpoints.
 */

export function sendSuccess(res, data = null, message = 'Success', statusCode = 200) {
  return res.status(statusCode).json({
    success: true,
    message,
    data
  });
}

export function sendError(res, message = 'An error occurred', statusCode = 500, code = 'INTERNAL_ERROR', details = null) {
  const errorObj = {
    message,
    code
  };

  if (details !== null && details !== undefined) {
    errorObj.details = details;
  }

  return res.status(statusCode).json({
    success: false,
    error: errorObj
  });
}
