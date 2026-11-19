import fs from 'node:fs';
import path from 'node:path';
import {
  getCompilerConfig,
  validateToolchain,
  sanitizeDiagnostics
} from './compilerConfig.js';
import { createWorkspace, writeSource } from './workspaceManager.js';
import { runCompilerContainer } from './compilerRunner.js';

/**
 * Parses structured module metrics from MemoryGuard opt pass diagnostic output.
 */
export function parseMemoryGuardDiagnostics(output) {
  if (!output || typeof output !== 'string') return null;

  const extractInt = (pattern) => {
    const match = output.match(pattern);
    return match ? parseInt(match[1], 10) : null;
  };

  const totalAccessesExamined = extractInt(/Total load and store instructions examined:\s+(\d+)/);
  const heapAccessesIdentified = extractInt(/Heap-origin accesses identified:\s+(\d+)/);
  const checksInserted = extractInt(/Runtime checks inserted:\s+(\d+)/);
  const volatileAccessesInstrumented = extractInt(/Volatile accesses instrumented:\s+(\d+)/);
  const nonHeapSkipped = extractInt(/Non-heap accesses skipped:\s+(\d+)/);
  const unknownOriginSkipped = extractInt(/Unknown-origin accesses skipped:\s+(\d+)/);
  const atomicOperationsSkipped = extractInt(/Atomic operations skipped:\s+(\d+)/);
  const unsupportedAddressSpaceSkipped = extractInt(/Unsupported address-space accesses skipped:\s+(\d+)/);
  const scalableVectorSkipped = extractInt(/Scalable-vector accesses skipped:\s+(\d+)/);
  const allocatorCallsRewritten = extractInt(/Allocator calls detected and successfully rewritten:\s+(\d+)/);
  const incompatibleAllocatorCallsSkipped = extractInt(/Incompatible or unsupported allocator calls skipped:\s+(\d+)/);

  if (totalAccessesExamined === null && checksInserted === null && allocatorCallsRewritten === null) {
    return null;
  }

  const functionsAnalyzed = [];
  const fnMatches = output.matchAll(/Analyzing function @([a-zA-Z0-9_$.]+)/g);
  for (const match of fnMatches) {
    if (!functionsAnalyzed.includes(match[1])) {
      functionsAnalyzed.push(match[1]);
    }
  }

  return {
    functionsAnalyzed,
    totalAccessesExamined: totalAccessesExamined ?? 0,
    heapAccessesIdentified: heapAccessesIdentified ?? 0,
    checksInserted: checksInserted ?? 0,
    volatileAccessesInstrumented: volatileAccessesInstrumented ?? 0,
    nonHeapSkipped: nonHeapSkipped ?? 0,
    unknownOriginSkipped: unknownOriginSkipped ?? 0,
    atomicOperationsSkipped: atomicOperationsSkipped ?? 0,
    unsupportedAddressSpaceSkipped: unsupportedAddressSpaceSkipped ?? 0,
    scalableVectorSkipped: scalableVectorSkipped ?? 0,
    allocatorCallsRewritten: allocatorCallsRewritten ?? 0,
    incompatibleAllocatorCallsSkipped: incompatibleAllocatorCallsSkipped ?? 0
  };
}

/**
 * Coordinate the full MemoryGuard compilation pipeline inside a restricted Docker container:
 * 1. Validate inputs (language, size)
 * 2. Create isolated workspace with separate src and out directories
 * 3. Write source into src/
 * 4. Execute compilation and instrumentation pipeline inside Docker container
 * 5. Parse diagnostics and capture resulting executable and LLVM IR artifacts
 */
export async function compileSource({
  sourceCode,
  language = 'c',
  customConfig = {},
  abortSignal = null,
  _internalTestOverrides = null
}) {
  const startTime = Date.now();
  const cfg = { ...getCompilerConfig(), ...customConfig };

  // 1. Validate language
  const normalizedLang = (language || 'c').toLowerCase().trim();
  if (normalizedLang !== 'c' && normalizedLang !== 'cpp') {
    return {
      success: false,
      errorCategory: 'INVALID_LANGUAGE',
      stage: 'VALIDATION',
      message: `Unsupported language '${language}'. Supported languages: 'c' or 'cpp'.`,
      diagnostics: ''
    };
  }

  // 2. Validate source code size
  const sourceBytes = Buffer.byteLength(sourceCode || '', 'utf8');
  if (sourceBytes > cfg.maxSourceBytes) {
    return {
      success: false,
      errorCategory: 'PAYLOAD_TOO_LARGE',
      stage: 'VALIDATION',
      message: `Source code exceeds maximum allowed size of ${cfg.maxSourceBytes} bytes.`,
      diagnostics: ''
    };
  }

  // Validate custom toolchain paths if explicitly provided in tests
  if (customConfig.pluginPath || customConfig.runtimeLibPath) {
    const toolchain = validateToolchain(cfg);
    if (!toolchain.valid) {
      return {
        success: false,
        errorCategory: 'TOOLCHAIN_ERROR',
        stage: 'TOOLCHAIN_VALIDATION',
        message: 'MemoryGuard toolchain components are missing or incomplete.',
        diagnostics: toolchain.errors.join('\n')
      };
    }
  }

  // 3. Create isolated workspace
  const workspace = createWorkspace(cfg.workspaceRoot);
  const paths = writeSource(workspace, sourceCode, normalizedLang);

  // 4. Run compilation pipeline inside dedicated restricted Docker container
  const containerRes = await runCompilerContainer({
    srcDir: paths.srcDir,
    outDir: paths.outDir,
    language: normalizedLang,
    abortSignal,
    _internalTestOverrides,
    customConfig
  });

  const rawCombinedOutput = (containerRes.stdout || '') + '\n' + (containerRes.stderr || '');
  const sanitizedOutput = sanitizeDiagnostics(rawCombinedOutput.trim(), workspace.workspaceDir);

  // Helper to ensure failed or interrupted compilations leave zero stale executable artifacts
  const cleanupStaleArtifacts = () => {
    try {
      if (fs.existsSync(paths.outExecutableFile)) {
        fs.rmSync(paths.outExecutableFile, { force: true });
      }
      if (fs.existsSync(paths.executableFile)) {
        fs.rmSync(paths.executableFile, { force: true });
      }
      const manifestPath = path.join(workspace.workspaceDir, 'manifest.json');
      if (fs.existsSync(manifestPath)) {
        fs.rmSync(manifestPath, { force: true });
      }
    } catch { }
  };

  // Handle fatal container orchestration or environment errors
  if (containerRes.errorCategory === 'DOCKER_UNAVAILABLE') {
    cleanupStaleArtifacts();
    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory: 'TOOLCHAIN_ERROR',
      stage: 'TOOLCHAIN_VALIDATION',
      message: 'Docker daemon is not running or unreachable. Unable to start compilation container.',
      stages: [],
      diagnostics: sanitizedOutput || 'Docker daemon unreachable'
    };
  }

  if (containerRes.errorCategory === 'IMAGE_NOT_FOUND') {
    cleanupStaleArtifacts();
    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory: 'TOOLCHAIN_ERROR',
      stage: 'TOOLCHAIN_VALIDATION',
      message: containerRes.message || 'Compiler container image is not available locally.',
      stages: [],
      diagnostics: sanitizedOutput || 'Compiler image not found'
    };
  }

  if (containerRes.errorCategory === 'TIMEOUT_ERROR' || containerRes.timedOut) {
    cleanupStaleArtifacts();
    let failingStage = 'FRONTEND_COMPILATION';
    if (rawCombinedOutput.includes('STAGE_FAILED: INSTRUMENTATION')) {
      failingStage = 'INSTRUMENTATION';
    } else if (rawCombinedOutput.includes('STAGE_FAILED: LINKING')) {
      failingStage = 'LINKING';
    }

    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory: 'TIMEOUT_ERROR',
      stage: failingStage,
      message: `Compiler timed out after ${cfg.timeoutMs}ms.`,
      stages: [{ name: failingStage, status: 'failed', durationMs: containerRes.durationMs }],
      diagnostics: sanitizedOutput,
      outputTruncated: containerRes.outputTruncated
    };
  }

  if (containerRes.errorCategory === 'ABORTED' || containerRes.aborted) {
    cleanupStaleArtifacts();
    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory: 'ABORTED',
      stage: 'COMPILATION',
      message: 'Compilation aborted by client disconnect.',
      stages: [],
      diagnostics: ''
    };
  }

  // Handle compilation, instrumentation, or linking failures inside the container
  if (!containerRes.success) {
    cleanupStaleArtifacts();

    // Check for static MemoryGuard diagnostics generated by the compiler pass
    let staticViolations = [];
    const candidateDiagPaths = [
      paths?.outDir ? path.join(paths.outDir, 'static_diagnostics.json') : null,
      workspace?.workspaceDir ? path.join(workspace.workspaceDir, 'static_diagnostics.json') : null
    ].filter(Boolean);

    for (const diagPath of candidateDiagPaths) {
      if (fs.existsSync(diagPath)) {
        try {
          const parsed = JSON.parse(fs.readFileSync(diagPath, 'utf8'));
          if (Array.isArray(parsed) && parsed.length > 0) {
            staticViolations = parsed;
            break;
          }
        } catch { }
      }
    }

    // Populate source statement from sourceCode if missing in diagnostic
    if (staticViolations.length > 0) {
      const srcLines = sourceCode.split('\n');
      for (const v of staticViolations) {
        if (!v.statement && v.line > 0 && v.line <= srcLines.length) {
          v.statement = srcLines[v.line - 1].trim();
        }
      }
    }

    // If a definite static heap memory violation was detected, block compilation immediately
    if (
      containerRes.exitCode === 4 ||
      rawCombinedOutput.includes('STATIC_MEMORY_VIOLATION') ||
      staticViolations.some((v) => v.severity === 'error')
    ) {
      const stages = [
        { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: Math.round(containerRes.durationMs / 2) },
        { name: 'INSTRUMENTATION', status: 'failed', durationMs: Math.round(containerRes.durationMs / 2) }
      ];

      return {
        success: false,
        jobId: workspace.jobId,
        errorCategory: 'STATIC_MEMORY_VIOLATION',
        stage: 'INSTRUMENTATION',
        message: 'MemoryGuard detected compile-time heap memory violation.',
        stages,
        diagnostics: sanitizedOutput,
        staticViolations,
        outputTruncated: containerRes.outputTruncated
      };
    }

    let failingStage = 'FRONTEND_COMPILATION';
    let errorCategory = 'COMPILATION_ERROR';

    if (containerRes.exitCode === 2 || rawCombinedOutput.includes('STAGE_FAILED: INSTRUMENTATION')) {
      failingStage = 'INSTRUMENTATION';
      errorCategory = 'INSTRUMENTATION_ERROR';
    } else if (containerRes.exitCode === 3 || rawCombinedOutput.includes('STAGE_FAILED: LINKING')) {
      failingStage = 'LINKING';
      errorCategory = 'LINKER_ERROR';
    } else if (containerRes.exitCode === 1 || rawCombinedOutput.includes('STAGE_FAILED: FRONTEND_COMPILATION')) {
      failingStage = 'FRONTEND_COMPILATION';
      errorCategory = 'COMPILATION_ERROR';
    } else if (containerRes.spawnError) {
      errorCategory = 'SPAWN_ERROR';
    }

    const stages = [];
    if (failingStage === 'FRONTEND_COMPILATION') {
      stages.push({ name: 'FRONTEND_COMPILATION', status: 'failed', durationMs: containerRes.durationMs });
    } else if (failingStage === 'INSTRUMENTATION') {
      stages.push({ name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 0 });
      stages.push({ name: 'INSTRUMENTATION', status: 'failed', durationMs: containerRes.durationMs });
    } else if (failingStage === 'LINKING') {
      stages.push({ name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 0 });
      stages.push({ name: 'INSTRUMENTATION', status: 'completed', durationMs: 0 });
      stages.push({ name: 'LINKING', status: 'failed', durationMs: containerRes.durationMs });
    }

    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory,
      stage: failingStage,
      message: `${failingStage.replace('_', ' ')} failed with errors.`,
      stages,
      diagnostics: sanitizedOutput,
      staticViolations: [],
      outputTruncated: containerRes.outputTruncated
    };
  }

  // ---------------------------------------------------------------------------
  // Compilation Successful
  // ---------------------------------------------------------------------------
  const stages = [
    { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: Math.round(containerRes.durationMs / 3) },
    { name: 'INSTRUMENTATION', status: 'completed', durationMs: Math.round(containerRes.durationMs / 3) },
    { name: 'LINKING', status: 'completed', durationMs: Math.round(containerRes.durationMs / 3) }
  ];

  // If program.bin was created in out/, copy to workspace root so executionService finds it
  if (fs.existsSync(paths.outExecutableFile) && !fs.existsSync(paths.executableFile)) {
    try {
      fs.copyFileSync(paths.outExecutableFile, paths.executableFile);
      fs.chmodSync(paths.executableFile, 0o755);
    } catch { }
  }

  const executableExists = fs.existsSync(paths.executableFile) && fs.statSync(paths.executableFile).size > 0;
  const irExists = fs.existsSync(paths.irFile);
  const instIrExists = fs.existsSync(paths.instrumentedIrFile);

  // Write verified completion manifest for this specific jobId
  try {
    const manifestPath = path.join(workspace.workspaceDir, 'manifest.json');
    fs.writeFileSync(
      manifestPath,
      JSON.stringify({
        jobId: workspace.jobId,
        status: 'completed',
        language: normalizedLang,
        compiledAt: Date.now(),
        executableAvailable: executableExists
      }, null, 2),
      'utf8'
    );
  } catch { }

  const parsedDiagnostics = parseMemoryGuardDiagnostics(rawCombinedOutput);
  const totalDurationMs = Date.now() - startTime;

  return {
    success: true,
    jobId: workspace.jobId,
    status: 'completed',
    language: normalizedLang,
    stages,
    diagnostics: {
      summary: parsedDiagnostics,
      raw: sanitizedOutput
    },
    artifacts: {
      executableAvailable: executableExists,
      irAvailable: irExists,
      instrumentedIrAvailable: instIrExists,
      // Backwards-compatible aliases for existing assertions:
      bitcodeAvailable: irExists,
      instrumentedBitcodeAvailable: instIrExists
    },
    metadata: {
      sourceBytes,
      totalDurationMs,
      executed: false
    }
  };
}
