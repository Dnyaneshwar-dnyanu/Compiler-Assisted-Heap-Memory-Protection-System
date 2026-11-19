/**
 * MemoryGuard API Client
 * Configurable base URL with unified error handling and network status management.
 */

const RAW_BASE_URL = import.meta.env.VITE_API_BASE_URL || '';
// Strip trailing slashes
export const API_BASE_URL = RAW_BASE_URL.replace(/\/+$/, '');

export class ApiError extends Error {
  constructor(message, { code = 'UNKNOWN_ERROR', statusCode = 500, details = null, stage = null, diagnostics = '', staticViolations = [], raw = null } = {}) {
    super(message);
    this.name = 'ApiError';
    this.code = code;
    this.statusCode = statusCode;
    this.details = details;
    this.stage = stage;
    this.diagnostics = diagnostics;
    this.staticViolations = staticViolations;
    this.raw = raw;
  }
}

/**
 * Standard fetch wrapper with JSON serialization, timeout, and structured error extraction.
 */
export async function apiRequest(endpoint, { method = 'GET', body = null, headers = {}, timeoutMs = 25000 } = {}) {
  const url = `${API_BASE_URL}${endpoint.startsWith('/') ? endpoint : `/${endpoint}`}`;

  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);

  const config = {
    method,
    headers: {
      'Accept': 'application/json',
      ...headers
    },
    signal: controller.signal
  };

  if (body !== null && body !== undefined) {
    config.headers['Content-Type'] = 'application/json';
    config.body = JSON.stringify(body);
  }

  try {
    const res = await fetch(url, config);
    clearTimeout(timer);

    let json;
    const contentType = res.headers.get('content-type') || '';
    if (contentType.includes('application/json')) {
      json = await res.json();
    } else {
      const text = await res.text();
      try {
        json = JSON.parse(text);
      } catch {
        json = { message: text };
      }
    }

    if (!res.ok || json?.success === false) {
      const errPayload = json?.error || {};
      throw new ApiError(
        errPayload.message || json?.message || `Request failed with HTTP status ${res.status}`,
        {
          code: errPayload.code || 'HTTP_ERROR',
          statusCode: res.status,
          details: errPayload.details || null,
          stage: errPayload.stage || null,
          diagnostics: errPayload.diagnostics || '',
          staticViolations: errPayload.staticViolations || [],
          raw: json
        }
      );
    }

    return json;
  } catch (err) {
    clearTimeout(timer);
    if (err instanceof ApiError) {
      throw err;
    }

    if (err.name === 'AbortError') {
      throw new ApiError(`Request to ${endpoint} timed out after ${timeoutMs}ms.`, {
        code: 'REQUEST_TIMEOUT',
        statusCode: 504
      });
    }

    // Network / unreachable error
    throw new ApiError(
      `Unable to connect to MemoryGuard backend (${err.message}). Verify backend is running on ${API_BASE_URL || 'localhost:5000'}.`,
      {
        code: 'NETWORK_ERROR',
        statusCode: 0,
        details: err.message
      }
    );
  }
}
