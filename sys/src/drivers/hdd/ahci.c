#include <drivers/hdd/ahci.h>
#include <drivers/timer/pit.h>
#include <arch/bus/pci.h>
#include <lib/module.h>
#include <lib/log.h>
#include <lib/string.h>
#include <lib/math.h>
#include <lib/asm.h>
#include <lib/assert.h>
#include <mm/heap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>

#define AHCI_DEBUG 1

MODULE("ahci");

#define SATA_CONTROLLER_PCI_CLASS 0x01
#define SATA_CONTROLLER_PCI_SUBCLSS 0x06

#define HBA_PORT_IPM_ACTIVE 1
#define HBA_PORT_DET_PRESENT 3

#define GHC_AHCI_ENABLE (1 << 31)
#define GHC_HBA_RESET   (1 << 0)

#define HBA_PxCMD_ST    (1 << 0)
#define HBA_PxCMD_FRE   (1 << 4)
#define HBA_PxCMD_FR    (1 << 14)
#define HBA_PxCMD_CR    (1 << 15)

#define HBA_PxIS_TFES      (1 << 30)
#define HBA_PxIS_HBFS      (1 << 29)
#define HBA_PxIS_IFS       (1 << 27)
#define HBA_PxIS_HBDS      (1 << 28)
#define HBA_PxIS_INFS      (1 << 26)
#define HBA_ALL_ERRORS (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | HBA_PxIS_IFS | HBA_PxIS_INFS)

#define SATA_SIG_ATA 0x00000101     // SATA drive
#define SATA_SIG_ATAPI 0xEB140101   // SATAPI drive
#define SATA_SIG_SEMB 0xC33C0101    // Enclosure management bridge
#define SATA_SIG_PM 0x96690101      // Port multiplier.

#define AHCI_DEV_NULL 0
#define AHCI_DEV_SATA 1
#define AHCI_DEV_SEMB 2
#define AHCI_DEV_PM 3
#define AHCI_DEV_SATAPI 4

static pci_device_t dev;
static HBA_MEM* abar = NULL;
static uint32_t n_slots = 0;
static sata_dev_t* devices = NULL;


static int find_cmdslot(sata_dev_t* device) {
  uint32_t slots = (device->port->sact | device->port->ci);
  for (uint32_t i = 0; i < n_slots; ++i, slots >>= 1) {
    if (!(slots & 1)) {
      return i;
    }
  }

  PRINTK_SERIAL("[%s]: Failed to find free command list entry.\n", MODULE_NAME);
  return -1;
}


static uint32_t check_type(HBA_PORT* p) {
  uint32_t ssts = p->ssts;
  uint8_t ipm = (ssts >> 8) & 0x0F;
  uint8_t det = ssts & 0x0F;

  if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) {
    return AHCI_DEV_NULL;
  }

  switch (p->sig) {
    case SATA_SIG_ATAPI:
      return AHCI_DEV_SATAPI;
    case SATA_SIG_SEMB:
      return AHCI_DEV_SEMB;
    case SATA_SIG_PM:
      return AHCI_DEV_PM;
    default:
      return AHCI_DEV_SATA;
  }
}


static void send_cmd(sata_dev_t* device, uint32_t slot) {
  while ((device->port->tfd & 0x88) != 0);
  device->port->cmd &= ~(1 << 0);
  while ((device->port->cmd & HBA_PxCMD_CR) != 0);

  device->port->cmd |= HBA_PxCMD_FR | HBA_PxCMD_ST;
  device->port->ci = 1 << slot;

  while (device->port->ci & (1 << slot) != 0);

  device->port->cmd &= ~(HBA_PxCMD_ST);
  while ((device->port->cmd & HBA_PxCMD_ST) != 0);
  device->port->cmd &= ~(HBA_PxCMD_FRE);
}

static inline volatile HBA_CMD_TBL* set_prdt(sata_dev_t* device, uint64_t buf_phys, uint8_t interrupt, uint32_t byte_count) {
  volatile HBA_CMD_TBL* cmdtable = (volatile HBA_CMD_TBL*)(device->ctba_phys + VMM_HIGHER_HALF);
  cmdtable->prdt_entry[0].dba = (uint32_t)buf_phys;
  cmdtable->prdt_entry[0].dbau = buf_phys >> 32;
  cmdtable->prdt_entry[0].dbc = byte_count;
  cmdtable->prdt_entry[0].i = interrupt;
  return cmdtable;
}

static void sata_read_at(sata_dev_t* device, uint64_t lba, uint32_t sector_count, uint16_t* buf) {
  int cmdslot = find_cmdslot(device);
  
  if (cmdslot == -1) {
    PRINTK_SERIAL(PRINTK_RED "[%s]: Could not find a command slot!\n", MODULE_NAME);
    return;
  }

  volatile HBA_CMD_HEADER* cmdheader = (volatile HBA_CMD_HEADER*)(device->cmdlist_phys + VMM_HIGHER_HALF);

  cmdheader[cmdslot].cfl = sizeof(FIS_REG_H2D)/sizeof(uint32_t);
  cmdheader[cmdslot].w = 0;
  cmdheader[cmdslot].prdtl = 1;

  volatile HBA_CMD_TBL* cmd_table = set_prdt(device, VMM_PHYS((uintptr_t)PAGE_ALIGN_UP(buf)), 0, sector_count*512-1);
  volatile FIS_REG_H2D* fis = (FIS_REG_H2D*)cmd_table->cfis;
  
  fis->fis_type = 0x27;        // Host to device.
  fis->c = 1;
  fis->command = 0x25;         // Read DMA extended.
  fis->device = 1 << 6;        // LBA mode.
  fis->lba0 = (uint8_t)(lba & 0xFF);
  fis->lba0 = (uint8_t)((lba >> 8) & 0xFF);
  fis->lba0 = (uint8_t)((lba >> 16) & 0xFF);
  fis->lba0 = (uint8_t)((lba >> 24) & 0xFF);
  fis->lba0 = (uint8_t)((lba >> 32) & 0xFF);
  fis->lba0 = (uint8_t)((lba >> 40) & 0xFF);
  fis->countl = sector_count & 0xFF;
  fis->counth = (sector_count >> 8) & 0xFF;

  send_cmd(device, cmdslot);
  
}


static void device_init(sata_dev_t* device) {
  ASSERT(device->magic == SATA_DEV_MAGIC, "Device magic invalid!\n");
  device->cmdlist_phys = pmm_alloc();

  HBA_CMD_HEADER* cmdlist_header = (HBA_CMD_HEADER*)(device->cmdlist_phys + VMM_HIGHER_HALF);

  for (uint8_t i = 0; i < 32; ++i) {
    uint64_t desc_base = pmm_alloc();
    device->ctba_phys = desc_base;
    cmdlist_header[i].ctba = (uint32_t)desc_base;
    cmdlist_header[i].ctbau = desc_base >> 32;
    cmdlist_header[i].prdtl = 1;
  }

  device->fis_base = pmm_alloc();

  // Set ST and FRE bits.
  device->port->cmd |= (1 << 0) | (1 << 4);
}


static void find_ports(void) {
  if (devices != NULL) {
    return;
  }

  size_t dev_idx = 0;

  uint32_t port_count = abar->cap & 0x1F;
  devices = kmalloc(sizeof(sata_dev_t) * port_count);
  n_slots = (abar->cap >> 8) & 0x1F;
  PRINTK_SERIAL("[%s]: HBA has %d ports and %d command slots.\n", MODULE_NAME, port_count, n_slots);

  for (uint32_t i = 0; i < port_count; ++i) {
    if (abar->pi & (1 << i) != 0) {
      HBA_PORT* port = (HBA_PORT*)&abar->ports[i];
      
      switch (check_type(port)) {
        case AHCI_DEV_SATA:
          devices[dev_idx].port = port;
          devices[dev_idx].magic = SATA_DEV_MAGIC;
          PRINTK_SERIAL("[%s]: SATA drive found @HBA_PORT_%d\n", MODULE_NAME, i);
          device_init(&devices[dev_idx]);
          ++dev_idx;
          break;
      }
    }
  }
}


static uint8_t take_ownership(void) {
  if (!(abar->cap & (1 << 0))) {
    PRINTK_SERIAL(PRINTK_RED "[%s] BIOS handoff not supported!\n", MODULE_NAME);
    return 1;
  }

  abar->bohc |= (1 << 1);
  
  while (abar->bohc & (1 << 0) == 0);

  for (uint32_t i = 0; i < 5; ++i) CLI_SLEEP;

  if (abar->bohc & (1 << 4)) {
    for (uint32_t i = 0; i < 10; ++i) CLI_SLEEP;
  }

  if (abar->bohc & (1 << 4) != 0 || abar->bohc & (1 << 0) != 0 || abar->bohc & (1 << 1) == 0) {
    PRINTK_SERIAL(PRINTK_RED "[%s]: BIOS handoff failure!\n", MODULE_NAME);
    return 1;
  }


  PRINTK_SERIAL("[%s]: BIOS handoff success!\n", MODULE_NAME);
  return 0;
}


void ahci_init(void)  {
  dev = pci_find_any(SATA_CONTROLLER_PCI_CLASS, SATA_CONTROLLER_PCI_SUBCLSS, -1);

  if (!(dev.valid)) {
    PRINTK_SERIAL("[%s]: No SATA controller attached!\n", MODULE_NAME);
    return;
  }

  printk("[%s]: SATA controller found on PCI bus %d, slot %d\n", MODULE_NAME, dev.bus, dev.slot);
  enable_bus_mastering(dev);
  printk("[%s]: Bus mastering enabled for SATA controller.\n", MODULE_NAME);

  abar = (HBA_MEM*)(uint64_t)dev.bar5;
  
  printk("[%s]: HBA is in %s mode\n", MODULE_NAME, abar->ghc & GHC_AHCI_ENABLE ? "AHCI" : "IDE emulation");
  take_ownership();

  abar->ghc |= (1 << 31);
  abar->ghc &= ~(1 << 1);

  find_ports();
  
  uint16_t* buf = (uint16_t*)(ALIGN_UP((uint64_t)kmalloc(1000), PAGE_SIZE));
  sata_read_at(&devices[0], 0, 1, (void*)buf);

  for (uint32_t i = 0; i < 1000/sizeof(uint32_t); ++i) {
    PRINTK_SERIAL("%x\n", buf[i]);
  }
}
