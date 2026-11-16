import React, { useState } from 'react';
import { Activity, Shield, Cpu, ChevronDown, ChevronRight, Binary, FileCode } from 'lucide-react';

function MetricCard({ label, value, subtext, highlight = false, badge = null, color = 'zinc' }) {
  const displayVal = value !== null && value !== undefined ? value : 'N/A';

  const colorStyles = {
    indigo: 'border-indigo-500/30 bg-indigo-500/5 text-indigo-400',
    emerald: 'border-emerald-500/30 bg-emerald-500/5 text-emerald-400',
    amber: 'border-amber-500/30 bg-amber-500/5 text-amber-400',
    zinc: 'border-zinc-800 bg-zinc-900/60 text-zinc-100'
  }[color] || 'border-zinc-800 bg-zinc-900/60 text-zinc-100';

  return (
    <div className={`p-3 rounded border ${colorStyles} flex flex-col justify-between transition-colors`}>
      <div className="flex items-center justify-between gap-1 mb-1">
        <span className="text-[11px] font-medium text-zinc-400 truncate" title={label}>
          {label}
        </span>
        {badge && (
          <span className="text-[9px] px-1 py-0.2 rounded bg-zinc-800 text-zinc-400 border border-zinc-700/60 font-mono">
            {badge}
          </span>
        )}
      </div>
      <div className="flex items-baseline gap-2">
        <span className={`text-xl font-bold font-mono tracking-tight ${highlight ? 'text-indigo-400' : 'text-zinc-100'}`}>
          {displayVal}
        </span>
      </div>
      {subtext && <span className="text-[10px] text-zinc-500 mt-0.5 truncate">{subtext}</span>}
    </div>
  );
}

export function AnalysisSummary({ diagnostics, metadata, stages }) {
  const [showRaw, setShowRaw] = useState(false);

  const summary = diagnostics?.summary;
  const rawOpt = diagnostics?.raw;

  if (!summary && !rawOpt) {
    return (
      <div className="w-full h-full flex flex-col items-center justify-center p-8 text-center text-zinc-500">
        <Activity className="w-8 h-8 text-zinc-600 mb-2 stroke-[1.5]" />
        <p className="text-xs font-medium text-zinc-400">No MemoryGuard Analysis Available</p>
        <p className="text-[11px] text-zinc-600 mt-1 max-w-sm">
          Run "Compile &amp; Analyze" on valid C or C++ source code to inspect heap safety instrumentation counters.
        </p>
      </div>
    );
  }

  const functionsList = summary?.functionsAnalyzed || [];

  return (
    <div className="w-full h-full overflow-y-auto p-4 space-y-4">
      {/* Top Banner: Key Pass Highlights */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-2.5">
        <MetricCard
          label="Checks Inserted"
          value={summary?.checksInserted}
          subtext="call @mg_check_access"
          highlight
          color="indigo"
          badge="Heap Guard"
        />
        <MetricCard
          label="Heap Accesses"
          value={summary?.heapAccessesIdentified}
          subtext="Loads & stores instrumented"
          color="emerald"
        />
        <MetricCard
          label="Allocators Rewritten"
          value={summary?.allocatorCallsRewritten}
          subtext="malloc / free intercepted"
          color="emerald"
        />
        <MetricCard
          label="Total Accesses"
          value={summary?.totalAccessesExamined}
          subtext="All LLVM Load & Store ops"
        />
      </div>

      {/* Secondary Metrics: Provenance & Filter Counters */}
      <div>
        <h4 className="text-[11px] font-semibold text-zinc-400 uppercase tracking-wider mb-2 flex items-center gap-1.5">
          <Cpu className="w-3 h-3 text-zinc-500" />
          Instruction Provenance &amp; Filtering Breakdown
        </h4>
        <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 gap-2">
          <MetricCard
            label="Non-Heap Skipped"
            value={summary?.nonHeapSkipped}
            subtext="Stack / Global allocations"
          />
          <MetricCard
            label="Unknown Origin Skipped"
            value={summary?.unknownOriginSkipped}
            subtext="Conservative fallback"
          />
          <MetricCard
            label="Volatiles Instrumented"
            value={summary?.volatileAccessesInstrumented}
            subtext="Volatile load/store ops"
          />
          <MetricCard
            label="Atomics Skipped"
            value={summary?.atomicOperationsSkipped}
            subtext="Preserves synchronization"
          />
          <MetricCard
            label="Unsupported AddrSpace"
            value={summary?.unsupportedAddressSpaceSkipped}
            subtext="Non-zero address spaces"
          />
          <MetricCard
            label="Scalable Vectors"
            value={summary?.scalableVectorSkipped}
            subtext="Dynamic vector types"
          />
          <MetricCard
            label="Incompatible Allocators"
            value={summary?.incompatibleAllocatorCallsSkipped}
            subtext="Non-standard signatures"
          />
          <MetricCard
            label="Functions Analyzed"
            value={functionsList.length > 0 ? functionsList.length : (summary?.functionsAnalyzed ? 0 : null)}
            subtext={functionsList.length > 0 ? functionsList.join(', ') : 'All CFGs'}
          />
        </div>
      </div>

      {/* Analyzed Functions Tags */}
      {functionsList.length > 0 && (
        <div className="p-3 rounded border border-zinc-800 bg-zinc-900/40">
          <span className="text-[11px] font-medium text-zinc-400 block mb-1.5 flex items-center gap-1.5">
            <FileCode className="w-3.5 h-3.5 text-zinc-500" />
            Analyzed Function Control Flow Graphs
          </span>
          <div className="flex flex-wrap gap-1.5">
            {functionsList.map((fn, idx) => (
              <span
                key={idx}
                className="font-mono text-xs px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/60 text-zinc-300"
              >
                @{fn}
              </span>
            ))}
          </div>
        </div>
      )}

      {/* Raw LLVM Pass Log Collapsible */}
      {rawOpt && (
        <div className="border border-zinc-800 rounded bg-zinc-900/30 overflow-hidden">
          <button
            onClick={() => setShowRaw(!showRaw)}
            className="w-full px-3 py-2 text-left flex items-center justify-between text-xs font-mono text-zinc-400 hover:text-zinc-200 hover:bg-zinc-800/40 transition-colors"
          >
            <span className="flex items-center gap-1.5 font-medium">
              <Binary className="w-3.5 h-3.5 text-indigo-400" />
              Raw LLVM Opt Module Diagnostics Log
            </span>
            {showRaw ? <ChevronDown className="w-3.5 h-3.5" /> : <ChevronRight className="w-3.5 h-3.5" />}
          </button>

          {showRaw && (
            <div className="p-3 border-t border-zinc-800 bg-zinc-950 font-mono text-xs text-zinc-300 whitespace-pre overflow-x-auto leading-relaxed">
              {rawOpt}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
