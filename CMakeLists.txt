# ===========================================================================
# ESP-Agent: Trợ lý nhắc việc thông minh sử dụng LLM + Telegram Bot
# Project-level CMakeLists.txt cho ESP-IDF
# Target: ESP32-C3 Super Mini
# ===========================================================================

cmake_minimum_required(VERSION 3.16)

# Sử dụng custom partition table
set(EXTRA_COMPONENT_DIRS "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-agent)
