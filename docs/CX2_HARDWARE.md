# TI-Nspire CX II Hardware Documentation (Aladdin SoC)

The "Aladdin" SoC used in the TI-Nspire CX II is a custom System-on-Chip composed of mixed IP cores from ARM, Faraday Technology, and Synopsys.

> **Reference**: [Hackspire Wiki - Memory-mapped I/O ports on CX II](https://hackspire.org/index.php?title=Memory-mapped_I%2FO_ports_on_CX_II)

## Architecture Overview

- **CPU**: ARM926EJ-S
- **Bus**: AMBA (AHB/APB)
- **RAM**: 64MB DDR (MEMC-FTDDR3030)

## Peripheral Map

| Address | Controller | IP Core | Vendor |
|---------|------------|---------|--------|
| `0x90000000` | GPIO | Unknown | - |
| `0x90010000` | Fast Timer | SP804 | ARM |
| `0x90020000` | Serial UART 1 | PL011 | ARM |
| `0x90030000` | Fastboot RAM | 4KB SRAM | - |
| `0x90040000` | LCD SPI | FTSSP010 | Faraday |
| `0x90050000` | Touchpad I2C | Designware I2C | Synopsys |
| `0x90060000` | Watchdog | SP805 | ARM |
| `0x90070000` | Serial UART 2 | PL011 | ARM |
| `0x90080000` | Cradle SPI | FTSSP010 | Faraday |
| `0x90090000` | RTC | PL031-like | ARM |
| `0x900A0000` | Miscellaneous | - | TI |
| `0x900B0000` | ADC | FTADCC010 | Faraday |
| `0x900C0000` | Timer 1 | SP804 | ARM |
| `0x900D0000` | Timer 2 | SP804 | ARM |
| `0x900E0000` | Keypad | Unknown | - |
| `0x90120000` | SDRAM Ctrl | FTDDR3030 | Faraday |
| `0x90130000` | Backlight PWM | Unknown | TI |
| `0x90140000` | PMU | Aladdin PMU | TI/Faraday |
| `0xA8000000` | Magic VRAM | HW Transposition | TI |
| `0xB0000000` | USB (top) | FOTG210 | Faraday |
| `0xB4000000` | USB (bottom) | FOTG210 | Faraday |
| `0xB8000000` | SPI NAND | FTSPI020 | Faraday |
| `0xBC000000` | DMA | FTDMAC020 (PL080 derivative) | Faraday |
| `0xC0000000` | LCD | PL111 | ARM |
| `0xC8010000` | Triple DES | Unknown | - |
| `0xCC000000` | SHA-256 | Unknown | - |
| `0xDC000000` | Interrupt Ctrl | Unknown | - |

## Key Details

### Magic VRAM (0xA8000000)
Per Hackspire: *"Written data is X-Y swapped and rotated, so that writing a 320x240 image with (0/0) at the top left results in a 320x320 image in the right orientation for the LCD."*

This is hardware transposition, not software.

### PMU (0x90140000)
- `0x90140050`: Peripheral power gating (bit 9=DES, bit 10=SHA256, bit 13=Watchdog, bit 26=I2C)
- `0x90140800+`: TI-specific ASIC/eFuse registers

### LCD Backlight (0x90130000)
- `0x90130014`: Brightness (0=brightest, 225=darkest)
- `0x90130018`: Enable (write 255)
