import config from '../config/index.js';

export const logger = {
  info: (msg, ...args) => {
    if (!config.isTest) {
      console.log(`[INFO] ${new Date().toISOString()} - ${msg}`, ...args);
    }
  },
  warn: (msg, ...args) => {
    if (!config.isTest) {
      console.warn(`[WARN] ${new Date().toISOString()} - ${msg}`, ...args);
    }
  },
  error: (msg, ...args) => {
    if (!config.isTest) {
      console.error(`[ERROR] ${new Date().toISOString()} - ${msg}`, ...args);
    }
  }
};

export const morganStream = {
  write: (message) => {
    if (!config.isTest) {
      process.stdout.write(message);
    }
  }
};
