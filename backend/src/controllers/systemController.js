import { getSystemInfo } from '../services/systemService.js';
import { sendSuccess } from '../utils/apiResponse.js';

/**
 * System information controller.
 * Exposes non-sensitive runtime metrics and environment tier.
 */
export function getSystem(req, res, next) {
  try {
    const info = getSystemInfo();
    return sendSuccess(res, info, 'System diagnostics retrieved successfully', 200);
  } catch (error) {
    next(error);
  }
}
