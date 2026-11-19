import React from 'react';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import App from '../src/App.jsx';

describe('App Integration', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  it('renders editor, header, and toolbar on startup', async () => {
    global.fetch = vi.fn().mockImplementation((url) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { status: 'healthy' } })
        });
      }
      return Promise.resolve({
        ok: true,
        headers: { get: () => 'application/json' },
        json: async () => ({ success: true, data: { available: true } })
      });
    });

    render(<App />);

    expect(screen.getByText('MemoryGuard')).toBeInTheDocument();
    expect(screen.getByText(/Compiler-Assisted Memory Safety/i)).toBeInTheDocument();

    // Editor textarea should render default C code
    const textarea = screen.getByTestId('code-editor-textarea');
    expect(textarea).toBeInTheDocument();
    expect(textarea.value).toContain('#include <stdlib.h>');

    // Buttons
    expect(screen.getByRole('button', { name: /compile & analyze/i })).toBeInTheDocument();
    const runBtn = screen.getByRole('button', { name: /^run$/i });
    expect(runBtn).toBeDisabled();
  });

  it('handles language selection without losing modified code on toggle', async () => {
    global.fetch = vi.fn().mockResolvedValue({
      ok: true,
      headers: { get: () => 'application/json' },
      json: async () => ({ success: true, data: {} })
    });

    render(<App />);

    const langSelect = screen.getByTestId('language-select');
    const textarea = screen.getByTestId('code-editor-textarea');

    // Edit C code
    fireEvent.change(textarea, { target: { value: '// Custom C code here' } });
    expect(textarea.value).toBe('// Custom C code here');

    // Switch to C++
    fireEvent.change(langSelect, { target: { value: 'cpp' } });
    expect(textarea.value).toContain('#include <iostream>');

    // Switch back to C - verify custom code was preserved!
    fireEvent.change(langSelect, { target: { value: 'c' } });
    expect(textarea.value).toBe('// Custom C code here');
  });

  it('executes successful compile pipeline and enables Run button', async () => {
    const mockJobId = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee';

    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { status: 'healthy' } })
        });
      }
      if (url.includes('/api/compile') && opts?.method === 'POST') {
        const body = JSON.parse(opts.body);
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'completed',
              language: body.language,
              stages: [
                { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 40 },
                { name: 'INSTRUMENTATION', status: 'completed', durationMs: 25 },
                { name: 'LINKING', status: 'completed', durationMs: 30 }
              ],
              diagnostics: {
                summary: {
                  functionsAnalyzed: ['main'],
                  totalAccessesExamined: 4,
                  heapAccessesIdentified: 2,
                  checksInserted: 2,
                  allocatorCallsRewritten: 2
                },
                raw: 'Module Analysis Summary: Checks inserted: 2'
              },
              artifacts: {
                executableAvailable: true
              },
              metadata: {
                totalDurationMs: 95
              }
            }
          })
        });
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

    expect(runBtn).toBeDisabled();

    // Trigger compilation
    fireEvent.click(compileBtn);

    // Wait for compilation to complete
    await waitFor(() => {
      expect(runBtn).not.toBeDisabled();
    });

    // Verify Analysis Dashboard shows inserted checks
    expect(screen.getByText('Checks Inserted')).toBeInTheDocument();
    expect(screen.getAllByText('2').length).toBeGreaterThanOrEqual(1);

    // Edit the source code in editor textarea
    const textarea = screen.getByTestId('code-editor-textarea');
    fireEvent.change(textarea, { target: { value: '#include <stdlib.h>\nint main() { return 1; }' } });

    // Run button must be invalidated and disabled!
    expect(runBtn).toBeDisabled();
  });

  it('disables Run button and shows compiler diagnostics when compilation fails', async () => {
    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { status: 'healthy' } })
        });
      }
      if (url.includes('/api/compile') && opts?.method === 'POST') {
        return Promise.resolve({
          ok: false,
          status: 422,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: false,
            error: {
              code: 'COMPILATION_ERROR',
              message: 'Syntax error in source code',
              stage: 'FRONTEND_COMPILATION',
              diagnostics: 'source.c:3:10: error: expected ;'
            }
          })
        });
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

    fireEvent.click(compileBtn);

    await waitFor(() => {
      expect(screen.getByText(/Compilation Failed/i)).toBeInTheDocument();
    });

    expect(runBtn).toBeDisabled();
    expect(screen.getByText(/source.c:3:10: error: expected ;/i)).toBeInTheDocument();
  });

  it('sends correct jobId on Run and renders runtime output and MemoryGuard violation', async () => {
    const mockJobId = '99999999-8888-7777-6666-555555555555';

    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: {} })
        });
      }
      if (url.includes('/api/compile') && opts?.method === 'POST') {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'completed',
              artifacts: { executableAvailable: true }
            }
          })
        });
      }
      if (url.includes('/api/execute') && opts?.method === 'POST') {
        const body = JSON.parse(opts.body);
        expect(body.jobId).toBe(mockJobId);

        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'violation_detected',
              execution: {
                exitCode: 134,
                terminationSignal: 'SIGABRT',
                durationMs: 44,
                stdout: 'Allocating buffer...\n',
                stderr: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x600003001, access_size=4 bytes\n'
              },
              diagnostics: {
                violation: {
                  detected: true,
                  violatingPointer: '0x600003001',
                  accessSizeBytes: 4,
                  rawMessage: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x600003001, access_size=4 bytes'
                }
              }
            }
          })
        });
      }
      return Promise.resolve({
        ok: true,
        headers: { get: () => 'application/json' },
        json: async () => ({ success: true, data: {} })
      });
    });

    render(<App />);

    // 1. Compile
    const compileBtn = screen.getByRole('button', { name: /compile & analyze/i });
    fireEvent.click(compileBtn);

    const runBtn = screen.getByRole('button', { name: /^run$/i });
    await waitFor(() => {
      expect(runBtn).not.toBeDisabled();
    });

    // 2. Run
    fireEvent.click(runBtn);

    // 3. Verify violation output
    await waitFor(() => {
      expect(screen.getByText('MemoryGuard Violation Detected')).toBeInTheDocument();
    });

    expect(screen.getByText('0x600003001')).toBeInTheDocument();
    expect(screen.getByText('4 bytes')).toBeInTheDocument();
  });
});
