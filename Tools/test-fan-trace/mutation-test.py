#!/usr/bin/env python3
"""Mutation test for the fan trace harness: inject known bugs into a copy of FanDxe.c and check that
compare.py notices each one.  HEKATE_DIR=/path/to/hekate Tools/test-fan-trace/mutation-test.py

Expected result: every mutant is CAUGHT except the control (unchanged file) and "no final PWM1 pinmux
write", an equivalent mutant: on the first call that second write stores the value the pad already has.
"""
import os, pathlib, subprocess, sys, tempfile

HERE = pathlib.Path(__file__).resolve().parent
PKG = HERE.parent.parent
HEKATE = pathlib.Path(os.environ["HEKATE_DIR"])
CXX = ["g++", "-std=gnu++17", "-x", "c++", "-fpermissive", "-w", "-O1"]
src = (PKG / "Drivers/FanDxe/FanDxe.c").read_text()

M = [
 ("CONTROL (no change)", "FAN_PWM_PARK                   0x100", "FAN_PWM_PARK                   0x100"),
 ("pulse shift 16->15", "#define PWM_CSR_PULSE_SHIFT            16", "#define PWM_CSR_PULSE_SHIFT            15"),
 ("fan pad port V->U", "gpio_config (GPIO_PORT_V, GPIO_PIN_4, GPIO_MODE_SPIO)", "gpio_config (GPIO_PORT_U, GPIO_PIN_4, GPIO_MODE_SPIO)"),
 ("fan pad pin 4->3", "gpio_config (GPIO_PORT_V, GPIO_PIN_4, GPIO_MODE_SPIO)", "gpio_config (GPIO_PORT_V, GPIO_PIN_3, GPIO_MODE_SPIO)"),
 ("LCD_GPIO2 offset 0x20C->0x208", "#define PINMUX_AUX_LCD_GPIO2           0x20C", "#define PINMUX_AUX_LCD_GPIO2           0x208"),
 ("SATA_LED offset 0x4C->0x48", "#define PINMUX_AUX_SATA_LED_ACTIVE     0x4C ", "#define PINMUX_AUX_SATA_LED_ACTIVE     0x48 "),
 ("drop PWM park write", "  PWM (PWM_CONTROLLER_PWM_CSR_1) = PWM_CSR_EN | (FAN_PWM_PARK << PWM_CSR_PULSE_SHIFT);\n", ""),
 ("gpio order: oe before out", "  gpio_write         (GPIO_PORT_A, GPIO_PIN_5, GPIO_HIGH);\n  gpio_output_enable (GPIO_PORT_A, GPIO_PIN_5, GPIO_OUTPUT_ENABLE);", "  gpio_output_enable (GPIO_PORT_A, GPIO_PIN_5, GPIO_OUTPUT_ENABLE);\n  gpio_write         (GPIO_PORT_A, GPIO_PIN_5, GPIO_HIGH);"),
 ("A5 driven low", "gpio_write         (GPIO_PORT_A, GPIO_PIN_5, GPIO_HIGH)", "gpio_write         (GPIO_PORT_A, GPIO_PIN_5, GPIO_LOW)"),
 ("PMC clobbers other bits", "PMC (APBDEV_PMC_NO_IOPOWER) = Value & ~PMC_GPIO_BIT;", "PMC (APBDEV_PMC_NO_IOPOWER) = 0;"),
 ("PMC bit 21->20", "#define PMC_GPIO_BIT                   (1U << 21)", "#define PMC_GPIO_BIT                   (1U << 20)"),
 ("duty scale 236->235", "#define FAN_DUTY_SCALE                 236", "#define FAN_DUTY_SCALE                 235"),
 ("non-inverted duty", "InvDuty = FAN_DUTY_SCALE - Duty;", "InvDuty = Duty;"),
 ("touch CC4 (USB VBUS pad)", "  PINMUX_AUX (PINMUX_AUX_SATA_LED_ACTIVE) = 1;\n", "  PINMUX_AUX (PINMUX_AUX_SATA_LED_ACTIVE) = 1;\n  gpio_write (GPIO_PORT_CC, GPIO_PIN_4, GPIO_LOW);\n"),
 ("touch backlight CSR_0", "  PWM (PWM_CONTROLLER_PWM_CSR_1) = PWM_CSR_EN | (InvDuty << PWM_CSR_PULSE_SHIFT);", "  PWM (0x00) = PWM_CSR_EN;\n  PWM (PWM_CONTROLLER_PWM_CSR_1) = PWM_CSR_EN | (InvDuty << PWM_CSR_PULSE_SHIFT);"),
 ("no final PWM1 pinmux write", "  PINMUX_AUX (PINMUX_AUX_LCD_GPIO2) = 1;                // enable fan\n", ""),
 ("clock source wrong", "((6U << 29) | 4U)", "((6U << 29) | 2U)"),
 ("chip check inverted", "!= GP_HIDREV_MAJOR_T210)", "== GP_HIDREV_MAJOR_T210)"),
 ("no clamp", "  if (Duty > FAN_DUTY_MAX) {\n    Duty = FAN_DUTY_MAX;\n  }\n", ""),
 ("PWM clock check ignores enable bit", "return ((CLOCK (CLK_RST_CLK_OUT_ENB_L) & CLK_L_PWM_BIT) != 0) &&", "return TRUE &&"),
 ("PWM clock check ignores reset", "((CLOCK (CLK_RST_RST_DEVICES_L) & CLK_L_PWM_BIT) == 0)", "TRUE"),
]

tmp = pathlib.Path(tempfile.mkdtemp())
subprocess.run(CXX + ["-I", str(HERE / "shim/hekate"), "-I", str(HERE / "shim/common"), "-I", str(HEKATE / "bdk"),
                      str(HERE / "main_hekate.cpp"), str(HERE / "regmodel.cpp"),
                      str(HEKATE / "bdk/thermal/fan.c"), str(HEKATE / "bdk/power/regulator_5v.c"), str(HEKATE / "bdk/soc/gpio.c"),
                      "-o", str(tmp / "trace_hekate")], check=True)
unexpected = 0
for name, old, new in M:
    assert src.count(old) == 1, (name, src.count(old))
    (tmp / "FanDxe.c").write_text(src.replace(old, new))
    r = subprocess.run(CXX + ["-I", str(HERE / "shim/edk2"), "-I", str(HERE / "shim/common"), "-I", str(PKG / "Include"),
                              str(HERE / "main_fan.cpp"), str(HERE / "regmodel.cpp"), str(tmp / "FanDxe.c"),
                              str(PKG / "Library/GpioLib/GpioLib.c"), "-o", str(tmp / "trace_mut")],
                       capture_output=True, text=True)
    if r.returncode:
        print("COMPILE-ERR", name, r.stderr[:200]); unexpected += 1; continue
    c = subprocess.run([sys.executable, str(HERE / "compare.py"), str(tmp / "trace_hekate"), str(tmp / "trace_mut")],
                       capture_output=True, text=True)
    caught = c.returncode != 0
    summary = [l for l in c.stdout.splitlines() if "scenarios passed" in l]
    expected_pass = name.startswith("CONTROL") or name == "no final PWM1 pinmux write"
    flag = "ok " if caught != expected_pass else "BAD"
    unexpected += (caught == expected_pass)
    print(f"{flag} {'CAUGHT' if caught else 'passed'}  {name:34s} {summary[0] if summary else ''}")
print("unexpected results:", unexpected)
sys.exit(1 if unexpected else 0)
