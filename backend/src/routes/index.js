import { Router } from 'express';
import healthRoutes from './healthRoutes.js';
import systemRoutes from './systemRoutes.js';
import compileRoutes from './compileRoutes.js';
import executionRoutes from './executionRoutes.js';

const router = Router();

router.use('/health', healthRoutes);
router.use('/system', systemRoutes);
router.use('/compile', compileRoutes);
router.use('/execute', executionRoutes);

export default router;
