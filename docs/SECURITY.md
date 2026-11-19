# MemoryGuard Security Model & Threat Confinement Specification

This document details the security architecture, threat model, container sandboxing parameters, and defense-in-depth mechanisms implemented across MemoryGuard.

---

## 1. Threat Model & Adversarial Assumptions

MemoryGuard accepts arbitrary user-authored C and C++ source code via a public HTTP API, compiles it using Clang and LLVM, and executes the resulting binary.

### Identified Threat Vectors

1. **Malicious Code Execution**:
   - Source code containing system calls to spawn interactive shells (`execve("/bin/sh")`).
   - Fork bombs (`while(1) fork();`) attempting host resource exhaustion.
   - Excessive memory allocation (`malloc(1ULL << 40)`) attempting host Out-Of-Memory (OOM) crashes.
   - Infinite compute loops (`while(1);`) attempting CPU starvation.
2. **Network Exfiltration & Probing**:
   - Sockets connecting to private internal infrastructure (`192.168.x.x`, `10.x.x.x`, AWS/GCP metadata endpoints `169.254.169.254`).
   - Command-and-control beacons or outbound reverse shells.
3. **Filesystem Escape & Tampering**:
   - Host root traversal via symlink creation (`ln -s /etc/passwd link`).
   - Overwriting compiler binaries, system libraries, or other concurrent job workspaces.
   - Reading or altering artifacts belonging to other user sessions.
4. **API Parameter Injection**:
   - HTTP request bodies attempting to pass Docker CLI flags (`--privileged`, `--volume /:/host`).
   - Overriding timeout limits, CPU caps, or memory limits through API manipulation.

---

## 2. Hardened Compiler Sandbox (`memoryguard-compiler:latest`)

The compilation phase translates untrusted source code into LLVM IR, executes the `MemoryGuardPass.so` plugin via `opt-18`, and links the binary via `clang-18`. Compilation is strictly isolated from the host.

### Container Security Configuration

| Flag / Parameter | Enforced Value | Security Rationale |
|:---|:---|:---|
| `--network` | `none` | Disables all network interfaces except the internal loopback. Precludes network exfiltration during compilation. |
| `--read-only` | Active | Root filesystem is mounted read-only. Compiler processes cannot write to `/bin`, `/usr`, `/lib`, or system directories. |
| `--cap-drop` | `ALL` | Drops all Linux capabilities (including `CAP_SYS_ADMIN`, `CAP_NET_ADMIN`, `CAP_DAC_OVERRIDE`). |
| `--security-opt` | `no-new-privileges:true` | Prevents setuid/setgid binaries from elevating privileges within the container. |
| `--user` | `10001:10001` | Runs as unprivileged user `mgcompiler`. Never executes as `root`. |
| `--memory` | `512m` | Limits compiler process to 512 MB. Prevents memory exhaustion during macro expansion or template instantiation. |
| `--cpus` | `1.5` | Throttles CPU usage to 1.5 cores. Prevents compiler DoS against host. |
| `--pids-limit` | `128` | Limits maximum concurrent processes to 128 to block fork attacks. |
| `--tmpfs` | `/tmp:rw,noexec,nosuid,size=64m` | Provides bounded temporary storage without allowing execution of binaries in `/tmp`. |

> [!NOTE]
> **Container User vs. Rootless Docker Clarification**:
> MemoryGuard enforces in-container non-root execution via `--user 10001:10001`. This guarantees that processes inside the container cannot exercise root privileges within the container namespace. This is distinct from "Rootless Docker" (where the host Docker daemon `dockerd` runs without root privileges). MemoryGuard operates with standard Docker installations while strictly isolating container processes.

### Fail-Closed Compilation Policy

- Production compilation **must always** execute inside the Docker container.
- If the Docker daemon is down, unreachable, or fails, the backend returns a controlled `503 Service Unavailable` error.
- **Zero Host Fallback**: Under no circumstances does production code fall back to executing `clang`, `opt`, or shell commands directly on the host machine.
- Test overrides are strictly disabled in production mode (`process.env.NODE_ENV === 'production'`).

---

## 3. Hardened Runtime Execution Sandbox (`memoryguard-runner:latest`)

Compiled binaries execute inside a dedicated, ultra-minimal container environment.

### Container Security Configuration

| Flag / Parameter | Enforced Value | Security Rationale |
|:---|:---|:---|
| `--network` | `none` | Absolute network isolation. Untrusted binaries cannot open sockets or access local/cloud networks. |
| `--read-only` | Active | Executables cannot write to the container filesystem or persist state. |
| `--cap-drop` | `ALL` | Complete removal of Linux kernel capabilities. |
| `--security-opt` | `no-new-privileges:true` | Disallows privilege escalation. |
| `--user` | `10001:10001` | Runs as unprivileged user `mgrunner`. |
| `--memory` | `128m` | Caps memory at 128 MB. Unchecked memory allocations immediately trigger container OOM kill. |
| `--cpus` | `1.0` | Bounds execution to 1 CPU core. |
| `--pids-limit` | `32` | Blocks fork bombs. Attempting to spawn more than 32 threads/processes immediately fails with `EAGAIN`. |
| Volume Mount | Read-only (`program.bin:ro`) | Only the target executable is mounted into the container at `/app/program.bin:ro`. No compiler tools or source code are visible. |

### Execution Timeout & Output Bounding

- **Hard Wall-Clock Timeout**: Default 5,000 ms. If a container does not terminate within the deadline, a `SIGKILL` is issued directly to the Docker process, followed by `docker kill` and `docker rm -f`.
- **Output Truncation**: Standard output and standard error streams are strictly capped at 64 KB (`MAX_OUTPUT_BYTES = 64 * 1024`). Infinite loops emitting output cannot exhaust backend memory.

---

## 4. Immutable Security Architecture (`IMMUTABLE_SECURITY`)

To prevent API callers from tampering with sandbox parameters, the backend enforces an immutable configuration architecture:

```javascript
// backend/src/services/execution/dockerRunner.js
const IMMUTABLE_SECURITY = Object.freeze({
  network: 'none',
  readOnly: true,
  dropCapabilities: 'ALL',
  user: '10001:10001',
  securityOpt: 'no-new-privileges:true'
});
```

- Request bodies (`req.body`) are strictly validated against JSON schemas.
- Custom Docker arguments (`customConfig`) are ignored or rejected by production controllers.
- Even if an attacker attempts to inject parameters via the API, the execution runner enforces `IMMUTABLE_SECURITY` overrides, ensuring security-critical flags can never be disabled.

---

## 5. Workspace Confinement & Path Traversal Protections

Every compilation request receives a uniquely generated UUIDv4 workspace: `/tmp/memoryguard-workspaces/{jobId}`.

### Multi-Layer Path Traversal Defense

1. **Job ID Format Validation**:
   - Every `jobId` is validated against a strict UUID regex:
     ```javascript
     const UUID_REGEX = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
     ```
   - Characters such as `..`, `/`, `\`, `%00`, or path separators are rejected immediately with `400 Bad Request`.
2. **Symlink Defense (`lstatSync`)**:
   - Prior to accessing any workspace or executable file, `workspaceManager.js` and `executionService.js` call `fs.lstatSync()`.
   - If `isSymbolicLink()` returns `true`, the operation is aborted immediately with an unhandled security exception.
3. **Canonical Path Containment (`fs.realpathSync`)**:
   - The resolved real path of the target artifact must reside strictly within the workspace root directory:
     ```javascript
     const realTarget = fs.realpathSync(targetPath);
     const realRoot = fs.realpathSync(rootDir);
     if (!realTarget.startsWith(realRoot + path.sep)) {
       throw new Error('Path traversal detected');
     }
     ```
4. **Compilation Manifest Verification (`manifest.json`)**:
   - Upon successful compilation, the compiler service creates a cryptographic completion record: `manifest.json`.
   - The manifest records `jobId`, `timestamp`, `sourceHash`, and `binaryStatus: "READY"`.
   - The execution service requires a valid, matching `manifest.json` before allowing any binary to run. If a compilation fails or is aborted, any stale executable or manifest is immediately deleted.

---

## 6. Security Assurance & Non-Claims

> [!WARNING]
> MemoryGuard is a compiler-assisted heap boundary checking system and sandboxed execution environment. It is **not** a formal mathematical memory-safety proof system.

- **Scope**: Heap memory allocations (`malloc`, `calloc`, `realloc`, `free`, C++ `new`/`delete`).
- **Non-Coverage**: Stack buffer overflows, global variable overflows, and unaligned type-punning are out of scope for heap boundary checking (though host security remains protected by the Docker sandbox).
- **TOCTOU Races**: In multi-threaded programs where one thread deallocates a buffer between another thread's `mg_check_access()` check and its raw CPU load/store instruction, a data race may still occur at the machine level.
