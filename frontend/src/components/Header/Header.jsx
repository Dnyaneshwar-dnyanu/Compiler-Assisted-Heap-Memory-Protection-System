import React from 'react';
import { ShieldCheck, RefreshCw, Server, Box } from 'lucide-react';
import { StatusIndicator } from '../StatusIndicator/StatusIndicator.jsx';

export function Header({
  isOnline,
  dockerStatus,
  isChecking,
  onRefreshHealth
}) {
  return (
    <header className="border-b border-zinc-800 bg-zinc-900/60 backdrop-blur px-4 py-3 shrink-0 flex items-center justify-between gap-4">
      {/* Brand & Subtitle */}
      <div className="flex items-center gap-3">
        <div className="flex items-center justify-center w-9 h-9 rounded-lg bg-indigo-600/20 border border-indigo-500/30 text-indigo-400 shadow-inner">
          <ShieldCheck className="w-5 h-5 text-indigo-400" />
        </div>
        <div>
          <div className="flex items-center gap-2">
            <h1 className="font-bold text-base tracking-tight text-zinc-100">
              MemoryGuard
            </h1>
            <span className="text-[10px] font-mono px-1.5 py-0.5 rounded bg-zinc-800 text-zinc-400 border border-zinc-700/60">
              LLVM 18
            </span>
          </div>
          <p className="text-xs text-zinc-400 font-normal">
            Compiler-Assisted Memory Safety Analysis &amp; Isolated Sandbox
          </p>
        </div>
      </div>

      {/* Backend & Docker Environment Status */}
      <div className="flex items-center gap-3">
        {/* Backend Node.js status */}
        <div className="flex items-center gap-1.5" title="Backend Express API Connection">
          <Server className="w-3.5 h-3.5 text-zinc-500 hidden sm:inline" />
          <StatusIndicator
            status={isOnline === true ? 'online' : isOnline === false ? 'offline' : 'default'}
            label={isOnline === true ? 'Backend Online' : isOnline === false ? 'Backend Offline' : 'Connecting...'}
            size="sm"
            pulse={isOnline === true}
          />
        </div>

        {/* Docker Sandbox status */}
        <div className="hidden md:flex items-center gap-1.5" title="Execution Docker Runner Status">
          <Box className="w-3.5 h-3.5 text-zinc-500" />
          <StatusIndicator
            status={dockerStatus?.available ? 'online' : 'warning'}
            label={dockerStatus?.available ? 'Docker Sandbox Ready' : 'Docker Daemon Down'}
            size="sm"
          />
        </div>

        {/* Refresh health button */}
        <button
          onClick={onRefreshHealth}
          disabled={isChecking}
          title="Refresh Backend Health"
          className="p-1.5 text-zinc-400 hover:text-zinc-200 hover:bg-zinc-800 rounded transition-colors disabled:opacity-50"
        >
          <RefreshCw className={`w-3.5 h-3.5 ${isChecking ? 'animate-spin text-indigo-400' : ''}`} />
        </button>
      </div>
    </header>
  );
}
