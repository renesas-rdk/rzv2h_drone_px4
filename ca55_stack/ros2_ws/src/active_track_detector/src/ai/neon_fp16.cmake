# ── ARMv8.2-A FP16 NEON flags (shared between Poky + Docker builds) ─────────
# Cortex-A55 in RZ/V2H supports FP16 vector arithmetic.
# Enables __ARM_FEATURE_FP16_VECTOR_ARITHMETIC → USE_NEON_FP16=1 in yolo_detector.cpp
# Performance: postprocess 9.6ms → 1.2ms (~8x speedup)
#
# Used by:
#   src/CMakeLists.txt     (CustomXRCEAgent — Poky SDK build)
#   src/ai/CMakeLists.txt  (ai_camera_test  — Docker SDK build)
set(NEON_FP16_FLAGS "-march=armv8.2-a+fp16")
