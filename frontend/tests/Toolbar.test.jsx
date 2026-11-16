import React from 'react';
import { describe, it, expect, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { Toolbar } from '../src/components/Toolbar/Toolbar.jsx';

describe('Toolbar Component', () => {
  it('renders language selector and triggers onChange', () => {
    const onLanguageChange = vi.fn();
    render(
      <Toolbar
        language="c"
        onLanguageChange={onLanguageChange}
        onSelectSample={vi.fn()}
        onResetCode={vi.fn()}
        onCompile={vi.fn()}
        onRun={vi.fn()}
        isCompiling={false}
        isRunning={false}
        canRun={false}
        hasJob={false}
      />
    );

    const select = screen.getByTestId('language-select');
    expect(select.value).toBe('c');

    fireEvent.change(select, { target: { value: 'cpp' } });
    expect(onLanguageChange).toHaveBeenCalledWith('cpp');
  });

  it('triggers compile button on click when not compiling', () => {
    const onCompile = vi.fn();
    render(
      <Toolbar
        language="c"
        onLanguageChange={vi.fn()}
        onSelectSample={vi.fn()}
        onResetCode={vi.fn()}
        onCompile={onCompile}
        onRun={vi.fn()}
        isCompiling={false}
        isRunning={false}
        canRun={false}
        hasJob={false}
      />
    );

    const compileBtn = screen.getByRole('button', { name: /compile & analyze/i });
    expect(compileBtn).not.toBeDisabled();
    fireEvent.click(compileBtn);
    expect(onCompile).toHaveBeenCalledTimes(1);
  });

  it('disables compile button during compilation', () => {
    render(
      <Toolbar
        language="c"
        onLanguageChange={vi.fn()}
        onSelectSample={vi.fn()}
        onResetCode={vi.fn()}
        onCompile={vi.fn()}
        onRun={vi.fn()}
        isCompiling={true}
        isRunning={false}
        canRun={false}
        hasJob={false}
      />
    );

    const compileBtn = screen.getByRole('button', { name: /compiling/i });
    expect(compileBtn).toBeDisabled();
  });

  it('disables run button when canRun is false', () => {
    const onRun = vi.fn();
    render(
      <Toolbar
        language="c"
        onLanguageChange={vi.fn()}
        onSelectSample={vi.fn()}
        onResetCode={vi.fn()}
        onCompile={vi.fn()}
        onRun={onRun}
        isCompiling={false}
        isRunning={false}
        canRun={false}
        hasJob={false}
      />
    );

    const runBtn = screen.getByRole('button', { name: /^run$/i });
    expect(runBtn).toBeDisabled();
    fireEvent.click(runBtn);
    expect(onRun).not.toHaveBeenCalled();
  });

  it('enables run button when canRun is true', () => {
    const onRun = vi.fn();
    render(
      <Toolbar
        language="c"
        onLanguageChange={vi.fn()}
        onSelectSample={vi.fn()}
        onResetCode={vi.fn()}
        onCompile={vi.fn()}
        onRun={onRun}
        isCompiling={false}
        isRunning={false}
        canRun={true}
        hasJob={true}
      />
    );

    const runBtn = screen.getByRole('button', { name: /^run$/i });
    expect(runBtn).not.toBeDisabled();
    fireEvent.click(runBtn);
    expect(onRun).toHaveBeenCalledTimes(1);
  });

  it('triggers reset code button', () => {
    const onReset = vi.fn();
    render(
      <Toolbar
        language="c"
        onLanguageChange={vi.fn()}
        onSelectSample={vi.fn()}
        onResetCode={onReset}
        onCompile={vi.fn()}
        onRun={vi.fn()}
        isCompiling={false}
        isRunning={false}
        canRun={false}
        hasJob={false}
      />
    );

    const resetBtn = screen.getByRole('button', { name: /reset/i });
    fireEvent.click(resetBtn);
    expect(onReset).toHaveBeenCalledTimes(1);
  });
});
