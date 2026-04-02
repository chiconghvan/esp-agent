# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [v5.1.0] - 2026-04-02

### Added
- **Unified Query Engine**: Refactored filtering logic into a shared module (`query_engine.c`) for cross-action consistency.
- **Enhanced Mutation Filtering**: Mutate actions (`DELETE_TASK`, `UPDATE_TASK`, `COMPLETE_TASK`) now support the same powerful filter array as `QUERY_TASKS`.
- **Bulk Operations**: AI can now identify and execute bulk operations (e.g., "delete all completed tasks", "complete all overdue events") using specialized filters.

### Changed
- Refactored `action_query.c`, `action_delete.c`, `action_complete.c`, and `action_update.c` to use the common Query Engine.
- Updated `PROMPT_B2_MUTATE` to support complex filter structures.
- Improved code maintainability by removing redundant filtering logic across handlers.

### Fixed
- Fixed a bug where AI failed to identify bulk deletion intent correctly by adding explicit status filtering rules.
- Resolved unused variable warnings (`TAG`, `all_entries`) during the build process.

## [v5.0.0] - 2026-04-02

### Added
- **Automatic Task Cleanup**: Implemented logic to permanently delete non-recurring completed tasks after 3 days.
- **Task History Logging**: Added a persistent, line-based history log (`history.txt`) for all completed tasks to track progress without storage limits.
- **View History on Telegram**: New `VIEW_HISTORY` intent and Telegram action to retrieve and display recently completed tasks.
- Dedicated background maintenance task in `reminder_scheduler` running daily at 3:00 AM.

## [v4.5.0] - 2026-04-01

### Changed
- Refined boot progress percentage sequence for smoother initialization feedback.
- Adjusted SPI hardware pins in `config.h` for improved display stability.
- Improved **Daily Briefing** logic to automatically include overdue tasks in the 8:00 AM notification.
- Cleaned up codebase by removing legacy `config.h.example`.
- Updated `README.md` with current project status and documentation.

## [v4.4.1] - 2026-03-23

### Changed
- Refactored API key management by moving sensitive credentials to a dedicated `api.h` file.
- Updated `.gitignore` to protect `api.h` from version control.

### Added
- New `api.h.sample` file to serve as a secure configuration template for developers.

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
