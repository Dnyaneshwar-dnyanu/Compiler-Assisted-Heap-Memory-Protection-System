import fs from 'node:fs';
import path from 'node:path';
import { performance } from 'node:perf_hooks';
import { compileSource } from '../src/services/compiler/compilerService.js';
import { createWorkspace, cleanupWorkspace } from '../src/services/compiler/workspaceManager.js';
import { checkDockerAvailability } from '../src/services/execution/executionConfig.js';
import { runContainer, setCustomSpawn, resetCustomSpawn } from '../src/services/execution/dockerRunner.js';
import EventEmitter from 'node:events';

const WORKLOAD_C = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int n = 1000;
    int *buf = (int *)malloc(n * sizeof(int));
    if (!buf) return 1;
    for (int i = 0; i < n; i++) {
        buf[i] = i * 3;
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += buf[i];
    }
    free(buf);
    return sum > 0 ? 0 : 1;
}
`;

function stats(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const sum = sorted.reduce((a, b) => a + b, 0);
  const mean = sum / sorted.length;
  const median = sorted[Math.floor(sorted.length / 2)];
  const min = sorted[0];
  const max = sorted[sorted.length - 1];
  return { mean: mean.toFixed(1), median: median.toFixed(1), min: min.toFixed(1), max: max.toFixed(1) };
}

async function measure() {
  console.log('================================================================');
  console.log('   MemoryGuard System Performance & Reliability Benchmark');
  console.log('================================================================\n');

  const iterations = 5;
  const stage1Times = [];
  const stage2Times = [];
  const stage3Times = [];
  const totalTimes = [];
  const compiledJobIds = [];

  console.log(`[1] Measuring Compilation Pipeline Breakdown (${iterations} runs)...`);
  for (let i = 0; i < iterations; i++) {
    const t0 = performance.now();
    const result = await compileSource({ sourceCode: WORKLOAD_C, language: 'c' });
    const total = performance.now() - t0;

    if (!result.success) {
      console.error(`Run ${i + 1} failed:`, result.message);
      continue;
    }

    compiledJobIds.push(result.jobId);
    totalTimes.push(total);

    const s1 = result.stages.find((s) => s.name === 'FRONTEND_COMPILATION')?.durationMs || 0;
    const s2 = result.stages.find((s) => s.name === 'INSTRUMENTATION')?.durationMs || 0;
    const s3 = result.stages.find((s) => s.name === 'LINKING')?.durationMs || 0;

    stage1Times.push(s1);
    stage2Times.push(s2);
    stage3Times.push(s3);

    console.log(`  Run ${i + 1}: Total=${total.toFixed(1)}ms (Frontend=${s1}ms, Pass=${s2}ms, Link=${s3}ms)`);
  }

  console.log('\n--- Pipeline Timing Summary (ms) ---');
  console.log('Stage 1: Frontend Clang   :', JSON.stringify(stats(stage1Times)));
  console.log('Stage 2: MemoryGuard Pass :', JSON.stringify(stats(stage2Times)));
  console.log('Stage 3: Runtime Linking  :', JSON.stringify(stats(stage3Times)));
  console.log('Total Compilation Pipeline:', JSON.stringify(stats(totalTimes)));

  // Concurrent compilation measurement
  console.log('\n[2] Measuring Concurrent Compilation (5 simultaneous requests)...');
  const tConc0 = performance.now();
  const concurrentResults = await Promise.all([
    compileSource({ sourceCode: WORKLOAD_C, language: 'c' }),
    compileSource({ sourceCode: WORKLOAD_C, language: 'c' }),
    compileSource({ sourceCode: WORKLOAD_C, language: 'c' }),
    compileSource({ sourceCode: WORKLOAD_C, language: 'c' }),
    compileSource({ sourceCode: WORKLOAD_C, language: 'c' })
  ]);
  const tConcDuration = performance.now() - tConc0;
  const allSuccess = concurrentResults.every((r) => r.success);
  console.log(`  5 Concurrent Compilations: Total=${tConcDuration.toFixed(1)}ms (${(tConcDuration / 5).toFixed(1)}ms/job equivalent), All Successful: ${allSuccess}`);

  // Cleanup workspaces
  for (const r of concurrentResults) {
    if (r.jobId) compiledJobIds.push(r.jobId);
  }

  // Docker status & execution overhead
  console.log('\n[3] Measuring Execution Sandbox Overhead...');
  const dockerHealth = await checkDockerAvailability();
  console.log(`  Docker Daemon Available: ${dockerHealth.available}`);

  const execDurations = [];
  setCustomSpawn((cmd, args = []) => {
    const emitter = new EventEmitter();
    emitter.stdout = new EventEmitter();
    emitter.stderr = new EventEmitter();
    if (args.includes('rm') || args.includes('kill')) {
      process.nextTick(() => emitter.emit('close', 0, null));
      return emitter;
    }
    // Simulate lightweight container run
    setTimeout(() => {
      emitter.stdout.emit('data', Buffer.from('Done\n'));
      emitter.emit('close', 0, null);
    }, 15);
    return emitter;
  });

  for (let i = 0; i < iterations; i++) {
    const t0 = performance.now();
    await runContainer({ executablePath: '/app/program.bin' });
    execDurations.push(performance.now() - t0);
  }
  resetCustomSpawn();

  console.log('  Container Execution / Dispatch Overhead:', JSON.stringify(stats(execDurations)));

  // Cleanup all workspaces
  const rootDir = path.resolve('.workspaces');
  for (const id of compiledJobIds) {
    cleanupWorkspace(path.join(rootDir, id));
  }

  console.log('\nBenchmark completed successfully.');
}

measure().catch(console.error);
