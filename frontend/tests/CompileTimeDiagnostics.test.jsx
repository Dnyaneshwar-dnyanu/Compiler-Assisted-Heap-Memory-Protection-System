import React from 'react';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import App from '../src/App.jsx';
import { DiagnosticsPanel } from '../src/components/DiagnosticsPanel/DiagnosticsPanel.jsx';
import { CodeEditor } from '../src/components/Editor/CodeEditor.jsx';

describe('Compile-Time Heap Memory Diagnostics (Frontend)', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  it('renders static violation card with exact metrics in DiagnosticsPanel', () => {
    const mockViolations = [
      {
        errorType: 'STATIC_HEAP_OUT_OF_BOUNDS_WRITE',
        severity: 'error',
        file: 'source.c',
        line: 4,
        column: 5,
        accessType: 'write',
        allocationSize: 12,
        accessSize: 4,
        offset: 12,
        bytesOutOfBounds: 4,
        statement: 'arr[3] = 40;',
        message: 'Definite heap out-of-bounds write of 4 bytes at offset 12 into 12-byte allocation (exceeds buffer by 4 bytes).'
      }
    ];

    render(
      <DiagnosticsPanel
        compileResult={null}
        compileError={{
          code: 'STATIC_MEMORY_VIOLATION',
          message: 'Compilation rejected: static heap memory violation detected.',
          stage: 'INSTRUMENTATION',
          staticViolations: mockViolations,
          rawOutput: '=== STAGE_FAILED: STATIC_MEMORY_VIOLATION ==='
        }}
        isCompiling={false}
      />
    );

    // Verify header and badge
    expect(screen.getByText(/Statically Proven Heap Violations/i)).toBeInTheDocument();
    expect(screen.getByText('STATIC_HEAP_OUT_OF_BOUNDS_WRITE')).toBeInTheDocument();

    // Verify location
    expect(screen.getByText('source.c:4:5')).toBeInTheDocument();

    // Verify metrics
    expect(screen.getByText('Allocation Size')).toBeInTheDocument();
    expect(screen.getByText('Offset')).toBeInTheDocument();
    expect(screen.getAllByText('12 bytes').length).toBe(2);
    expect(screen.getByText('Access Type')).toBeInTheDocument();
    expect(screen.getByText('WRITE (4 bytes)')).toBeInTheDocument();

    // Verify offending statement and message
    expect(screen.getByText('arr[3] = 40;')).toBeInTheDocument();
    expect(screen.getByText(/Definite heap out-of-bounds write/)).toBeInTheDocument();
  });

  it('renders line violation indicators in CodeEditor component', () => {
    const mockViolations = [
      {
        errorType: 'STATIC_HEAP_OUT_OF_BOUNDS_WRITE',
        severity: 'error',
        line: 4,
        column: 5,
        message: 'Definite heap out-of-bounds write'
      }
    ];

    render(
      <CodeEditor
        code="int main() {\n  int *arr = malloc(12);\n  arr[0] = 1;\n  arr[3] = 40;\n  return 0;\n}"
        language="c"
        onChange={() => {}}
        staticViolations={mockViolations}
      />
    );

    // Verify fallback indicator rendered in test environment
    expect(screen.getByTestId('editor-static-violations-indicator')).toBeInTheDocument();
    expect(screen.getByTestId('static-violation-marker-line-4')).toBeInTheDocument();
    expect(screen.getByText(/Line 4:5/)).toBeInTheDocument();
    expect(screen.getByText(/Definite heap out-of-bounds write/)).toBeInTheDocument();
  });

  it('disables Run button on static violation, and re-enables Run button after fixing code and compiling cleanly', async () => {
    let compileAttempt = 0;

    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { status: 'healthy', available: true } })
        });
      }

      if (url.includes('/api/compile') && opts?.method === 'POST') {
        compileAttempt++;
        if (compileAttempt === 1) {
          // First attempt: Static violation
          return Promise.resolve({
            ok: false,
            status: 422,
            headers: { get: () => 'application/json' },
            json: async () => ({
              success: false,
              error: {
                code: 'STATIC_MEMORY_VIOLATION',
                stage: 'INSTRUMENTATION',
                message: 'Compilation blocked: static heap memory violation detected.',
                staticViolations: [
                  {
                    errorType: 'STATIC_HEAP_OUT_OF_BOUNDS_WRITE',
                    severity: 'error',
                    file: 'source.c',
                    line: 4,
                    column: 5,
                    accessType: 'write',
                    allocationSize: 12,
                    accessSize: 4,
                    offset: 12,
                    bytesOutOfBounds: 4,
                    statement: 'arr[3] = 40;',
                    message: 'Definite heap out-of-bounds write of 4 bytes at offset 12'
                  }
                ]
              }
            })
          });
        } else {
          // Second attempt: Clean code
          return Promise.resolve({
            ok: true,
            status: 200,
            headers: { get: () => 'application/json' },
            json: async () => ({
              success: true,
              data: {
                jobId: '11111111-2222-3333-4444-555555555555',
                status: 'completed',
                stages: [
                  { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 30 },
                  { name: 'INSTRUMENTATION', status: 'completed', durationMs: 20 },
                  { name: 'LINKING', status: 'completed', durationMs: 25 }
                ],
                diagnostics: {
                  summary: {
                    functionsAnalyzed: ['main'],
                    totalAccessesExamined: 3,
                    heapAccessesIdentified: 1,
                    checksInserted: 1,
                    allocatorCallsRewritten: 1
                  }
                },
                artifacts: {
                  executableAvailable: true
                }
              }
            })
          });
        }
      }

      return Promise.resolve({
        ok: true,
        headers: { get: () => 'application/json' },
        json: async () => ({ success: true, data: {} })
      });
    });

    render(<App />);

    const compileBtn = screen.getByRole('button', { name: /compile & analyze/i });
    const runBtn = screen.getByRole('button', { name: /^run$/i });

    // Initially Run button is disabled
    expect(runBtn).toBeDisabled();

    // 1. First compile: Triggers static violation
    fireEvent.click(compileBtn);

    await waitFor(() => {
      expect(screen.getByTestId('static-violation-card')).toBeInTheDocument();
    });

    // Run button MUST remain disabled
    expect(runBtn).toBeDisabled();

    // Offending line marker must be rendered
    expect(screen.getByTestId('static-violation-marker-line-4')).toBeInTheDocument();

    // 2. Fix the code in the editor
    const textarea = screen.getByTestId('code-editor-textarea');
    fireEvent.change(textarea, {
      target: {
        value: '#include <stdlib.h>\nint main() {\n  int *arr = malloc(12);\n  arr[2] = 40;\n  free(arr);\n  return 0;\n}'
      }
    });

    // Run button still disabled
    expect(runBtn).toBeDisabled();

    // 3. Second compile: Clean code succeeds
    fireEvent.click(compileBtn);

    await waitFor(() => {
      // Run button must now be enabled!
      expect(runBtn).not.toBeDisabled();
    });

    // Static violation card should no longer be present
    expect(screen.queryByTestId('static-violation-card')).not.toBeInTheDocument();
  });

  it('keeps compiler syntax errors strictly separate from MemoryGuard diagnostics', () => {
    // Ordinary syntax error (e.g. clang error)
    render(
      <DiagnosticsPanel
        compileResult={null}
        compileError={{
          message: 'Compilation failed at stage FRONTEND_COMPILATION',
          diagnostics: "source.c:3:5: error: use of undeclared identifier 'foo'",
          stage: 'FRONTEND_COMPILATION',
          staticViolations: []
        }}
        isCompiling={false}
      />
    );

    // Should NOT show MemoryGuard violation card
    expect(screen.queryByTestId('static-violation-card')).not.toBeInTheDocument();
    expect(screen.queryByText(/Statically Proven Heap Violations/i)).not.toBeInTheDocument();

    // Should show compiler failure header and raw diagnostics
    expect(screen.getAllByText(/Compilation Failed/i).length).toBeGreaterThanOrEqual(1);
    expect(screen.getByText(/use of undeclared identifier 'foo'/)).toBeInTheDocument();
  });
});
