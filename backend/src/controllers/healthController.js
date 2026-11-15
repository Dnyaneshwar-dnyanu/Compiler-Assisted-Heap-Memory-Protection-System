import config from '../config/index.js';
import { sendSuccess } from '../utils/apiResponse.js';

/**
 * Health check controller.
 * Returns operational status, app name, version, timestamp, and uptime.
 */
export function getHealth(req, res) {
  return sendSuccess(
    res,
    {
      status: 'healthy',
      application: config.appName,
      version: config.appVersion,
      timestamp: new Date().toISOString(),
      uptimeSeconds: Math.floor(process.uptime())
    },
    'MemoryGuard Backend is healthy and operational',
    200
  );
}
