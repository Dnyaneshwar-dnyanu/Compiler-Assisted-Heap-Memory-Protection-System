import process from 'node:process';
import config from '../config/index.js';

/**
 * Collect safe, non-sensitive backend system diagnostics.
 * Crucially avoids exposing filesystem paths, process.env, usernames, or network interfaces.
 */
export function getSystemInfo() {
  const memory = process.memoryUsage();

  return {
    nodeVersion: process.version,
    platform: process.platform,
    arch: process.arch,
    environment: config.env,
    uptimeSeconds: Math.floor(process.uptime()),
    memoryUsageMB: {
      rss: Math.round(memory.rss / 1024 / 1024),
      heapTotal: Math.round(memory.heapTotal / 1024 / 1024),
      heapUsed: Math.round(memory.heapUsed / 1024 / 1024)
    }
  };
}
