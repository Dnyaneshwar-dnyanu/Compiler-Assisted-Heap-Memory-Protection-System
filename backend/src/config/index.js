import dotenv from 'dotenv';
import process from 'node:process';

// Load environment variables from .env file if available
dotenv.config();

const config = {
  port: parseInt(process.env.PORT || '5000', 10),
  env: process.env.NODE_ENV || 'development',
  isProduction: process.env.NODE_ENV === 'production',
  isTest: process.env.NODE_ENV === 'test',
  corsOrigin: process.env.CORS_ORIGIN || '*',
  maxRequestSize: process.env.MAX_REQUEST_SIZE || '1mb',
  appName: 'MemoryGuard Backend API',
  appVersion: '1.0.0'
};

export default config;
