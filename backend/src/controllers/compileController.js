import { handleCompilePlaceholder } from '../services/compileService.js';
import { sendSuccess } from '../utils/apiResponse.js';

/**
 * Compile controller.
 * In Step 19, receives and validates source code and returns a placeholder response.
 */
export function compileSource(req, res, next) {
  try {
    const sourceCode = req.validatedSourceCode;
    const { language } = req.body;

    const result = handleCompilePlaceholder(sourceCode, { language });

    return sendSuccess(
      res,
      result,
      'Source code received successfully. Compiler execution is not implemented in Step 19.',
      200
    );
  } catch (error) {
    next(error);
  }
}
