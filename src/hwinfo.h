#ifndef HWINFO_H
#define HWINFO_H

#include <stdint.h>

// Hardware report for the text-mode build (see hwinfo.c)
void hwinfo_init(uint32_t mboot_phys);
void hwinfo_cpu(void);
void hwinfo_memmap(void);
void hwinfo_acpi(void);
void hwinfo_pci(int page);
void hwinfo_pcidump(const char* bdf);
void hwinfo_gpu(void);
void hwinfo_input(void);
void hwinfo_poweroff(void);       // ACPI S5; does not return
void hwinfo_reboot(void);
void hwinfo_all(void);

#endif
