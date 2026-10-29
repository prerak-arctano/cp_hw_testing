cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(cp_hardware_test)

target_sources(app PRIVATE
	src/main.c
	src/net_enable.c
	src/i2c_scanner.c
	src/qspi.c
	src/rtc.c
	src/uart.c
)
