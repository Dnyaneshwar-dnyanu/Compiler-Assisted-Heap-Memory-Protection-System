import fs from 'node:fs';
import {
  getCompilerConfig,
  validateToolchain,
  toWslPath,
  sanitizeDiagnostics
} from './compilerConfig.js';
import { createWorkspace, writeSource } from './workspaceManager.js';
import { runTool } from './compilerRunner.js';

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
 * Coordinate the full MemoryGuard compilation pipeline:
 * 1. Validate inputs and toolchain
 * 2. Create workspace and write source
 * 3. Emit LLVM IR bitcode with Clang/Clang++
 * 4. Run MemoryGuard pass with opt
 * 5. Link with MemoryGuard runtime static library
 */
export async function compileSource({ sourceCode, language = 'c', customConfig = {} }) {
  const startTime = Date.now();
  const cfg = { ...getCompilerConfig(), ...customConfig };

  // Validate language
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

  // Validate source code size
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

  // Validate toolchain dependencies
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

  // Create isolated workspace
  const workspace = createWorkspace(cfg.workspaceRoot);
  const paths = writeSource(workspace, sourceCode, normalizedLang);

  const stages = [];
  const compilerTool = normalizedLang === 'cpp' ? cfg.clangxxTool : cfg.clangTool;

  // Resolve file paths for the execution platform (WSL or native)
  const sourceArg = cfg.useWsl ? toWslPath(paths.filePath) : paths.filePath;
  const bitcodeArg = cfg.useWsl ? toWslPath(paths.bitcodeFile) : paths.bitcodeFile;
  const instBitcodeArg = cfg.useWsl ? toWslPath(paths.instrumentedBitcodeFile) : paths.instrumentedBitcodeFile;
  const executableArg = cfg.useWsl ? toWslPath(paths.executableFile) : paths.executableFile;
  const includeArg = cfg.useWsl ? toWslPath(cfg.runtimeIncludePath) : cfg.runtimeIncludePath;
  const pluginArg = cfg.useWsl ? toWslPath(cfg.pluginPath) : cfg.pluginPath;
  const runtimeLibArg = cfg.useWsl ? toWslPath(cfg.runtimeLibPath) : cfg.runtimeLibPath;

  // ---------------------------------------------------------------------------
  // Stage 1: Frontend Compilation to LLVM IR
  // ---------------------------------------------------------------------------
  const stage1Start = Date.now();
  const clangArgs = [
    '-O0',
    '-Xclang',
    '-disable-O0-optnone',
    '-emit-llvm',
    '-S',
    sourceArg,
    '-I',
    includeArg,
    '-o',
    bitcodeArg
  ];

  const clangResult = await runTool(compilerTool, clangArgs, {
    timeoutMs: cfg.timeoutMs,
    maxOutputBytes: cfg.maxOutputBytes,
    useWsl: cfg.useWsl
  });

  const stage1Duration = Date.now() - stage1Start;

  if (!clangResult.success) {
    stages.push({
      name: 'FRONTEND_COMPILATION',
      status: 'failed',
      durationMs: stage1Duration
    });

    const errorCategory = clangResult.timedOut
      ? 'TIMEOUT_ERROR'
      : (clangResult.spawnError ? 'SPAWN_ERROR' : 'COMPILATION_ERROR');

    const rawDiagnostics = clangResult.stderr || clangResult.stdout;
    const sanitized = sanitizeDiagnostics(rawDiagnostics, workspace.workspaceDir);

    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory,
      stage: 'FRONTEND_COMPILATION',
      message: clangResult.timedOut
        ? `Frontend compiler timed out after ${cfg.timeoutMs}ms.`
        : 'Frontend compilation failed with errors.',
      stages,
      diagnostics: sanitized,
      outputTruncated: clangResult.outputTruncated
    };
  }

  stages.push({
    name: 'FRONTEND_COMPILATION',
    status: 'completed',
    durationMs: stage1Duration
  });

  // ---------------------------------------------------------------------------
  // Stage 2: MemoryGuard LLVM Pass Instrumentation
  // ---------------------------------------------------------------------------
  const stage2Start = Date.now();
  const optArgs = [
    `-load-pass-plugin=${pluginArg}`,
    '-passes=memoryguard',
    '-S',
    bitcodeArg,
    '-o',
    instBitcodeArg
  ];

  const optResult = await runTool(cfg.optTool, optArgs, {
    timeoutMs: cfg.timeoutMs,
    maxOutputBytes: cfg.maxOutputBytes,
    useWsl: cfg.useWsl
  });

  const stage2Duration = Date.now() - stage2Start;

  if (!optResult.success) {
    stages.push({
      name: 'INSTRUMENTATION',
      status: 'failed',
      durationMs: stage2Duration
    });

    const errorCategory = optResult.timedOut ? 'TIMEOUT_ERROR' : 'INSTRUMENTATION_ERROR';
    const rawDiagnostics = optResult.stderr || optResult.stdout;
    const sanitized = sanitizeDiagnostics(rawDiagnostics, workspace.workspaceDir);

    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory,
      stage: 'INSTRUMENTATION',
      message: optResult.timedOut
        ? `MemoryGuard pass timed out after ${cfg.timeoutMs}ms.`
        : 'MemoryGuard LLVM instrumentation pass failed.',
      stages,
      diagnostics: sanitized,
      outputTruncated: optResult.outputTruncated
    };
  }

  stages.push({
    name: 'INSTRUMENTATION',
    status: 'completed',
    durationMs: stage2Duration
  });

  // Extract MemoryGuard analysis summary from opt output
  const optCombinedOutput = (optResult.stdout || '') + '\n' + (optResult.stderr || '');
  const parsedDiagnostics = parseMemoryGuardDiagnostics(optCombinedOutput);
  const sanitizedOptOutput = sanitizeDiagnostics(optCombinedOutput.trim(), workspace.workspaceDir);

  // ---------------------------------------------------------------------------
  // Stage 3: Linking with MemoryGuard Runtime
  // ---------------------------------------------------------------------------
  const stage3Start = Date.now();
  const linkArgs = [
    instBitcodeArg,
    runtimeLibArg,
    '-pthread',
    '-lm',
    '-o',
    executableArg
  ];

  const linkResult = await runTool(compilerTool, linkArgs, {
    timeoutMs: cfg.timeoutMs,
    maxOutputBytes: cfg.maxOutputBytes,
    useWsl: cfg.useWsl
  });

  const stage3Duration = Date.now() - stage3Start;

  if (!linkResult.success) {
    stages.push({
      name: 'LINKING',
      status: 'failed',
      durationMs: stage3Duration
    });

    const errorCategory = linkResult.timedOut ? 'TIMEOUT_ERROR' : 'LINKER_ERROR';
    const rawDiagnostics = linkResult.stderr || linkResult.stdout;
    const sanitized = sanitizeDiagnostics(rawDiagnostics, workspace.workspaceDir);

    return {
      success: false,
      jobId: workspace.jobId,
      errorCategory,
      stage: 'LINKING',
      message: linkResult.timedOut
        ? `Linker timed out after ${cfg.timeoutMs}ms.`
        : 'Linking against MemoryGuard runtime library failed.',
      stages,
      diagnostics: sanitized,
      outputTruncated: linkResult.outputTruncated
    };
  }

  stages.push({
    name: 'LINKING',
    status: 'completed',
    durationMs: stage3Duration
  });

  // Verify final executable artifact existence
  const executableExists = fs.existsSync(paths.executableFile) && fs.statSync(paths.executableFile).size > 0;
  const bitcodeExists = fs.existsSync(paths.bitcodeFile);
  const instBitcodeExists = fs.existsSync(paths.instrumentedBitcodeFile);

  const totalDurationMs = Date.now() - startTime;

  return {
    success: true,
    jobId: workspace.jobId,
    status: 'completed',
    language: normalizedLang,
    stages,
    diagnostics: {
      summary: parsedDiagnostics,
      raw: sanitizedOptOutput
    },
    artifacts: {
      executableAvailable: executableExists,
      bitcodeAvailable: bitcodeExists,
      instrumentedBitcodeAvailable: instBitcodeExists
    },
    metadata: {
      sourceBytes,
      totalDurationMs,
      executed: false
    }
  };
}
