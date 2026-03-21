# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v4.4.0] - 2026-03-21

### Added
- Integration for **ST7565R SPI Monochrome LCD** (128x64).
- Performance-optimized initialization for **GMG12864-06D** display modules.
- New **3x3 pixel square Page Indicator** for better visibility on LCD.
- Detailed display parameter control (Bias, Resistor Ratio, ADC/COM Reverse mappings).
- Ignored `scripts/` directory in version control.

### Changed
- Migrated primary display hardware from I2C SSD1306 OLED to **SPI ST7565R LCD**.
- Updated hardware GPIO mapping for SPI:
    - **SCK**: GPIO 0
    - **MOSI**: GPIO 1
    - **CS**: GPIO 10 (Switched for safety)
    - **DC**: GPIO 2
    - **RST**: GPIO 3
- Refactored `display_manager.c` to support ST7565R specific initialization and contrast management using U8G2.

### Fixed
- Fixed display mirroring and coordinate offset issues for standard ST7565R modules.
- Resolved build errors related to manual command sending by switching to standard U8G2 HAL methods.
- Stabilized startup sequence with hardware-synchronized delays.

---
