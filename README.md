# STM32H750 DCMI + OV2640 Capture Demo

This project configures an STM32H750 to preview RGB565 frames from an OV2640 on an ST7735 LCD and capture JPEG snapshots to SD.

## Build & Flash
- Toolchain: Arm GNU 13.3; project uses STM32Cube/HAL.
- Build: `make` (outputs `build/08-DCMI2LCD.elf`).
- Flash with your preferred SWD/JTAG method.

## Usage
- On boot, the LCD shows camera info; press K1 to start.
- Live preview: RGB565 QQVGA streamed via DCMI DMA in circular mode.
- Snapshot: press K1; DCMI switches to JPEG mode, captures, writes `PHOTO_#####.jpeg` (or `P#####.JPG` on 8.3-only cards), then returns to preview.

## Notes
- SD card must be present; errors are shown on the LCD with FatFS codes.
- DCMI JPEG bit is toggled between preview/capture; DMA mode switches circular/normal accordingly.
- Cache maintenance is applied around DMA buffers where needed.
