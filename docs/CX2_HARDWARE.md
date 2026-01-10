# TI-Nspire CX II Hardware Documentation (Aladdin SoC)

The "Aladdin" SoC used in the TI-Nspire CX II is a custom System-on-Chip composed primarily of Faraday Technology IP cores. This document maps emulated peripherals and "mysteries" to their known Faraday counterparts.

## Architecture Overview

- **CPU**: ARM926EJ-S variant.
- **Bus**: AMBA (AHB/APB).

## Peripheral Mapping

### LCD Controller (FTLCDC210)
**Status**: Partial / Hacky
**Address**: `0x900C0000` (Registers) / `0xA8000000` (Magic VRAM)

*   **Identified IP**: Faraday FTLCDC210.
*   **Key Feature**: Hardware Windowing (HWW). Supports multiple overlapping windows (overlays) blended in real-time.
*   **"Magic VRAM"**: The region at `0xA8000000` is used for these hardware windows.
*   **OS Behavior**: The stock TI-Nspire OS (CX II) renders the mouse cursor in **software** directly into the framebuffer (at `0xA8000000`), bypassing the hardware cursor registers. This explains why register spies remain silent during mouse movement.
*   **Current Hack**: Firebird treats `0xA8000000` as standard RAM and only renders the primary framebuffer. This works for the OS because it manually draws the cursor pixels there.
*   **Optimization Target**: Real hardware supports automatic transposition (rotation). The current C++ manual rotation loop in `lcd_cx_w_draw_frame` can be replaced with proper FTLCDC210 windowing logic.

### DMA Controller
**Status**: Liked FTDMAC020 (Mapped / Unused)
**Address**: `0x90100000` (original CX) / `0xBC000000` (Aladdin)

*   **Identified IP**: Likely **FTDMAC020** (standard Faraday suite).
*   **Verification**: Probes at `0xBC000000` show **no activity** during OS boot or GUI operation. The OS appears to use PIO (CPU) for most transfers in its current state.
*   **Mapping**: Firebird maps `0xBC00xxxx` to a generic DMA handler compatible with FTDMAC020.

### Power Management Unit (PMU)
- **Base Address**: `0x90140000`
- **Faraday IP**: `FTPMU010`
- **Register Map**:
    | Offset | Known Name | Aladdin usage | Status |
    |--------|------------|---------------|--------|
    | 0x00   | IDNMBR0    | Wakeup Reason | Verified |
    | 0x08   | OSCC       | Osc Control   | Researching |
    | 0x0C   | PMODE      | Power Mode    | Researching (Sleep) |
    | 0x20   | PMSR       | Status Reg    | Used as `disable[0]` |
    | 0x24   | PGSR       | Group Status  | Used for `int_state` |
    | 0x30   | PDLLCR0    | PLL Control 0 | Used for `clocks` |

> [!NOTE]
> The registers in the `0x800` range (e.g., `0x808`, `0x80C`, `0x810`) appear to be TI-specific ASIC extensions or "efuse" registers not part of the standard `FTPMU010` core.

### DMA Controller
- **Base Address**: `0x90020000`
- **Faraday IP**: `FTDMAC020`
- **Structure**: 8-channel prioritized DMA controller.
- **Status**: Partially implemented in `cx2.cpp`.

### USB Controller
- **Base Address**: `0x90110000` (EHCI) / `0x90111000` (OTG)
- **Faraday IP**: `FTOTG210` (USB 2.0 OTG)
- **Status**: Verified via U-Boot headers.

### LCD Controller
- **Faraday IP**: Likely `FTLCDC210`.
- **"Magic VRAM"**: The CX II uses a display-list based or windowed memory mapping for the LCD. 
- **Status**: TODO item #9.

## Known Mysteries
- **`0x9014080C`**: ASIC user flags, must match `0x80E0` field in OS image.
- **`0x90140810` / Bit 8**: Cleared when the physical **ON** key is pressed.
