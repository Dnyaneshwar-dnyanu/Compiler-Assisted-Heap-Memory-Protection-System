#!/bin/sh
# ==============================================================================
# MemoryGuard Compiler Sandbox Runner Script
# Executes the 3-stage compilation and instrumentation pipeline inside Docker
# ==============================================================================
set -e

LANGUAGE="c"
SOURCE_FILE=""
OUTPUT_FILE="/out/program.bin"

while [ $# -gt 0 ]; do
  case "$1" in
    --lang)
      LANGUAGE="$2"
      shift 2
      ;;
    --source)
      SOURCE_FILE="$2"
      shift 2
      ;;
    --out)
      OUTPUT_FILE="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

# Resolve default source file from language if not explicitly provided
if [ -z "$SOURCE_FILE" ]; then
  if [ "$LANGUAGE" = "cpp" ]; then
    SOURCE_FILE="/src/source.cpp"
  else
    SOURCE_FILE="/src/source.c"
  fi
fi

if [ ! -f "$SOURCE_FILE" ]; then
  echo "Error: Source file $SOURCE_FILE not found in /src directory." >&2
  exit 10
fi

if [ "$LANGUAGE" = "cpp" ]; then
  COMPILER="clang++-18"
else
  COMPILER="clang-18"
fi

PLUGIN="/opt/memoryguard/lib/MemoryGuardPass.so"
RUNTIME_LIB="/opt/memoryguard/lib/libmemoryguard.a"
INCLUDE_DIR="/opt/memoryguard/include"

TMP_DIR="/tmp"
IR_FILE="$TMP_DIR/source.ll"
INST_IR_FILE="$TMP_DIR/source_inst.ll"

# ---------------------------------------------------------------------------
# Stage 1: Frontend Compilation to textual LLVM IR (with debug info)
# ---------------------------------------------------------------------------
echo "=== STAGE_START: FRONTEND_COMPILATION ==="
if ! $COMPILER -O0 -g -Xclang -disable-O0-optnone -emit-llvm -S "$SOURCE_FILE" -I "$INCLUDE_DIR" -o "$IR_FILE"; then
  echo "=== STAGE_FAILED: FRONTEND_COMPILATION ===" >&2
  exit 1
fi
echo "=== STAGE_COMPLETED: FRONTEND_COMPILATION ==="

# ---------------------------------------------------------------------------
# Stage 2: MemoryGuard LLVM Pass Instrumentation
# ---------------------------------------------------------------------------
echo "=== STAGE_START: INSTRUMENTATION ==="
export MG_STATIC_DIAGNOSTICS_PATH="/out/static_diagnostics.json"
# opt output (diagnostics & summary) is printed to stderr/stdout
if ! opt-18 -load-pass-plugin="$PLUGIN" -passes=memoryguard -S "$IR_FILE" -o "$INST_IR_FILE"; then
  echo "=== STAGE_FAILED: INSTRUMENTATION ===" >&2
  exit 2
fi
echo "=== STAGE_COMPLETED: INSTRUMENTATION ==="

# Check if static memory violations were detected by compile-time analysis
if [ -f "/out/static_diagnostics.json" ] || [ -f "$TMP_DIR/static_diagnostics.json" ]; then
  if [ -f "$TMP_DIR/static_diagnostics.json" ] && [ ! -f "/out/static_diagnostics.json" ]; then
    cp "$TMP_DIR/static_diagnostics.json" /out/static_diagnostics.json 2>/dev/null || true
  fi
  if grep -q '"severity"[[:space:]]*:[[:space:]]*"error"' /out/static_diagnostics.json 2>/dev/null; then
    echo "=== STAGE_FAILED: STATIC_MEMORY_VIOLATION ===" >&2
    echo "MemoryGuard compile-time analysis detected definite heap memory violation." >&2
    rm -f "$OUTPUT_FILE" 2>/dev/null || true
    exit 4
  fi
fi

# ---------------------------------------------------------------------------
# Stage 3: Linking with MemoryGuard Runtime
# ---------------------------------------------------------------------------
echo "=== STAGE_START: LINKING ==="
if ! $COMPILER "$INST_IR_FILE" "$RUNTIME_LIB" -pthread -lm -o "$OUTPUT_FILE"; then
  echo "=== STAGE_FAILED: LINKING ===" >&2
  exit 3
fi
echo "=== STAGE_COMPLETED: LINKING ==="

# Copy textual LLVM IR artifacts to the output volume for developer inspection
cp "$IR_FILE" /out/source.ll 2>/dev/null || true
cp "$INST_IR_FILE" /out/source_inst.ll 2>/dev/null || true
chmod 755 "$OUTPUT_FILE" 2>/dev/null || true

echo "=== COMPILATION_SUCCESS ==="
exit 0
