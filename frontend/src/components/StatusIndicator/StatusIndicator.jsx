import React from 'react';

export function StatusIndicator({
  status = 'default',
  label,
  sublabel,
  size = 'md',
  pulse = false,
  className = ''
}) {
  const styles = {
    online: {
      dot: 'bg-emerald-400',
      border: 'border-emerald-500/20',
      bg: 'bg-emerald-500/10',
      text: 'text-emerald-400'
    },
    offline: {
      dot: 'bg-rose-400',
      border: 'border-rose-500/20',
      bg: 'bg-rose-500/10',
      text: 'text-rose-400'
    },
    warning: {
      dot: 'bg-amber-400',
      border: 'border-amber-500/20',
      bg: 'bg-amber-500/10',
      text: 'text-amber-400'
    },
    info: {
      dot: 'bg-indigo-400',
      border: 'border-indigo-500/20',
      bg: 'bg-indigo-500/10',
      text: 'text-indigo-400'
    },
    default: {
      dot: 'bg-zinc-400',
      border: 'border-zinc-500/20',
      bg: 'bg-zinc-500/10',
      text: 'text-zinc-400'
    }
  }[status] || {
    dot: 'bg-zinc-400',
    border: 'border-zinc-500/20',
    bg: 'bg-zinc-500/10',
    text: 'text-zinc-400'
  };

  const sizeClasses = size === 'sm'
    ? 'text-xs px-2 py-0.5'
    : 'text-xs px-2.5 py-1';

  return (
    <span
      className={`inline-flex items-center gap-1.5 rounded-full border ${styles.bg} ${styles.border} ${styles.text} font-medium ${sizeClasses} ${className}`}
      title={sublabel}
    >
      <span className="relative flex h-2 w-2">
        {pulse && (
          <span
            className={`animate-ping absolute inline-flex h-full w-full rounded-full opacity-75 ${styles.dot}`}
          />
        )}
        <span className={`relative inline-flex rounded-full h-2 w-2 ${styles.dot}`} />
      </span>
      <span>{label}</span>
      {sublabel && <span className="opacity-60 text-[10px]">({sublabel})</span>}
    </span>
  );
}
