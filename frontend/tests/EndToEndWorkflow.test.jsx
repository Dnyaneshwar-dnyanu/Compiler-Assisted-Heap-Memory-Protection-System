import React from 'react';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import App from '../src/App.jsx';

describe('Frontend End-to-End User Workflows', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
  });

  // ---------------------------------------------------------------------------
  // 1. Complete C Compile and Run Workflow
  // ---------------------------------------------------------------------------
  it('should complete full C compile-to-execution workflow cleanly', async () => {
    const mockJobId = '11111111-aaaa-bbbb-cccc-111111111111';

    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { status: 'healthy' } })
        });
      }
      if (url.includes('/api/execute/health')) {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({ success: true, data: { available: true, version: 'Docker 29.2.1' } })
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
              language: 'c',
              stages: [
                { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 45 },
                { name: 'INSTRUMENTATION', status: 'completed', durationMs: 30 },
                { name: 'LINKING', status: 'completed', durationMs: 40 }
              ],
              diagnostics: {
                summary: {
                  functionsAnalyzed: ['main'],
                  totalAccessesExamined: 8,
                  heapAccessesIdentified: 4,
                  checksInserted: 4,
                  allocatorCallsRewritten: 2,
                  nonHeapSkipped: 4
                },
                raw: 'Pass completed: 4 checks inserted.'
              },
              artifacts: {
                executableAvailable: true
              },
              metadata: {
                totalDurationMs: 115
              }
            }
          })
        });
      }
      if (url.includes('/api/execute') && opts?.method === 'POST') {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'no_violations_detected',
              execution: {
                exitCode: 0,
                terminationSignal: null,
                durationMs: 52,
                stdout: '[Program] Array elements: 10 20 30\nExecution completed.\n',
                stderr: '',
                outputTruncated: false
              },
              diagnostics: { violation: null }
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

    // 1. Initial State: Run disabled, editor loaded
    const compileBtn = screen.getByRole('button', { name: /compile & analyze/i });
    const runBtn = screen.getByRole('button', { name: /^run$/i });
    expect(runBtn).toBeDisabled();

    // 2. Trigger Compile
    fireEvent.click(compileBtn);

    // 3. Verify Analysis Dashboard appears with real metrics
    await waitFor(() => {
      expect(screen.getByText('Checks Inserted')).toBeInTheDocument();
      expect(screen.getByText('Heap Accesses')).toBeInTheDocument();
      expect(screen.getByText('Allocators Rewritten')).toBeInTheDocument();
    });

    // 4. Verify Run button is now enabled
    expect(runBtn).not.toBeDisabled();

    // 5. Trigger Run
    fireEvent.click(runBtn);

    // 6. Verify Output View shows clean execution
    await waitFor(() => {
      expect(screen.getByText('No Violations Detected')).toBeInTheDocument();
      expect(screen.getByText(/Array elements: 10 20 30/i)).toBeInTheDocument();
    });

    // Verify exit code and timing
    expect(screen.getByText('0')).toBeInTheDocument();
    expect(screen.getByText('52ms')).toBeInTheDocument();
  });

  // ---------------------------------------------------------------------------
  // 2. Complete C++ Compile and Run Workflow
  // ---------------------------------------------------------------------------
  it('should switch to C++ and complete compile & run workflow', async () => {
    const mockJobId = '22222222-bbbb-cccc-dddd-222222222222';

    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/compile') && opts?.method === 'POST') {
        const body = JSON.parse(opts.body);
        expect(body.language).toBe('cpp');

        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'completed',
              language: 'cpp',
              stages: [
                { name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 120 },
                { name: 'INSTRUMENTATION', status: 'completed', durationMs: 40 },
                { name: 'LINKING', status: 'completed', durationMs: 50 }
              ],
              diagnostics: {
                summary: {
                  functionsAnalyzed: ['main'],
                  totalAccessesExamined: 6,
                  heapAccessesIdentified: 2,
                  checksInserted: 2
                }
              },
              artifacts: { executableAvailable: true }
            }
          })
        });
      }
      if (url.includes('/api/execute') && opts?.method === 'POST') {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: mockJobId,
              status: 'no_violations_detected',
              execution: {
                exitCode: 0,
                durationMs: 65,
                stdout: 'C++ Object verified.\n',
                stderr: ''
              },
              diagnostics: { violation: null }
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

    // Switch to C++
    const langSelect = screen.getByTestId('language-select');
    fireEvent.change(langSelect, { target: { value: 'cpp' } });

    // Compile
    const compileBtn = screen.getByRole('button', { name: /compile & analyze/i });
    fireEvent.click(compileBtn);

    const runBtn = screen.getByRole('button', { name: /^run$/i });
    await waitFor(() => {
      expect(runBtn).not.toBeDisabled();
    });

    // Run
    fireEvent.click(runBtn);

    await waitFor(() => {
      expect(screen.getByText(/C\+\+ Object verified/i)).toBeInTheDocument();
    });
  });

  // ---------------------------------------------------------------------------
  // 3. Violation Detection & Visual Presentation
  // ---------------------------------------------------------------------------
  it('should present MemoryGuard violation card with address, size, and signal', async () => {
    const mockJobId = '33333333-cccc-dddd-eeee-333333333333';

    global.fetch = vi.fn().mockImplementation((url, opts) => {
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
                durationMs: 38,
                stdout: 'Starting out-of-bounds write test...\n',
                stderr: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55aa1234b000, access_size=4 bytes\n'
              },
              diagnostics: {
                violation: {
                  detected: true,
                  violatingPointer: '0x55aa1234b000',
                  accessSizeBytes: 4,
                  rawMessage: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55aa1234b000, access_size=4 bytes'
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

    fireEvent.click(screen.getByRole('button', { name: /compile & analyze/i }));

    const runBtn = screen.getByRole('button', { name: /^run$/i });
    await waitFor(() => expect(runBtn).not.toBeDisabled());

    fireEvent.click(runBtn);

    await waitFor(() => {
      expect(screen.getByText('MemoryGuard Violation Detected')).toBeInTheDocument();
      expect(screen.getByText('0x55aa1234b000')).toBeInTheDocument();
      expect(screen.getByText('4 bytes')).toBeInTheDocument();
      expect(screen.getByText(/SIGABRT Triggered/i)).toBeInTheDocument();
    });
  });

  // ---------------------------------------------------------------------------
  // 4. Stack-Only Code Shows Zero Checks
  // ---------------------------------------------------------------------------
  it('should correctly display 0 checks inserted for stack-only memory access', async () => {
    global.fetch = vi.fn().mockImplementation((url, opts) => {
      if (url.includes('/api/compile') && opts?.method === 'POST') {
        return Promise.resolve({
          ok: true,
          headers: { get: () => 'application/json' },
          json: async () => ({
            success: true,
            data: {
              jobId: '44444444-dddd-eeee-ffff-444444444444',
              status: 'completed',
              stages: [{ name: 'FRONTEND_COMPILATION', status: 'completed', durationMs: 30 }],
              diagnostics: {
                summary: {
                  functionsAnalyzed: ['main'],
                  totalAccessesExamined: 5,
                  heapAccessesIdentified: 0,
                  checksInserted: 0,
                  nonHeapSkipped: 5
                }
              },
              artifacts: { executableAvailable: true }
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

    fireEvent.click(screen.getByRole('button', { name: /compile & analyze/i }));

    await waitFor(() => {
      expect(screen.getByText('Non-Heap Skipped')).toBeInTheDocument();
      expect(screen.getAllByText('0').length).toBeGreaterThanOrEqual(1);
    });
  });
});
