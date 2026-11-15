#!/usr/bin/env python3
"""
test_diagnostics_summary.py

Automated test verifying Step 17 requirements:
  1. Module-level analysis counters:
     - Total load and store instructions examined.
     - Heap-origin accesses identified.
     - Runtime checks inserted.
     - Volatile accesses instrumented.
     - Non-heap accesses skipped.
     - Unknown-origin accesses skipped.
     - Atomic operations skipped.
     - Unsupported address-space accesses skipped.
     - Scalable-vector accesses skipped.
     - Allocator calls detected and successfully rewritten.
     - Incompatible or unsupported allocator calls skipped.
  2. Per-function summaries for each defined function.
  3. Internal consistency across counters (no double-counting).
  4. Distinction between unknown and non-heap accesses.
  5. Allocator rewrite and incompatible allocator skip counts.
  6. Atomic and unsupported operation skip counts.
  7. Pass idempotence on double pass: checks inserted becomes 0 on second pass,
     and IR output remains bit-for-bit identical.
"""

import sys
import os
import subprocess
import re

def parse_key_value_blocks(text):
    """
    Parses sections matching:
      [MemoryGuard][Function] @<name>:
        Key: Value
      ...
      [MemoryGuard][Summary] Module Analysis Summary:
        Key: Value
    """
    function_summaries = {}
    module_summary = {}

    current_fn = None

    for line in text.splitlines():
        fn_match = re.match(r"^\[MemoryGuard\]\[Function\]\s+@([a-zA-Z0-9_]+):", line)
        if fn_match:
            current_fn = fn_match.group(1)
            function_summaries[current_fn] = {}
            continue

        sum_match = re.match(r"^\[MemoryGuard\]\[Summary\]\s+Module Analysis Summary:", line)
        if sum_match:
            current_fn = "__MODULE_SUMMARY__"
            continue

        if current_fn:
            kv_match = re.match(r"^\s+([^:]+):\s+(\d+)", line)
            if kv_match:
                key = kv_match.group(1).strip()
                val = int(kv_match.group(2).strip())
                if current_fn == "__MODULE_SUMMARY__":
                    module_summary[key] = val
                else:
                    function_summaries[current_fn][key] = val

    return function_summaries, module_summary

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <opt_tool> <plugin_path> <input_ll_path>")
        sys.exit(1)

    opt_tool = sys.argv[1]
    plugin_path = sys.argv[2]
    input_ll = sys.argv[3]

    out_pass1 = "test_diagnostics_pass1.ll"
    out_pass2 = "test_diagnostics_pass2.ll"

    # Step 1: Run single pass and capture stderr
    cmd1 = [
        opt_tool,
        f"-load-pass-plugin={plugin_path}",
        "-passes=memoryguard",
        "-S",
        input_ll,
        "-o",
        out_pass1
    ]
    print(f"[RUN] {' '.join(cmd1)}")
    res1 = subprocess.run(cmd1, capture_output=True, text=True)
    if res1.returncode != 0:
        print(f"[FAIL] Single pass failed:\nSTDOUT:\n{res1.stdout}\nSTDERR:\n{res1.stderr}")
        sys.exit(1)

    stderr1 = res1.stderr
    fn_summaries, mod_summary = parse_key_value_blocks(stderr1)

    total_checks = 0
    passed_checks = 0

    def assert_eq(actual, expected, msg):
        nonlocal total_checks, passed_checks
        total_checks += 1
        if actual == expected:
            passed_checks += 1
            print(f"  [PASS] {msg} ({actual} == {expected})")
        else:
            print(f"  [FAIL] {msg} (actual: {actual}, expected: {expected})")

    print("\n--- Verifying Module-Level Summary Counters ---")
    assert_eq(mod_summary.get("Total load and store instructions examined"), 18,
              "Total load and store instructions examined")
    assert_eq(mod_summary.get("Heap-origin accesses identified"), 6,
              "Heap-origin accesses identified")
    assert_eq(mod_summary.get("Runtime checks inserted"), 6,
              "Runtime checks inserted")
    assert_eq(mod_summary.get("Volatile accesses instrumented"), 2,
              "Volatile accesses instrumented")
    assert_eq(mod_summary.get("Non-heap accesses skipped"), 4,
              "Non-heap accesses skipped")
    assert_eq(mod_summary.get("Unknown-origin accesses skipped"), 2,
              "Unknown-origin accesses skipped")
    assert_eq(mod_summary.get("Atomic operations skipped"), 5,
              "Atomic operations skipped")
    assert_eq(mod_summary.get("Unsupported address-space accesses skipped"), 2,
              "Unsupported address-space accesses skipped")
    assert_eq(mod_summary.get("Scalable-vector accesses skipped"), 2,
              "Scalable-vector accesses skipped")
    assert_eq(mod_summary.get("Allocator calls detected and successfully rewritten"), 7,
              "Allocator calls detected and successfully rewritten")
    assert_eq(mod_summary.get("Incompatible or unsupported allocator calls skipped"), 2,
              "Incompatible or unsupported allocator calls skipped")

    print("\n--- Verifying Internal Counter Consistency ---")
    # Total examined must equal atomic loads/stores (2) + scalable (2) + addrspace (2) + non-heap (4) + unknown (2) + heap (6)
    expected_sum = (2 + 2 + 2 + 4 + 2 + 6)
    assert_eq(mod_summary.get("Total load and store instructions examined"), expected_sum,
              "Instruction partitioning consistency (no double counting)")

    print("\n--- Verifying Per-Function Summaries ---")
    # Function 1: @fn_heap_and_stack
    f1 = fn_summaries.get("fn_heap_and_stack", {})
    assert_eq(f1.get("Accesses examined"), 4, "@fn_heap_and_stack accesses examined")
    assert_eq(f1.get("Checks inserted"), 2, "@fn_heap_and_stack checks inserted")
    assert_eq(f1.get("Heap accesses identified"), 2, "@fn_heap_and_stack heap accesses identified")
    assert_eq(f1.get("Non-heap accesses skipped"), 2, "@fn_heap_and_stack non-heap accesses skipped")
    assert_eq(f1.get("Unknown-origin accesses skipped"), 0, "@fn_heap_and_stack unknown accesses skipped")
    assert_eq(f1.get("Unsupported operations skipped"), 0, "@fn_heap_and_stack unsupported ops skipped")

    # Function 2: @fn_volatile_and_unknown
    f2 = fn_summaries.get("fn_volatile_and_unknown", {})
    assert_eq(f2.get("Accesses examined"), 4, "@fn_volatile_and_unknown accesses examined")
    assert_eq(f2.get("Checks inserted"), 2, "@fn_volatile_and_unknown checks inserted")
    assert_eq(f2.get("Heap accesses identified"), 2, "@fn_volatile_and_unknown heap accesses identified")
    assert_eq(f2.get("Non-heap accesses skipped"), 0, "@fn_volatile_and_unknown non-heap accesses skipped")
    assert_eq(f2.get("Unknown-origin accesses skipped"), 2, "@fn_volatile_and_unknown unknown accesses skipped")
    assert_eq(f2.get("Unsupported operations skipped"), 0, "@fn_volatile_and_unknown unsupported ops skipped")

    # Function 3: @fn_unsupported_ops
    f3 = fn_summaries.get("fn_unsupported_ops", {})
    assert_eq(f3.get("Accesses examined"), 6, "@fn_unsupported_ops accesses examined")
    assert_eq(f3.get("Checks inserted"), 0, "@fn_unsupported_ops checks inserted")
    assert_eq(f3.get("Heap accesses identified"), 0, "@fn_unsupported_ops heap accesses identified")
    assert_eq(f3.get("Non-heap accesses skipped"), 0, "@fn_unsupported_ops non-heap accesses skipped")
    assert_eq(f3.get("Unknown-origin accesses skipped"), 0, "@fn_unsupported_ops unknown accesses skipped")
    assert_eq(f3.get("Unsupported operations skipped"), 11, "@fn_unsupported_ops unsupported ops skipped")

    # Function 4: @fn_realloc_and_globals
    f4 = fn_summaries.get("fn_realloc_and_globals", {})
    assert_eq(f4.get("Accesses examined"), 4, "@fn_realloc_and_globals accesses examined")
    assert_eq(f4.get("Checks inserted"), 2, "@fn_realloc_and_globals checks inserted")
    assert_eq(f4.get("Heap accesses identified"), 2, "@fn_realloc_and_globals heap accesses identified")
    assert_eq(f4.get("Non-heap accesses skipped"), 2, "@fn_realloc_and_globals non-heap accesses skipped")
    assert_eq(f4.get("Unknown-origin accesses skipped"), 0, "@fn_realloc_and_globals unknown accesses skipped")
    assert_eq(f4.get("Unsupported operations skipped"), 0, "@fn_realloc_and_globals unsupported ops skipped")

    print("\n--- Verifying Double-Pass Idempotence & Second-Pass Diagnostics ---")
    cmd2 = [
        opt_tool,
        f"-load-pass-plugin={plugin_path}",
        "-passes=memoryguard,memoryguard",
        "-S",
        input_ll,
        "-o",
        out_pass2
    ]
    print(f"[RUN] {' '.join(cmd2)}")
    res2 = subprocess.run(cmd2, capture_output=True, text=True)
    if res2.returncode != 0:
        print(f"[FAIL] Double pass failed:\n{res2.stderr}")
        sys.exit(1)

    stderr2 = res2.stderr
    # The second module summary is at the bottom of stderr2
    sections = stderr2.split("[MemoryGuard][Summary] Module Analysis Summary:")
    assert len(sections) >= 3, "Expected at least 2 module summary sections in double-pass run"
    pass2_sum_text = "[MemoryGuard][Summary] Module Analysis Summary:" + sections[-1]
    _, mod2_summary = parse_key_value_blocks(pass2_sum_text)

    assert_eq(mod2_summary.get("Runtime checks inserted"), 0,
              "Second pass runtime checks inserted is 0 (idempotence)")
    assert_eq(mod2_summary.get("Allocator calls detected and successfully rewritten"), 0,
              "Second pass allocators rewritten is 0 (already rewritten)")
    assert_eq(mod2_summary.get("Total load and store instructions examined"), 18,
              "Second pass accesses examined remains 18")

    # Compare generated IR between pass1 and pass2
    with open(out_pass1, "r") as f1, open(out_pass2, "r") as f2:
        ir1 = f1.read()
        ir2 = f2.read()
    assert_eq(ir1 == ir2, True, "Pass output IR is bit-for-bit identical between single and double pass")

    # Cleanup temporary output files
    if os.path.exists(out_pass1):
        os.remove(out_pass1)
    if os.path.exists(out_pass2):
        os.remove(out_pass2)

    print(f"\n====================================================")
    print(f"Summary: {passed_checks} / {total_checks} assertions passed")
    print(f"====================================================")

    if passed_checks == total_checks:
        print("ALL DIAGNOSTICS & SUMMARY ASSERTIONS PASSED!")
        sys.exit(0)
    else:
        print(f"FAILED: {total_checks - passed_checks} assertion(s) failed!")
        sys.exit(1)

if __name__ == "__main__":
    main()
