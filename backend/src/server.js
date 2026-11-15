import app from './app.js';
import config from './config/index.js';
import { logger } from './utils/logger.js';

const server = app.listen(config.port, () => {
  logger.info(`${config.appName} (v${config.appVersion}) listening on port ${config.port} in ${config.env} mode`);
});

// Graceful shutdown handling
function gracefulShutdown(signal) {
  logger.info(`Received ${signal}. Gracefully terminating HTTP server...`);

  server.close(() => {
    logger.info('HTTP server closed. Exiting process.');
    process.exit(0);
  });

  // Force close after 5 seconds if lingering connections exist
  setTimeout(() => {
    logger.error('Graceful shutdown timeout exceeded. Forcing exit.');
    process.exit(1);
  }, 5000).unref();
}

process.on('SIGTERM', () => gracefulShutdown('SIGTERM'));
process.on('SIGINT', () => gracefulShutdown('SIGINT'));

export default server;
