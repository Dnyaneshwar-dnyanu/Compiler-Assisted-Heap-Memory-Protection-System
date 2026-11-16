import React from 'react';
import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { AnalysisSummary } from '../src/components/AnalysisSummary/AnalysisSummary.jsx';

describe('AnalysisSummary Component', () => {
  it('renders empty state when diagnostics are null', () => {
    render(<AnalysisSummary diagnostics={null} metadata={null} stages={[]} />);
    expect(screen.getByText(/No MemoryGuard Analysis Available/i)).toBeInTheDocument();
  });

  it('renders all actual metrics from diagnostics summary', () => {
    const mockDiagnostics = {
      summary: {
        functionsAnalyzed: ['main', 'processData'],
        totalAccessesExamined: 14,
        heapAccessesIdentified: 6,
        checksInserted: 6,
        volatileAccessesInstrumented: 1,
        nonHeapSkipped: 7,
        unknownOriginSkipped: 0,
        atomicOperationsSkipped: 0,
        unsupportedAddressSpaceSkipped: 0,
        scalableVectorSkipped: 0,
        allocatorCallsRewritten: 2,
        incompatibleAllocatorCallsSkipped: 0
      },
      raw: '[MemoryGuard][Summary] Module Analysis Summary:\nChecks inserted: 6'
    };

    render(<AnalysisSummary diagnostics={mockDiagnostics} metadata={{}} stages={[]} />);

    // Verify key highlight metrics
    expect(screen.getByText('Checks Inserted')).toBeInTheDocument();
    expect(screen.getByText('Heap Accesses')).toBeInTheDocument();
    expect(screen.getByText('Allocators Rewritten')).toBeInTheDocument();
    expect(screen.getByText('Total Accesses')).toBeInTheDocument();

    // Verify function tags
    expect(screen.getByText('@main')).toBeInTheDocument();
    expect(screen.getByText('@processData')).toBeInTheDocument();
  });

  it('shows N/A for missing metrics without fabricating values', () => {
    const partialDiagnostics = {
      summary: {
        checksInserted: 3,
        // All other metrics omitted/undefined
      },
      raw: 'Minimal output'
    };

    render(<AnalysisSummary diagnostics={partialDiagnostics} metadata={{}} stages={[]} />);

    // Number 3 appears for checks inserted
    expect(screen.getByText('3')).toBeInTheDocument();

    // N/A should appear for unprovided counters
    const naElements = screen.getAllByText('N/A');
    expect(naElements.length).toBeGreaterThan(0);
  });
});
