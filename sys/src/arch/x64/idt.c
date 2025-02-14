#include <arch/x64/idt.h>
#include <arch/x86/apic/ioapic.h>
#include <firmware/acpi/acpi.h>
#include <lib/asm.h>
#include <lib/log.h>
#include <lib/module.h>

#define TRAP_GATE_FLAGS 0x8F
#define INT_GATE_FLAGS 0x8E
#define IDT_INT_GATE_USER 0xEE

MODULE("idt");

struct interrupt_gate_descriptor idt[256];
struct idtr idtr = {
  .limit = sizeof(struct interrupt_gate_descriptor) * 256 - 1,
  .base = (uint64_t)&idt
};

static void set_desc(uint8_t vector, void* isr, uint8_t flags) {
  uint64_t addr = (uint64_t)isr;
  struct interrupt_gate_descriptor* desc = &idt[vector];

  desc->offlow16 = addr & 0xFFFF;
  desc->offmid16 = (addr >> 16) & 0xFFFF;
  desc->offhigh32 = (addr >> 32);
  desc->cs = 0x28;
  desc->ist = 0;
  desc->zero = 0;
  desc->zero1 = 0;
  desc->reserved = 0;
  desc->attr = flags;
  desc->dpl = 3;
  desc->p = 1;
}


void register_int(uint8_t vector, void* isr) {
  set_desc(vector, isr, INT_GATE_FLAGS);
}

void register_exception_handler(uint8_t vector, void* isr) {
  set_desc(vector, isr, TRAP_GATE_FLAGS);
}

void register_irq(uint8_t irq, void* isr, uint64_t extra_redentry_data) {
  register_int(0x20 + irq, isr);
  ioapic_set_entry(acpi_remap_irq(irq), (0x20 + irq) | extra_redentry_data);
}

void load_idt(void) {
  ASMV("lidt %0" :: "m" (idtr));
}
