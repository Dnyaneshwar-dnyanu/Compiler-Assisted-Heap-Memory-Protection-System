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
# Stage 1: Frontend Compilation to textual LLVM IR
# ---------------------------------------------------------------------------
echo "=== STAGE_START: FRONTEND_COMPILATION ==="
if ! $COMPILER -O0 -Xclang -disable-O0-optnone -emit-llvm -S "$SOURCE_FILE" -I "$INCLUDE_DIR" -o "$IR_FILE"; then
  echo "=== STAGE_FAILED: FRONTEND_COMPILATION ===" >&2
  exit 1
fi
echo "=== STAGE_COMPLETED: FRONTEND_COMPILATION ==="

# ---------------------------------------------------------------------------
# Stage 2: MemoryGuard LLVM Pass Instrumentation
# ---------------------------------------------------------------------------
echo "=== STAGE_START: INSTRUMENTATION ==="
# opt output (diagnostics & summary) is printed to stderr/stdout
if ! opt-18 -load-pass-plugin="$PLUGIN" -passes=memoryguard -S "$IR_FILE" -o "$INST_IR_FILE"; then
  echo "=== STAGE_FAILED: INSTRUMENTATION ===" >&2
  exit 2
fi
echo "=== STAGE_COMPLETED: INSTRUMENTATION ==="

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
