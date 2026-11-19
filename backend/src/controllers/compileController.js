import { compileSource as compilePipeline } from '../services/compiler/compilerService.js';
import { sendSuccess } from '../utils/apiResponse.js';

/**
 * Compile controller.
 * In Step 20, invokes the MemoryGuard LLVM compilation pipeline:
 * 1. Clang/Clang++ emits uninstrumented bitcode
 * 2. Opt runs MemoryGuard pass to instrument memory operations
 * 3. Linker produces final instrumented executable
 */
export async function compileSource(req, res, next) {
  try {
    const sourceCode = req.validatedSourceCode;
    const language = req.validatedLanguage || 'c';

    const result = await compilePipeline({ sourceCode, language });

    if (!result.success) {
      let statusCode = 422; // Unprocessable Entity for compilation/syntax/linker errors
      if (result.errorCategory === 'TIMEOUT_ERROR') {
        statusCode = 504; // Gateway Timeout
      } else if (result.errorCategory === 'TOOLCHAIN_ERROR' || result.errorCategory === 'SPAWN_ERROR') {
        statusCode = 503; // Service Unavailable
      } else if (result.errorCategory === 'PAYLOAD_TOO_LARGE') {
        statusCode = 413; // Payload Too Large
      } else if (result.errorCategory === 'INVALID_LANGUAGE') {
        statusCode = 400; // Bad Request
      }

      return res.status(statusCode).json({
        success: false,
        error: {
          code: result.errorCategory || 'COMPILATION_ERROR',
          message: result.message,
          stage: result.stage,
          jobId: result.jobId || null,
          diagnostics: result.diagnostics || '',
          stages: result.stages || [],
          staticViolations: result.staticViolations || []
        }
      });
    }

    return sendSuccess(
      res,
      result,
      'Compilation and MemoryGuard instrumentation succeeded.',
      200
    );
  } catch (error) {
    next(error);
  }
}
