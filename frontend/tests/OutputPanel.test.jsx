import React from 'react';
import { describe, it, expect } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { OutputPanel } from '../src/components/OutputPanel/OutputPanel.jsx';

describe('OutputPanel Component', () => {
  it('renders unexecuted empty state when executionResult is null', () => {
    render(<OutputPanel executionResult={null} executionError={null} isRunning={false} />);
    expect(screen.getByText(/Program Not Executed Yet/i)).toBeInTheDocument();
  });

  it('renders running spinner when isRunning is true', () => {
    render(<OutputPanel executionResult={null} executionError={null} isRunning={true} />);
    expect(screen.getByText(/Executing Inside Docker Sandbox/i)).toBeInTheDocument();
  });

  it('displays clean execution with no_violations_detected and safety disclaimer', () => {
    const mockResult = {
      status: 'no_violations_detected',
      execution: {
        exitCode: 0,
        terminationSignal: null,
        durationMs: 42,
        stdout: 'Hello MemoryGuard!\n',
        stderr: ''
      },
      diagnostics: { violation: null }
    };

    render(<OutputPanel executionResult={mockResult} executionError={null} isRunning={false} />);

    expect(screen.getByText('No Violations Detected')).toBeInTheDocument();
    expect(screen.getByText(/Hello MemoryGuard!/i)).toBeInTheDocument();
    expect(screen.getByText(/42ms/i)).toBeInTheDocument();
    // Verify that we do not claim "Completely memory-safe"
    expect(screen.getByText(/Dynamic Heap Verification/i)).toBeInTheDocument();
  });

  it('displays MemoryGuard violation details when violation is detected', () => {
    const mockViolation = {
      status: 'violation_detected',
      execution: {
        exitCode: 134,
        terminationSignal: 'SIGABRT',
        durationMs: 38,
        stdout: 'Beginning test...\n',
        stderr: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d1a8f9c2a0, access_size=4 bytes\n'
      },
      diagnostics: {
        violation: {
          detected: true,
          violatingPointer: '0x55d1a8f9c2a0',
          accessSizeBytes: 4,
          rawMessage: 'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d1a8f9c2a0, access_size=4 bytes'
        }
      }
    };

    render(<OutputPanel executionResult={mockViolation} executionError={null} isRunning={false} />);

    expect(screen.getByText('MemoryGuard Violation Detected')).toBeInTheDocument();
    expect(screen.getByText('0x55d1a8f9c2a0')).toBeInTheDocument();
    expect(screen.getByText('4 bytes')).toBeInTheDocument();
    expect(screen.getByText(/SIGABRT Triggered/i)).toBeInTheDocument();
  });

  it('allows switching between stdout, stderr, and combined stream views', () => {
    const mockResult = {
      status: 'no_violations_detected',
      execution: {
        exitCode: 0,
        durationMs: 25,
        stdout: 'Only Stdout Output',
        stderr: 'Only Stderr Warning'
      },
      diagnostics: { violation: null }
    };

    render(<OutputPanel executionResult={mockResult} executionError={null} isRunning={false} />);

    // Click Stdout tab
    const stdoutTab = screen.getByRole('button', { name: /stdout/i });
    fireEvent.click(stdoutTab);
    expect(screen.getByText('Only Stdout Output')).toBeInTheDocument();

    // Click Stderr tab
    const stderrTab = screen.getByRole('button', { name: /stderr/i });
    fireEvent.click(stderrTab);
    expect(screen.getByText('Only Stderr Warning')).toBeInTheDocument();
  });

  it('renders execution error banner when executionError is present', () => {
    const mockError = {
      code: 'DOCKER_UNAVAILABLE',
      message: 'Docker daemon is not running.'
    };

    render(<OutputPanel executionResult={null} executionError={mockError} isRunning={false} />);

    expect(screen.getByText(/Execution Error \(DOCKER_UNAVAILABLE\)/i)).toBeInTheDocument();
    expect(screen.getByText(/Docker daemon is not running/i)).toBeInTheDocument();
  });
});
