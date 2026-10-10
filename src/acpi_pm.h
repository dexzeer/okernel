#ifndef ACPI_PM_H
#define ACPI_PM_H

#include <stdint.h>

// ACPI power management: what the firmware says about turning the machine off (S5) and resetting it.
// The parsers are pure (host-tested with crafted tables, tests/test_acpi.c); the two actions write I/O ports and
// are shared by the desktop (acpi.c fills the struct from the tables it already maps) and the text-mode report
// kernel (hwinfo.c walks the tables itself).

struct acpi_pm {
    int      valid;            // a FADT with a usable PM1a control block was parsed
    uint32_t dsdt;             // phys of the DSDT (0 = none / above 4GB)
    uint32_t smi_cmd;          // port that switches the chipset into ACPI mode (0 = already in ACPI mode)
    uint8_t  acpi_enable;      // value written to smi_cmd
    uint32_t pm1a_cnt, pm1b_cnt;   // I/O ports of the PM1 control registers (pm1b may be 0)
    int      hw_reduced;       // FADT flag: no PM1 blocks, a single SLEEP_CONTROL register instead
    uint32_t sleep_ctl_port;   // I/O port of SLEEP_CONTROL_REG (hardware-reduced ACPI), 0 = unusable
    int      reset_ok;         // RESET_REG is an I/O port we can use
    uint32_t reset_port;
    uint8_t  reset_value;
    int      have_s5;          // slp_typa/b came from the DSDT's _S5_ object
    uint8_t  slp_typa, slp_typb;
};

// Fill what the FADT ("FACP") holds. Returns 0 for a table that is too short or has no PM1a block.
int acpi_parse_fadt(const uint8_t* t, uint32_t len, struct acpi_pm* out);

// Find the _S5_ package (SLP_TYPa, SLP_TYPb) in a DSDT/SSDT body. Returns 0 when there is none.
int acpi_find_s5(const uint8_t* aml, uint32_t len, uint8_t* typa, uint8_t* typb);

// Turn the machine off through the ACPI S5 sleep state. Returns only on failure (after trying, for emulators,
// the well-known power-off ports). Interrupts are left disabled.
void acpi_pm_poweroff(const struct acpi_pm* pm);

// Reset the machine: ACPI reset register, keyboard controller, PCI reset port, finally a triple fault.
void acpi_pm_reboot(const struct acpi_pm* pm);

#endif
