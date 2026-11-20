/*
 * @file telemetry_config.hpp
 * @brief Pins the MCU qualification fixture to descriptor v3.0 (no type names).
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * The H7S fixture image and its host/ARM qualification builds publish the
 * 1775-byte v3.0 descriptor that verify.py and the historical receipts pin.
 * This project file keeps that image v3.0 while the SDK default emits names;
 * mcu/run.py and mcu/h7s/run.py put this directory on the include path.
 */
#ifndef TELEMETRY_TESTS_MCU_CONFIG_HPP
#define TELEMETRY_TESTS_MCU_CONFIG_HPP
#pragma once

#define TELEMETRY_TYPE_NAMES 0

#endif
