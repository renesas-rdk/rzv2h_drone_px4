# third_party_bin — Prebuilt binaries & model weights

This directory holds prebuilt DRP-AI TVM runtime libraries and YOLOv8n
model artifacts for the Renesas RZ/V2H. These files are **not committed
to git** (see `.gitignore`) due to size/license constraints.

## Expected layout

```
third_party_bin/
├── lib/aarch64/
│   ├── libmera2_runtime.so
│   ├── libmera2_plan_io.so
│   ├── libdrp_tvm_rt.so
│   ├── libdrp_rt.so
│   ├── libacl_rt.so
│   └── libarm_compute.so
├── include/
│   └── (DRP-AI TVM runtime headers)
└── models/
    └── yolov8n/
        ├── mera.plan          (DRP-AI compiled model)
        ├── preprocess/        (DRP-AI preprocessing subgraph)
        │   ├── mera.plan
        │   └── addr_map.txt
        └── labels_coco.txt    (80 COCO class names)
```

## How to populate

### Option A — Copy from existing wheel_legged_robot project (fastest)

```bash
SRC=/path/to/wheel_legged_robot

# DRP-AI runtime libraries
cp $SRC/rzv_drp-ai_tvm/obj/build_runtime/*.so  third_party_bin/lib/aarch64/

# Model weights (already compiled for DRP-AI)
cp -r $SRC/yolov8n_drpai/  third_party_bin/models/yolov8n/
```

### Option B — Build YOLOv8n from scratch

See `docs/BUILD_YOLOV8_WEIGHTS.md` for full instructions.

### Option C — Renesas RUHMI SDK

Download and install the Renesas DRP-AI TVM (RUHMI) SDK from:
https://github.com/renesas-rz/rzv2h_drp-ai_tvm

Extract runtime libraries to `third_party_bin/lib/aarch64/`.
