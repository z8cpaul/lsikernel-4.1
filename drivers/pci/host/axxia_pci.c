/**
* AXXIA X7 PCIe Driver
*
* Copyright (c) 2014 Applied Micro Circuits Corporation.
*
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
*/
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/interrupt.h>

#undef PRINT_CONFIG_ACCESSES


#define PCIE_CONFIG              (0x1000)
#define PCIE_STATUS              (0x1004)
#define PCIE_CORE_DEBUG          (0x1008)
#define PCIE_LOOPBACK_FAIL       (0x100C)
#define PCIE_MPAGE_U(n)          (0x1010 + (n * 8)) /* n = 0..7 */
#define PCIE_MPAGE_L(n)          (0x1014 + (n * 8)) /* n = 0..7 */
#define PCIE_TPAGE_BAR0(n)       (0x1050 + (n * 4)) /* n = 0..7 */
#define     PCIE_TPAGE_32        (0<<31) /* AXI 32-bit access */
#define     PCIE_TPAGE_128       (1<<31) /* AXI 128-bit access */
#define PCIE_TPAGE_BAR1(n)       (0x1070 + (n * 4)) /* n = 0..7 */
#define PCIE_TPAGE_BAR2(n)       (0x1090 + (n * 4)) /* n = 0..7 */
#define PCIE_MSG_IN_FIFO         (0x10B0)
#define PCIE_MSG_IN_FIFO_STATUS  (0x10B4)
#define PCIE_MSG_OUT             (0x10B8)
#define PCIE_TRN_ORDER_STATUS    (0x10BC)
#define PCIE_INT0_STATUS         (0x10C0)
#define PCIE_INT0_ENABLE         (0x10C4)
#define PCIE_INT0_FORCE          (0x10C8)
#define    INT0_MSI              0x80000000U
#define    INT0_INT_ASSERTED     0x08000000U
#define    INT0_INT_DEASSERTED   0x04000000U
#define    INT0_ERROR            0x73FFFFABU
#define PCIE_PHY_STATUS0         (0x10CC)
#define PCIE_PHY_STATUS1         (0x10D0)
#define PCIE_PHY_CONTROL0        (0x10D4)
#define PCIE_PHY_CONTROL1        (0x10D8)
#define PCIE_PHY_CONTROL2        (0x10DC)
#define PCIE_RESERVED_E0         (0x10E0)
#define PCIE_RESERVED_E4         (0x10E4)
#define PCIE_RESERVED_E8         (0x10E8)
#define PCIE_AXI_MASTER_WR       (0x10EC)
#define PCIE_LINK_STATUS         (0x117C)
#define PCIE_EP_BAR2_CFG         (0x1184)
#define PCIE_AXI_MSI_ADDR        (0x1190)
#define PCIE_INT1_STATUS         (0x11C4)
#define PCIE_INT1_ENABLE         (0x11C8)
#define PCIE_INT1_FORCE          (0x11CC)
#define INT1_DOORBELL            0x00000001U
#define PCIE_RC_BAR0_SIZE        (0x11F4)
#define PCIE_MSI0_STATUS         (0x1230)
#define PCIE_MSI0_ENABLE         (0x1234)
#define PCIE_MSI0_FORCE          (0x1238)
#define PCIE_MSI1_STATUS(_grp)   (0x123C+(_grp)*12)
#define PCIE_MSI1_ENABLE(_grp)   (0x1240+(_grp)*12)
#define PCIE_MSI1_FORCE(_grp)    (0x1244+(_grp)*12)

/* Every MPAGE register maps 128MB in the AXI memory range */
#define MPAGE_SIZE               (128U<<20)

/* We have 7 MPAGE registers available for outbound window (one reserved for
* mapping PCI configuration space).
*/
#define MAX_OUTBOUND_SIZE        (7 * MPAGE_SIZE)

static const struct resource pcie_outbound_default[] = {
	[0] = {
		.start = 0,
		.end   = 0,
		.flags = IORESOURCE_MEM
	},
	[1] = {
		.start = 0,
		.end   = 0,
		.flags = IORESOURCE_MEM
	},
	[2] = {
		.start = 0,
		.end   = 0,
		.flags = IORESOURCE_MEM
	}
};


struct axxia_pciex_port {
	char                name[16];
	unsigned int        index;
	bool                link_up;
	int                 irq[17]; /* 1 legacy, 16 MSI */
	void __iomem        *regs;
	void __iomem        *cfg_data;
	u32                 last_mpage;
	int                 endpoint;
	struct device       *dev;
	struct device_node  *node;
	struct resource     utl_regs;
	struct resource     cfg_space;
	struct pci_sys_data sysdata;
	/* Outbound PCI base address */
	u64                 pci_addr;
	/* Outbound range in (physical) CPU addresses */
	struct resource     outbound;
	/* Inbound PCI base address */
	u64                 pci_bar;
	/* Inbound range in (physical) CPU addresses */
	struct resource     inbound;
	/* Virtual and physical (CPU space) address for MSI table */
	void                *msi_virt;
	dma_addr_t          msi_phys;
	/* PCI memory space address for MSI table */
	u32                 msi_pci_addr;
};

static int num_pcie_ports;

static void
fixup_axxia_pci_bridge(struct pci_dev *dev)
{
	/* if we aren't a PCIe don't bother */
	if (!pci_find_capability(dev, PCI_CAP_ID_EXP))
		return;

	/* Set the class appropriately for a bridge device */
	dev_info(&dev->dev,
		"Fixup PCI Class to PCI_CLASS_BRIDGE_HOST for %04x:%04x\n",
		dev->vendor, dev->device);
		dev->class = PCI_CLASS_BRIDGE_HOST << 8;
	/* Make the bridge transparent */
	dev->transparent = 1;
}

DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5101, fixup_axxia_pci_bridge);
DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5108, fixup_axxia_pci_bridge);
DECLARE_PCI_FIXUP_HEADER(0x1000, 0x5120, fixup_axxia_pci_bridge);

/*
* Return the configuration access base address
*/
static void __iomem *
axxia_pciex_get_config_base(struct axxia_pciex_port *port,
	struct pci_bus *bus,
	unsigned int devfn)
{
	int relbus, dev, fn;
	unsigned mpage;

	if (bus->number == 0)
		return port->regs;

	relbus = bus->number - 1;
	dev    = PCI_SLOT(devfn);
	fn     = PCI_FUNC(devfn);

	if (dev > 31)
		return NULL;

	/* Build the mpage register (MPAGE[4]=1 for cfg access) */
	mpage = (fn << 19) | (bus->number << 11) | (dev << 6) | (1<<4);

	/* Primary bus */
	if (relbus && bus->number != 0)
		mpage |= 1<<5;

	if (mpage != port->last_mpage) {
		writel(0,     port->regs + PCIE_MPAGE_U(7));
		writel(mpage, port->regs + PCIE_MPAGE_L(7));
		port->last_mpage = mpage;
	}

	return port->cfg_data;
}

static inline struct axxia_pciex_port *sys_to_pcie(struct pci_sys_data *sys)
{
	return sys->private_data;
}

/*
* Validate the Bus#/Device#/Function#
*/
static int
axxia_pciex_validate_bdf(struct pci_bus *bus, unsigned int devfn)
{
	struct pci_sys_data *sys = bus->sysdata;
	struct axxia_pciex_port *port = sys_to_pcie(sys);

	/* Endpoint can not generate upstream(remote) config cycles */
	if (port->endpoint)
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (((!((PCI_FUNC(devfn) == 0) && (PCI_SLOT(devfn) == 0)))
		&& (bus->number == 0))
		|| (!(PCI_SLOT(devfn) == 0)
		&& (bus->number == 1))) {
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return 0;
}

/*
* Read PCI config space
*/
static int
axxia_pciex_read_config(struct pci_bus *bus, unsigned int devfn,
			int offset, int len, u32 *val)
{
	struct pci_sys_data *sys = bus->sysdata;
	struct axxia_pciex_port *port = sys_to_pcie(sys);
	void __iomem *addr;
	u32 val32;
	int bo = offset & 0x3;
	int rc = PCIBIOS_SUCCESSFUL;

	if (axxia_pciex_validate_bdf(bus, devfn) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = axxia_pciex_get_config_base(port, bus, devfn);

	if (!addr) {
		*val = 0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	/*
	 * addressing is different for local config access vs.
	 * access through the mapped config space.
	 */
	if (bus->number == 0) {
		int wo = offset & 0xfffffffc;

		addr = addr + wo;
	} else {
		/*
		 * mapped config space only supports 32-bit access
		 * AXI address [3:0] is not used at all.
		 *  AXI address[9:4] becomes register number.
		 *  AXI address[13:10] becomes Ext. register number
		 *  AXI address[17:14] becomes 1st DWBE for configuration
		 *  read only.
		 *  AXI address[29:27] is used to select one of 8 Mpage
		 *  registers.
		 */
		offset = (offset << 2);

		switch (len) {
		case 1:
			offset |= ((1 << bo)) << 14;
			addr = addr + offset;
			break;
		case 2:
			offset |=  ((3 << bo)) << 14;
			addr = addr + offset;
			break;
		default:
			offset |=  (0xf) << 14;
			addr = addr + offset;
			break;
		}
	}
	/*
	 * do the read
	 */
	val32 = readl(addr);

	switch (len) {
	case 1:
		*val = (val32 >> (bo * 8)) & 0xff;
		break;
	case 2:
		*val = (val32 >> (bo * 8)) & 0xffff;
		break;
	default:
		*val = val32;
		break;
	}
#ifdef PRINT_CONFIG_ACCESSES
	pr_info("acp_read_config for PCIE%d: %3d  fn=0x%04x o=0x%04x l=%d a=0x%p v=0x%08x, dev=%d\n",
		port->index, bus->number, devfn, offset, len,
		addr, *val, PCI_SLOT(devfn));
#endif
	return rc;
}

/*
* Write PCI config space.
*/
static int
axxia_pciex_write_config(struct pci_bus *bus, unsigned int devfn,
			 int offset, int len, u32 val)
{
	struct pci_sys_data *sys = bus->sysdata;
	struct axxia_pciex_port *port = sys_to_pcie(sys);
	void __iomem *addr;
	u32 val32;

	if (axxia_pciex_validate_bdf(bus, devfn) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = axxia_pciex_get_config_base(port, bus, devfn);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/*
	 * addressing is different for local config access vs.
	 * access through the mapped config space. We need to
	 * translate the offset for mapped config access
	 */
	if (bus->number == 0) {
		/* the local ACP RC only supports 32-bit dword config access,
		 * so if this is a byte or 16-bit word access we need to
		 * perform a read-modify write
		 */
		if (len == 4) {
			addr =  addr + offset;
		} else {
			int bs = ((offset & 0x3) * 8);

			addr = addr + (offset & 0xfffffffc);
			val32 = readl(addr);

			if (len == 2)
				val32 = (val32 & ~(0xffff << bs))
					| ((val & 0xffff) << bs);
			else
				val32 = (val32 & ~(0xff << bs))
					| ((val & 0xff) << bs);

			val = val32;
		}

		len = 4;
	} else {
		addr = addr + (offset << 2) + (offset & 0x3);
	}

#ifdef PRINT_CONFIG_ACCESSES
	pr_info("acp_write_config: bus=%3d devfn=0x%04x offset=0x%04x len=%d addr=0x%p val=0x%08x\n",
		bus->number, devfn, offset, len, addr, val);
#endif

	switch (len) {
	case 1:
		writeb(val, (u8 __iomem *)(addr));
		break;
	case 2:
		writew(val, (u16 __iomem *)(addr));
		break;
	default:
		writel(val, (u32 __iomem *)(addr));
		break;
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops axxia_pciex_pci_ops = {
	.read  = axxia_pciex_read_config,
	.write = axxia_pciex_write_config,
};


/*
* pcie_legacy_isr
*
* The interrupt line for this handler is shared between the PCIE controller
* itself (for status and error interrupts) and devices using legacy PCI
* interrupt signalling. Statis and error interrupts are serviced here and this
* handler will return IRQ_HANDLED. If the reasont is the assertion of a device
* legacy interrupt, this handler returns IRQ_NONE the next action on this line
* will be called (the PCI EP interrupt service routine).
*/
static irqreturn_t
pcie_legacy_isr(int irq, void *arg)
{
	struct axxia_pciex_port *port  = arg;
	void __iomem            *mbase = port->regs;
	u32                      intr_status, intr1_status;
	irqreturn_t              retval = IRQ_HANDLED;

	/* read the PEI interrupt status register */
	intr_status = readl(mbase + PCIE_INT0_STATUS);
	intr1_status = readl(mbase + PCIE_INT1_STATUS);

	/* check if this is a PCIe message not from an external device */
	if (intr_status & INT0_ERROR) {
		u32 int_enb;
		u32 offset;

		pr_info("PCIE%d: Error interrupt %#x\n",
			port->index, intr_status);

		pr_info("PCIE%d: link status = %#x\n",
			port->index, readl(mbase + PCIE_LINK_STATUS));

		if (intr_status & 0x00020000) {
			pr_info("PCIE%d: t2a_fn_indp_err_stat = %#x\n",
				port->index, readl(mbase+0x1170));
			int_enb = readl(mbase + PCIE_INT0_ENABLE);
			int_enb &= 0xfffdffff;
			writel(int_enb, mbase + PCIE_INT0_ENABLE);
		}

		if (intr_status & 0x00040000) {
			pr_info("PCIE%d: t2a_fn_indp_other_err_stat = %#x\n",
				port->index, readl(mbase+0x1174));
			int_enb = readl(mbase + PCIE_INT0_ENABLE);
			int_enb &= 0xfffbffff;
			writel(int_enb, mbase + PCIE_INT0_ENABLE);
		}

		if (intr_status & 0x00000800) {
			pr_info("PCIE%d: config=%#x status=%#x\n",
				port->index,
				readl(mbase + PCIE_CONFIG),
				readl(mbase + PCIE_STATUS));
			int_enb = readl(mbase + PCIE_INT0_ENABLE);
			int_enb &= 0xfffff7ff;
			writel(int_enb, mbase + PCIE_INT0_ENABLE);
		}

		/*
		 * Dump all the potentially interesting PEI registers
		 */
		for (offset = 0x114c; offset <= 0x1180; offset += 4) {
			pr_err("  [0x%04x] = 0x%08x\n",
				offset, readl(mbase + offset));
		}
	} else if (intr_status & INT0_INT_ASSERTED) {
		/* Empty the message FIFO */
		while ((readl(port->regs + PCIE_MSG_IN_FIFO_STATUS) & 1) == 0)
			(void) readl(port->regs + PCIE_MSG_IN_FIFO);
		/* Next handler in chain will service this interrupt */
		retval = IRQ_NONE;
	}

	if (intr1_status & INT1_DOORBELL) {
		/* RC doorbell interrupt. This support expects kernel module
		 * to handle this doorbell interrupt
		 */
		/* Clear it */
		writel(INT1_DOORBELL, mbase + PCIE_INT1_STATUS);
		return IRQ_NONE;
	}

	/*
	 *  We clear all the interrupts in the PEI status, even though
	 *  interrupts from external devices have not yet been handled.
	 *  That should be okay, since the PCI IRQ in the GIC won't be
	 *  re-enabled until all external handlers have been called.
	 */
	writel(intr_status, mbase + PCIE_INT0_STATUS);
	return retval;
}

/*
* pcie_doorbell_isr
*
* This ISR is for doorbell interrupts for
* Endpoint mode which has a dedicated IRQ line
* This support expects kernel module to handle the doorbell
* interrupt
*/
static irqreturn_t
pcie_doorbell_isr(int irq, void *arg)
{
	struct axxia_pciex_port *port  = arg;
	void __iomem            *mbase = port->regs;
	u32                     intr1_status;

	intr1_status = readl(mbase + PCIE_INT1_STATUS);

	if (intr1_status & INT1_DOORBELL) {
		/* EP doorbell interrupt. This support expects kernel module
		 * to handle this doorbell interrupt
		 */
		/* Clear it */
		writel(INT1_DOORBELL, mbase + PCIE_INT1_STATUS);
	}
	return IRQ_NONE;
}

static int axxia_pcie_setup(struct axxia_pciex_port *port,
			    struct list_head *res)
{
	u32 pci_config, pci_status, link_state;
	int i, num_pages, err;
	u32 outbound_size;
	u32 inbound_size;
	u64 dest;

	/* Map PCIe bridge control registers */
	port->regs = ioremap(port->utl_regs.start,
			     resource_size(&port->utl_regs));
	if (!port->regs) {
		pr_err("PCIE%d: Failed to map registers\n", port->index);
		goto fail;
	}

	/* Map range for access to PCI configuration space */
	port->cfg_data = ioremap(port->cfg_space.start,
		resource_size(&port->cfg_space));
	if (!port->cfg_data) {
		pr_err("PCIE%d: Failed to map config space\n", port->index);
		goto fail;
	}
	pci_add_resource_offset(res, &port->outbound,
		port->outbound.start - port->pci_addr);

	/* Status/error interrupt */
	port->irq[0] = irq_of_parse_and_map(port->node, 0);
	err = request_irq(port->irq[0], pcie_legacy_isr, IRQF_SHARED,
		"pcie", port);
	if (err) {
		pr_err("PCIE%d: Failed to request IRQ#%d (%d)\n",
			port->index, port->irq[0], err);
		goto fail;
	}
	/* Setup as root complex */
	pci_config = readl(port->regs + PCIE_CONFIG);
	pci_status = readl(port->regs + PCIE_STATUS);
	link_state = (pci_status >> 8) & 0x3f;
	pr_info("PCIE%d: status=0x%08x, link state=%#x\n",
		port->index, pci_status, link_state);

	/* make sure the ACP device is configured as PCI Root Complex */
	if ((pci_status & 0x18) != 0x18) {
		/* Endpoint */
		pr_err("PCIE%d: Device is not Root Complex\n", port->index);
		if (port->index == 0) {
			/* PEI0 */
			err = request_irq(port->irq[0]+3, pcie_doorbell_isr,
				IRQF_SHARED, "pcie_db", port);
			if (err) {
				pr_err("PCIE%d: Failed to request IRQ#%d (%d)\n",
					port->index, port->irq[0], err);
				goto fail;
			}
		} else if (port->index == 1) {
			/* PEI1 */
			err = request_irq(port->irq[0]+2, pcie_doorbell_isr,
				IRQF_SHARED, "pcie_db", port);
			if (err) {
				pr_err("PCIE%d: Failed to request IRQ#%d (%d)\n",
					port->index, port->irq[0], err);
				goto fail;
			}
		}
		/* Enable doorbell interrupts */
		writel(INT1_DOORBELL, port->regs + PCIE_INT1_ENABLE);
		return 1;
	}

	/* Make sure the link is up */
	if (link_state != 0xb) {
		/* Reset */
		pr_warn("PCIE%d: Link in bad state - resetting\n", port->index);
		pci_config |= 1;
		writel(pci_config, port->regs + PCIE_CONFIG);
		msleep(1000);
		pci_status = readl(port->regs + PCIE_STATUS);
		link_state = (pci_status & 0x3f00) >> 8;
		pr_warn("PCIE%d: (after reset) link state=%#x\n",
			port->index, link_state);
		if (link_state != 0xb) {
			pr_warn("PCIE%d: Link in bad state - giving up!\n",
			port->index);
			pr_warn("PCIE%d: Link in bad state - giving up!\n",
			port->index);
			goto fail;
		}
	}

	/*
	 * Setup outbound PCI Memory Window
	 */
	outbound_size = resource_size(&port->outbound);
	num_pages = (outbound_size + MPAGE_SIZE - 1) / MPAGE_SIZE;
	dest = port->pci_addr;
	for (i = 0; i < num_pages; i++) {
		u32 mpage_u = dest >> 32;
		u32 mpage_l = (u32)dest & ~(MPAGE_SIZE-1);

		writel(mpage_u, port->regs + PCIE_MPAGE_U(i));
		writel(mpage_l, port->regs + PCIE_MPAGE_L(i));
		pr_debug("PCIE%d: MPAGE(%d) = %08x %08x\n",
			port->index, i, mpage_u, mpage_l);
		dest += MPAGE_SIZE;
	}

	/*
	 * Setup inbound PCI window
	 */

	/* Configure the inbound window size */
	inbound_size = (u32) resource_size(&port->inbound);
	writel(~(inbound_size-1), port->regs + PCIE_RC_BAR0_SIZE);

	/* Verify BAR0 size */
	{
		u32 bar0_size;

		writel(~0, port->regs + PCI_BASE_ADDRESS_0);
		bar0_size = readl(port->regs + PCI_BASE_ADDRESS_0);
		if ((bar0_size & ~0xf) != ~(inbound_size-1))
			pr_err("PCIE%d: Config BAR0 failed\n", port->index);
	}

	/* Set the BASE0 address to start of PCIe base */
	writel(port->pci_bar, port->regs + PCI_BASE_ADDRESS_0);

	/* Set the BASE1 address to 0x0 */
	writel(0x0, port->regs + PCI_BASE_ADDRESS_1);


	/* Setup TPAGE registers for inbound mapping
	 * We set the MSB of each TPAGE to select 128-bit AXI access. For the
	 * address field we simply program an incrementing value to map
	 * consecutive pages
	 */
	for (i = 0; i < 8; i++)
		writel(PCIE_TPAGE_128 | i, port->regs + PCIE_TPAGE_BAR0(i));

	/* Enable all legacy/status/error interrupts */
	writel(INT0_MSI | INT0_INT_ASSERTED | INT0_ERROR,
		port->regs + PCIE_INT0_ENABLE);

	/* Enable doorbell interrupts */
	writel(INT1_DOORBELL, port->regs + PCIE_INT1_ENABLE);

	return 1;
fail:
	if (port->cfg_data)
		iounmap(port->cfg_data);
	if (port->regs)
		iounmap(port->regs);
	return 0;
}

static int
axxia_probe_pciex_bridge(struct platform_device *pdev)
{
	struct axxia_pciex_port *port;
	struct device_node *np = pdev->dev.of_node;
	const char *val;
	const u32 *field;
	int rlen;
	int pna = of_n_addr_cells(np); /* address-size of parent */
	u32 pci_space;
	u64 pci_addr;
	u64 cpu_addr;
	u64 size;
	LIST_HEAD(res);
	struct pci_bus *bus;
	int lastbus;

	port = devm_kzalloc(&pdev->dev, sizeof *port, GFP_KERNEL);
	if (port == NULL)
		return -ENOMEM;

	port->index = num_pcie_ports++;
	snprintf(port->name, sizeof(port->name) - 1, "PCIE%d", port->index);
	port->dev = &pdev->dev;
	port->node = of_node_get(np);

	/* Check if device_type property is set to "pci" or "pci-endpoint".
	 * Resulting from this setup this PCIe port will be configured
	 * as root-complex or as endpoint.
	 */
	val = of_get_property(port->node, "device_type", NULL);
	port->endpoint = val && strcmp(val, "pci-endpoint") == 0;

	/* Fetch address range for PCIE config space */
	if (of_address_to_resource(np, 0, &port->cfg_space)) {
		pr_err("PCIE%d: No resource for PCIe config space\n",
		       port->index);
		return 1;
	}

	/* Fetch address range for host bridge internal registers */
	if (of_address_to_resource(np, 1, &port->utl_regs)) {
		pr_err("PCIE%d: No resource for PCIe registers\n", port->index);
		return 1;
	}

	if (request_resource(&iomem_resource, &port->utl_regs))
		return 1;

	/*
	 * Outbound PCI memory window
	 */

	/* Defaults */
	port->outbound = pcie_outbound_default[port->index];
	port->outbound.name = port->name;

	field = of_get_property(np, "ranges", &rlen);
	if (field) {
		pci_space = of_read_number(field + 0, 1);
		switch ((pci_space >> 24) & 3) {
		case 0:
			pr_err("PCIE%d: Invalid 'ranges'\n", port->index);
			break;
		case 1: /* PCI IO Space */
			pr_err("PCIE%d: PCI IO not supported\n", port->index);
			break;
		case 2: /* PCI MEM 32-bit */
		case 3: /* PCI MEM 64-bit */
			cpu_addr  = of_read_number(field + 3, 2);
			size      = of_read_number(field + 5, 2);
			port->outbound.start = cpu_addr;
			port->outbound.end   = cpu_addr + size - 1;
			port->pci_addr = of_read_number(field + 1, 2);
			break;
		}
	}
	if (resource_size(&port->outbound) > MAX_OUTBOUND_SIZE) {
		pr_err("PCIE%d: Outbound window too large (using max %#x)\n",
		       port->index, MAX_OUTBOUND_SIZE);
		port->outbound.end = (port->outbound.start +
				      MAX_OUTBOUND_SIZE - 1);
	}

	if (request_resource(&iomem_resource, &port->outbound)) {
		pr_err("PCIE%d: Memory resource request failed\n", port->index);
		return 1;
	}

	if (request_resource(&iomem_resource, &port->cfg_space)) {
		pr_err("PCIE%d: Config space request failed\n", port->index);
		return 1;
	}

	pr_info("PCIE%d: Outbound %#llx..%#llx (CPU) -> %#llx (PCI)\n",
		port->index,
		port->outbound.start, port->outbound.end,
		port->pci_addr);

	/*
	 * Inbound PCI memory window
	 */

	/* Default 4GB */
	port->inbound.name  = "PCIE DMA";
	port->inbound.start = 0x00000000;
	port->inbound.end   = 0xffffffff;
	port->inbound.flags = IORESOURCE_MEM | IORESOURCE_PREFETCH;

	/* Get dma-ranges property */
	field = of_get_property(np, "dma-ranges", &rlen);
	if (!field) {
		pr_info("PCIE%d: No 'dma-ranges' property, using defaults\n",
			port->index);
	} else {
		BUILD_BUG_ON(sizeof(resource_size_t) != sizeof(u64));

		/* Limited to one inbound window for now... */
		pci_space = of_read_number(field + 0, 1);
		pci_addr  = of_read_number(field + 1, 2);
		cpu_addr  = of_read_number(field + 3, pna);
		size      = of_read_number(field + pna + 3, 2);

		port->inbound.start = cpu_addr;
		port->inbound.end   = cpu_addr + size - 1;
		port->pci_bar       = pci_addr;
	}

	pr_info("PCIE%d: Inbound %#llx (PCI) -> %#llx..%#llx (CPU)\n",
		port->index,
		port->pci_bar,
		port->inbound.start, port->inbound.end);

	if (axxia_pcie_setup(port, &res)) {
		port->sysdata.private_data = port;
		bus = pci_create_root_bus(&pdev->dev, 0,
					  &axxia_pciex_pci_ops,
					  &port->sysdata, &res);
		if (!bus)
			return 1;

		lastbus = pci_scan_child_bus(bus);
		pci_bus_update_busn_res_end(bus, lastbus);
		pci_assign_unassigned_bus_resources(bus);
		pci_bus_add_devices(bus);

		platform_set_drvdata(pdev, port);
	}

	return 0;
}

static const struct of_device_id axxia_pcie_match_table[] = {
	{.compatible = "lsi,plb-pciex",},
	{},
};

MODULE_DEVICE_TABLE(of, axxia_pcie_match_table);

static struct platform_driver axxia_pcie_driver = {
	.driver = {
		.name = "axxia-pci",
		.owner = THIS_MODULE,
		.of_match_table = axxia_pcie_match_table,
	},
	.probe = axxia_probe_pciex_bridge
};

module_platform_driver(axxia_pcie_driver);

MODULE_AUTHOR("Sangeetha Rao <sangeetha.rao@intel.com>");
MODULE_DESCRIPTION("AXXIA PCIe driver");
MODULE_LICENSE("GPL v2");
