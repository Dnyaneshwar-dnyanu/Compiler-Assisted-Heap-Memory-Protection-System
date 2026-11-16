import { Router } from 'express';
import { executeProgram, checkExecutionHealth } from '../controllers/executionController.js';

const router = Router();

// POST /api/execute
router.post('/', executeProgram);

// GET /api/execute/health
router.get('/health', checkExecutionHealth);

export default router;
