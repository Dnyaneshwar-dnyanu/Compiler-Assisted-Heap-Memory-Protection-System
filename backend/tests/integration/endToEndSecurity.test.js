import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import request from 'supertest';
import EventEmitter from 'node:events';
import app from '../../src/app.js';
import { setCustomSpawn, resetCustomSpawn } from '../../src/services/execution/dockerRunner.js';

describe('End-to-End Security & Compiler Regression Suite', () => {

  // ---------------------------------------------------------------------------
  // 1. Heap Out-of-Bounds Write (C)
  // ---------------------------------------------------------------------------
  it('should compile and detect heap out-of-bounds write in C', async () => {
    const sourceCode = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *buf = (int *)malloc(4 * sizeof(int)); // 16 bytes
    if (!buf) return 1;
    buf[0] = 10;
    buf[3] = 40; // valid
    buf[4] = 99; // VIOLATION: index 4 is offset 16 (out-of-bounds for 16-byte alloc)
    free(buf);
    return 0;
}
`;
    // 1. Compile
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    assert.equal(compileRes.body.success, true);
    const { jobId, diagnostics, artifacts } = compileRes.body.data;
    assert.ok(jobId);
    assert.equal(artifacts.executableAvailable, true);
    assert.ok(diagnostics.summary.checksInserted >= 3);
    assert.ok(diagnostics.summary.allocatorCallsRewritten >= 2);

    // 2. Execute via isolated container simulation
    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      emitter.kill = () => {
        process.nextTick(() => emitter.emit('close', 134, 'SIGABRT'));
      };
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x55d1a8f9c2b0, access_size=4 bytes\nAborted\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
      assert.equal(execRes.body.data.execution.terminationSignal, 'SIGABRT');
      assert.ok(execRes.body.data.diagnostics.violation);
      assert.equal(execRes.body.data.diagnostics.violation.detected, true);
      assert.equal(execRes.body.data.diagnostics.violation.violatingPointer, '0x55d1a8f9c2b0');
      assert.equal(execRes.body.data.diagnostics.violation.accessSizeBytes, 4);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 2. Heap Out-of-Bounds Read
  // ---------------------------------------------------------------------------
  it('should compile and detect heap out-of-bounds read', async () => {
    const sourceCode = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char *str = (char *)malloc(10);
    if (!str) return 1;
    for (int i = 0; i < 10; i++) str[i] = 'A' + i;
    char val = str[15]; // VIOLATION: out-of-bounds read at offset 15
    free(str);
    return (int)val;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    assert.equal(compileRes.body.success, true);
    const { jobId, diagnostics } = compileRes.body.data;
    assert.ok(diagnostics.summary.checksInserted >= 2);

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x7ffe1020304f, access_size=1 bytes\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
      assert.equal(execRes.body.data.diagnostics.violation.accessSizeBytes, 1);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 3. Heap Use-After-Free (C and C++)
  // ---------------------------------------------------------------------------
  it('should compile and detect heap use-after-free in C', async () => {
    const sourceCode = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int *p = (int *)malloc(sizeof(int));
    if (!p) return 1;
    *p = 100;
    free(p);
    *p = 200; // VIOLATION: Use-After-Free
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId, diagnostics } = compileRes.body.data;
    assert.ok(diagnostics.summary.checksInserted >= 2);

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x5555555592a0, access_size=4 bytes\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
    } finally {
      resetCustomSpawn();
    }
  });

  it('should compile and detect heap use-after-free in C++', async () => {
    const sourceCode = `
#include <iostream>
#include <cstdlib>

int main() {
    int *num = static_cast<int *>(std::malloc(sizeof(int)));
    if (!num) return 1;
    *num = 42;
    std::free(num);
    *num = 999; // VIOLATION: C++ Use-After-Free
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'cpp' });

    assert.equal(compileRes.status, 200);
    const { jobId, diagnostics } = compileRes.body.data;
    assert.ok(diagnostics.summary.checksInserted >= 2);
    assert.ok(diagnostics.summary.allocatorCallsRewritten >= 2);

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.from(
          'MemoryGuard: VIOLATION DETECTED! Invalid access to ptr=0x562300112230, access_size=4 bytes\n'
        ));
        emitter.emit('close', 134, 'SIGABRT');
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'violation_detected');
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 4. Valid Interior Pointer Access
  // ---------------------------------------------------------------------------
  it('should compile and safely validate interior pointer access', async () => {
    const sourceCode = `
#include <stdio.h>
#include <stdlib.h>

struct Record {
    int id;
    char name[32];
    double score;
};

int main(void) {
    struct Record *rec = (struct Record *)malloc(sizeof(struct Record));
    if (!rec) return 1;
    rec->id = 7;
    // Interior pointer to score field
    double *scorePtr = &rec->score;
    *scorePtr = 99.5;
    printf("Score: %.1f\\n", *scorePtr);
    free(rec);
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId, diagnostics } = compileRes.body.data;
    assert.ok(diagnostics.summary.checksInserted >= 2);

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.from('Score: 99.5\n'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 0);
      assert.ok(execRes.body.data.execution.stdout.includes('Score: 99.5'));
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 5. Valid Heap Array Access
  // ---------------------------------------------------------------------------
  it('should compile and validate valid heap array traversal', async () => {
    const sourceCode = `
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int n = 10;
    int *arr = (int *)calloc(n, sizeof(int));
    if (!arr) return 1;
    for (int i = 0; i < n; i++) {
        arr[i] = i * 10;
    }
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];
    }
    printf("Sum: %d\\n", sum);
    free(arr);
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId, diagnostics } = compileRes.body.data;
    assert.ok(diagnostics.summary.allocatorCallsRewritten >= 2);
  });

  // ---------------------------------------------------------------------------
  // 6. Stack-Only Memory Access (Classification Verification)
  // ---------------------------------------------------------------------------
  it('should classify stack-only accesses as NON_HEAP and insert 0 checks', async () => {
    const sourceCode = `
#include <stdio.h>

int main(void) {
    int stack_array[10];
    for (int i = 0; i < 10; i++) {
        stack_array[i] = i * 2;
    }
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += stack_array[i];
    }
    printf("Stack sum: %d\\n", sum);
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { diagnostics } = compileRes.body.data;
    // Stack accesses must NOT be instrumented as heap
    assert.equal(diagnostics.summary.checksInserted, 0);
    assert.equal(diagnostics.summary.heapAccessesIdentified, 0);
    assert.ok(diagnostics.summary.nonHeapSkipped > 0);
  });

  // ---------------------------------------------------------------------------
  // 7. Global-Only Memory Access
  // ---------------------------------------------------------------------------
  it('should classify global-only accesses as NON_HEAP and insert 0 checks', async () => {
    const sourceCode = `
#include <stdio.h>

static int global_buffer[16] = {0};

int main(void) {
    global_buffer[0] = 42;
    global_buffer[15] = 99;
    printf("Global: %d, %d\\n", global_buffer[0], global_buffer[15]);
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { diagnostics } = compileRes.body.data;
    assert.equal(diagnostics.summary.checksInserted, 0);
    assert.equal(diagnostics.summary.heapAccessesIdentified, 0);
    assert.ok(diagnostics.summary.nonHeapSkipped > 0);
  });

  // ---------------------------------------------------------------------------
  // 8. Infinite Loop Timeout
  // ---------------------------------------------------------------------------
  it('should terminate timed-out container execution and return 504 TIMEOUT_ERROR', async () => {
    const sourceCode = `
int main(void) {
    while (1) {}
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId } = compileRes.body.data;

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      emitter.kill = () => {
        process.nextTick(() => emitter.emit('close', 137, 'SIGKILL'));
      };
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({
          jobId,
          customConfig: { executionTimeoutMs: 20 }
        });

      assert.equal(execRes.status, 504);
      assert.equal(execRes.body.success, false);
      assert.equal(execRes.body.error.code, 'TIMEOUT_ERROR');
      assert.equal(execRes.body.error.details.timedOut, true);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 9. Excessive Stdout & Output Truncation
  // ---------------------------------------------------------------------------
  it('should truncate excessive container stdout and set output_truncated status', async () => {
    const sourceCode = `
#include <stdio.h>
int main(void) {
    printf("Flood output...\\n");
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId } = compileRes.body.data;

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stdout.emit('data', Buffer.alloc(500, 'Z'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({
          jobId,
          customConfig: { maxOutputBytes: 100 }
        });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'output_truncated');
      assert.equal(execRes.body.data.execution.outputTruncated, true);
      assert.equal(execRes.body.data.execution.stdout.length, 100);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 10. Excessive Stderr
  // ---------------------------------------------------------------------------
  it('should truncate excessive container stderr correctly', async () => {
    const sourceCode = `
#include <stdio.h>
int main(void) {
    fprintf(stderr, "Log error...\\n");
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId } = compileRes.body.data;

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.stderr.emit('data', Buffer.alloc(600, 'E'));
        emitter.emit('close', 0, null);
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({
          jobId,
          customConfig: { maxOutputBytes: 120 }
        });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'output_truncated');
      assert.equal(execRes.body.data.execution.stderr.length, 120);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 11. Nonzero Application Exit
  // ---------------------------------------------------------------------------
  it('should report no_violations_detected for normal nonzero application exit', async () => {
    const sourceCode = `
#include <stdlib.h>
int main(void) {
    exit(42);
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 200);
    const { jobId } = compileRes.body.data;

    setCustomSpawn((cmd, args = []) => {
      const emitter = new EventEmitter();
      emitter.stdout = new EventEmitter();
      emitter.stderr = new EventEmitter();
      if (args.includes('rm') || args.includes('kill')) {
        process.nextTick(() => emitter.emit('close', 0, null));
        return emitter;
      }
      process.nextTick(() => {
        emitter.emit('close', 42, null);
      });
      return emitter;
    });

    try {
      const execRes = await request(app)
        .post('/api/execute')
        .send({ jobId });

      assert.equal(execRes.status, 200);
      assert.equal(execRes.body.data.status, 'no_violations_detected');
      assert.equal(execRes.body.data.execution.exitCode, 42);
      assert.equal(execRes.body.data.diagnostics.violation, null);
    } finally {
      resetCustomSpawn();
    }
  });

  // ---------------------------------------------------------------------------
  // 12. Invalid C Syntax
  // ---------------------------------------------------------------------------
  it('should return 422 COMPILATION_ERROR on syntax error in C', async () => {
    const sourceCode = `
int main(void) {
    this is completely invalid syntax !;
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'c' });

    assert.equal(compileRes.status, 422);
    assert.equal(compileRes.body.success, false);
    assert.equal(compileRes.body.error.code, 'COMPILATION_ERROR');
    assert.equal(compileRes.body.error.stage, 'FRONTEND_COMPILATION');
    assert.ok(compileRes.body.error.diagnostics.includes('error:'));
  });

  // ---------------------------------------------------------------------------
  // 13. Invalid C++ Syntax
  // ---------------------------------------------------------------------------
  it('should return 422 COMPILATION_ERROR on syntax error in C++', async () => {
    const sourceCode = `
#include <iostream>
int main() {
    std::cout << missing_variable << std::endl;
    return 0;
}
`;
    const compileRes = await request(app)
      .post('/api/compile')
      .send({ sourceCode, language: 'cpp' });

    assert.equal(compileRes.status, 422);
    assert.equal(compileRes.body.success, false);
    assert.equal(compileRes.body.error.code, 'COMPILATION_ERROR');
    assert.ok(compileRes.body.error.diagnostics.includes('error:'));
  });
});
