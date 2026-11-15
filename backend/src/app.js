import express from 'express';
import helmet from 'helmet';
import cors from 'cors';
import morgan from 'morgan';
import config from './config/index.js';
import apiRoutes from './routes/index.js';
import { notFoundHandler } from './middleware/notFoundHandler.js';
import { errorHandler } from './middleware/errorHandler.js';
import { morganStream } from './utils/logger.js';

const app = express();

// Security headers with Helmet
app.use(helmet());

// CORS configuration
const corsOptions = {
  origin: config.corsOrigin === '*'
    ? '*'
    : config.corsOrigin.split(',').map((o) => o.trim()),
  methods: ['GET', 'POST', 'PUT', 'DELETE', 'OPTIONS'],
  allowedHeaders: ['Content-Type', 'Authorization'],
  credentials: config.corsOrigin !== '*'
};
app.use(cors(corsOptions));

// Body parsers with configurable payload limits
app.use(express.json({ limit: config.maxRequestSize }));
app.use(express.urlencoded({ extended: true, limit: config.maxRequestSize }));

// HTTP access logging
if (!config.isTest) {
  const logFormat = config.isProduction ? 'combined' : 'dev';
  app.use(morgan(logFormat, { stream: morganStream }));
}

// Mount API routes
app.use('/api', apiRoutes);

// Catch-all 404 handler for unknown routes
app.use(notFoundHandler);

// Centralized error handler
app.use(errorHandler);

export default app;
