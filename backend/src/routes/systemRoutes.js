import { Router } from 'express';
import { getSystem } from '../controllers/systemController.js';

const router = Router();

// GET /api/system/info
router.get('/info', getSystem);

export default router;
