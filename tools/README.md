# Build & Runtime Tools

## memory/

Memory layout validation and diagnostics.

### check_sections.py

Post-build verification script that validates firmware memory regions (heap, BSS, IRQ/FIQ stacks).

**Status**: Active – runs automatically after every firmware build.

Run manually:
```bash
python3 tools/memory/check_sections.py Debug/rzv2h_px4_freertos.map
```
