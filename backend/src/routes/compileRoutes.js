import { Router } from 'express';
import { compileSource } from '../controllers/compileController.js';
import { validateCompileRequest } from '../middleware/validateCompileRequest.js';

const router = Router();

// POST /api/compile
router.post('/', validateCompileRequest, compileSource);

export default router;
