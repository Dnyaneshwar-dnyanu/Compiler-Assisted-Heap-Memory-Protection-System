import { Router } from 'express';
import healthRoutes from './healthRoutes.js';
import systemRoutes from './systemRoutes.js';
import compileRoutes from './compileRoutes.js';

const router = Router();

router.use('/health', healthRoutes);
router.use('/system', systemRoutes);
router.use('/compile', compileRoutes);

export default router;
