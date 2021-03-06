 /*
  * drivers/edac/axxia_edac.c
  *
  * EDAC Driver for Avago's Axxia 5500
  *
  * Copyright (C) 2010 LSI Inc.
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program; if not, write to the Free Software
  * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
  *
  */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/edac.h>
#include <mach/ncr.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#include "edac_core.h"
#include "edac_module.h"

#define LSI_EDAC_MOD_STR     "lsi_edac"
#define CORES_PER_CLUSTER 4

/* Private structure for common edac device */
struct lsi_edac_dev_info {
	void __iomem *vbase;
	struct platform_device *pdev;
	char *ctl_name;
	char *blk_name;
	int edac_idx;
	u32 sm0_region;
	u32 sm1_region;
	void __iomem *apb2ser3_region;
	void __iomem *dickens_L3[8];
	struct edac_device_ctl_info *edac_dev;
	void (*init)(struct lsi_edac_dev_info *dev_info);
	void (*exit)(struct lsi_edac_dev_info *dev_info);
	void (*check)(struct edac_device_ctl_info *edac_dev);
};

static void lsi_error_init(struct lsi_edac_dev_info *dev_info)
{
}

static void lsi_error_exit(struct lsi_edac_dev_info *dev_info)
{
}

void log_cpumerrsr(void *edac)
{
	struct edac_device_ctl_info *edac_dev =
		(struct edac_device_ctl_info *)edac;
	u32 tmp1, tmp2, count0, count1;
	unsigned long setVal;
	int i;
	struct lsi_edac_dev_info *dev_info;

	dev_info = (struct lsi_edac_dev_info *) edac_dev->pvt_info;

	/* Read cp15 for CPUMERRSR counts */
	asm volatile("mrrc\tp15, 0, %0, %1, c15" : "=r"(tmp1),
		"=r"(tmp2));
	if (tmp1 & 0x80000000) {
		count0 = (tmp2) & 0x000000ff;
		count1 = ((tmp2) & 0x0000ff00) >> 8;

		/* increment correctable error counts */
		for (i = 0; i < count0+count1; i++) {
			edac_device_handle_ce(edac_dev, 0,
				raw_smp_processor_id(), edac_dev->ctl_name);
		}

		/* Clear the valid bit */
		tmp1 = 0x80000000;
		tmp2 = 0;
		asm volatile("mcrr\tp15, 0, %0, %1, c15" : : "r"(tmp1),
			"r"(tmp2));
	}
	if (tmp2 & 0x80000000) {
		setVal = readl(dev_info->apb2ser3_region + 0xdc);
		/* set bit 3 in pscratch reg */
		setVal = (setVal) | (0x1 << 3);
		writel(setVal, dev_info->apb2ser3_region + 0xdc);
		pr_info("CPU uncorrectable error\n");
		machine_restart(NULL);
	}
}


/* Check for CPU Errors */
static void lsi_cpu_error_check(struct edac_device_ctl_info *edac_dev)
{
	/* execute on current cpu */
	log_cpumerrsr(edac_dev);

	/* send ipis to execute on other cpus */
	smp_call_function(log_cpumerrsr, edac_dev, 1);

}

void log_l2merrsr(void *edac)
{
	struct edac_device_ctl_info *edac_dev =
			(struct edac_device_ctl_info *)edac;
	u32 tmp1, tmp2, count0, count1;
	unsigned long setVal;
	int i;
	struct lsi_edac_dev_info *dev_info;

	dev_info = (struct lsi_edac_dev_info *) edac_dev->pvt_info;

	/* Read cp15 for L2MERRSR counts */
	asm volatile("mrrc\tp15, 1, %0, %1, c15" : "=r"(tmp1),
		"=r"(tmp2));
	if (tmp1 & 0x80000000) {
		count0 = (tmp2) & 0x000000ff;
		count1 = ((tmp2) & 0x0000ff00) >> 8;

		/* increment correctable error counts */
		for (i = 0; i < count0+count1; i++) {
			edac_device_handle_ce(edac_dev, 0,
				raw_smp_processor_id()/CORES_PER_CLUSTER,
				edac_dev->ctl_name);
		}

		/* Clear the valid bit */
		tmp1 = 0x80000000;
		tmp2 = 0;
		asm volatile("mcrr\tp15, 1, %0, %1, c15" : : "r"(tmp1),
			"r"(tmp2));
	}
	if (tmp2 & 0x80000000) {
		setVal = readl(dev_info->apb2ser3_region + 0xdc);
		/* set bit 3 in pscratch reg */
		setVal = (setVal) | (0x1 << 3);
		writel(setVal, dev_info->apb2ser3_region + 0xdc);
		pr_info("L2 uncorrectable error\n");
		machine_restart(NULL);
	}
}

/* Check for L2 Errors */
static void lsi_l2_error_check(struct edac_device_ctl_info *edac_dev)
{
	/* 4 cores per cluster */
	int nr_cluster_ids = ((nr_cpu_ids - 1) / CORES_PER_CLUSTER) + 1;
	int i, j, cpu;

	/* execute on current cpu */
	log_l2merrsr(edac_dev);

	for (i = 0; i < nr_cluster_ids; i++) {
		/* No need to run on local cluster. */
		if (i == (raw_smp_processor_id() / CORES_PER_CLUSTER))
			continue;
		/*
		 * Have some core in each cluster execute this,
		 * Start with the first core on that cluster.
		 */
		cpu = i * CORES_PER_CLUSTER;
		for (j = cpu; j < cpu + CORES_PER_CLUSTER; j++) {
			if (cpu_online(j)) {
				smp_call_function_single(j, log_l2merrsr,
					edac_dev, 1);
				break;
			}
		}
	}
}

/* Check for L3 Errors */
static void lsi_l3_error_check(struct edac_device_ctl_info *edac_dev)
{
	unsigned long regVal1, regVal2, setVal;
	unsigned count = 0;
	int i, instance;
	struct lsi_edac_dev_info *dev_info;

	dev_info = (struct lsi_edac_dev_info *) edac_dev->pvt_info;

	for (instance = 0; instance < 8; instance++) {
		regVal1 = readl(dev_info->dickens_L3[instance]);
		regVal2 = readl(dev_info->dickens_L3[instance] + 4);
		/* First error valid */
		if (regVal2 & 0x40000000) {
			if (regVal2 & 0x30000000) {
				setVal = readl(dev_info->apb2ser3_region +
					0xdc);
				/* set bit 3 in pscratch reg */
				setVal = (setVal) | (0x1 << 3);
				writel(setVal, dev_info->apb2ser3_region +
					0xdc);
				/* Fatal error */
				pr_info("L3 uncorrectable error\n");
				machine_restart(NULL);
			}
			count = (regVal2 & 0x07fff800) >> 11;
			for (i = 0; i < count; i++)
				edac_device_handle_ce(edac_dev, 0,
					instance, edac_dev->ctl_name);
			/* clear the valid bit */
			writel(0x48000000, dev_info->dickens_L3[instance] + 84);
		}
	}
}

/* Check for SysMem Errors */
static void lsi_sm_error_check(struct edac_device_ctl_info *edac_dev)
{
	unsigned long sm0_regVal, sm1_regVal, clearVal, setVal;
	struct lsi_edac_dev_info *dev_info;

	dev_info = (struct lsi_edac_dev_info *) edac_dev->pvt_info;

	/* SM0 is instance 0 */
	ncr_read(dev_info->sm0_region, 0x410, 4, &sm0_regVal);
	if (sm0_regVal & 0x8) {
		/* single bit and multiple bit correctable errors */
		edac_device_handle_ce(edac_dev, 0, 0, edac_dev->ctl_name);
		/* Clear bits */
		clearVal = 0x8;
		ncr_write(dev_info->sm0_region, 0x548, 4, &clearVal);
	}
	if (sm0_regVal & 0x40) {
		setVal = readl(dev_info->apb2ser3_region + 0xdc);
		/* set bit 3 in pscratch reg */
		setVal = (setVal) | (0x1 << 3);
		writel(setVal, dev_info->apb2ser3_region + 0xdc);
		/* single bit and multiple bit uncorrectable errors */
		pr_info("SM0 uncorrectable error\n");
		machine_restart(NULL);
	}

	/* SM1 is instance 1 */
	ncr_read(dev_info->sm1_region, 0x410, 4, &sm1_regVal);
	if (sm1_regVal & 0x8) {
		/* single bit and multiple bit correctable errors */
		edac_device_handle_ce(edac_dev, 0, 1, edac_dev->ctl_name);
		/* Clear bits */
		clearVal = 0x8;
		ncr_write(dev_info->sm1_region, 0x548, 4, &clearVal);
	}
	if (sm1_regVal & 0x40) {
		setVal = readl(dev_info->apb2ser3_region + 0xdc);
		/* set bit 3 in pscratch reg */
		setVal = (setVal) | (0x1 << 3);
		writel(setVal, dev_info->apb2ser3_region + 0xdc);
		/* single bit and multiple bit uncorrectable errors */
		pr_info("SM1 uncorrectable error\n");
		machine_restart(NULL);
	}
}


static struct lsi_edac_dev_info lsi_edac_devs[] = {
	{
		.ctl_name = "LSI_CPU",
		.blk_name = "cpumerrsr",
		.init = lsi_error_init,
		.exit = lsi_error_exit,
		.check = lsi_cpu_error_check
	},
	{
		.ctl_name = "LSI_L2",
		.blk_name = "l2merrsr",
		.init = lsi_error_init,
		.exit = lsi_error_exit,
		.check = lsi_l2_error_check
	},
	{
		.ctl_name = "LSI_L3",
		.blk_name = "l3merrsr",
		.init = lsi_error_init,
		.exit = lsi_error_exit,
		.check = lsi_l3_error_check
	},
	{
		.ctl_name = "LSI_SM",
		.blk_name = "ECC",
		.init = lsi_error_init,
		.exit = lsi_error_exit,
		.check = lsi_sm_error_check
	},
	{0} /* Terminated by NULL */
};



/* static void lsi_add_edac_devices(void __iomem *vbase) */
static void lsi_add_edac_devices(struct platform_device *pdev)
{
	struct lsi_edac_dev_info *dev_info = NULL;
	/* 4 cores per cluster */
	int nr_cluster_ids = ((nr_cpu_ids - 1) / CORES_PER_CLUSTER) + 1;
	struct resource *io0, *io1;
	struct device_node *np = pdev->dev.of_node;
	void __iomem *apb2ser3_region;
	int i;

	apb2ser3_region = of_iomap(np, 2);
	if (!apb2ser3_region) {
		dev_err(&pdev->dev, "LSI_apb2ser3_region iomap error\n");
		goto err2;
	}

	for (dev_info = &lsi_edac_devs[0]; dev_info->init; dev_info++) {
		dev_info->pdev = platform_device_register_simple(
		dev_info->ctl_name, 0, NULL, 0);
		if (IS_ERR(dev_info->pdev)) {
			pr_info("Can't register platform device for %s\n",
				dev_info->ctl_name);
			continue;
		}
		/*
		 * Don't have to allocate private structure but
		 * make use of cpc925_devs[] instead.
		 */
		dev_info->edac_idx = edac_device_alloc_index();

		if (strcmp(dev_info->ctl_name, "LSI_SM") == 0) {
			io0 = platform_get_resource(pdev, IORESOURCE_MEM, 0);
			if (!io0) {
				dev_err(&pdev->dev, "Unable to get mem resource\n");
				goto err2;
			}
			io1 = platform_get_resource(pdev, IORESOURCE_MEM, 1);
			if (!io1) {
				dev_err(&pdev->dev, "Unable to get mem resource\n");
				goto err2;
			}

			dev_info->sm0_region = io0->start;
			dev_info->sm1_region = io1->start;
			dev_info->apb2ser3_region = apb2ser3_region;
			dev_info->edac_dev =
			edac_device_alloc_ctl_info(0, dev_info->ctl_name,
			1, dev_info->blk_name, 2, 0,
				NULL, 0, dev_info->edac_idx);
		} else if (strcmp(dev_info->ctl_name, "LSI_L3") == 0) {
			/* 8 L3 caches */
			for (i = 0; i < 8; i++) {
				dev_info->dickens_L3[i] = of_iomap(np, i+3);
				if (!dev_info->dickens_L3[i]) {
					dev_err(&pdev->dev,
						"LSI_L3 iomap error\n");
					goto err2;
				}
			}
			dev_info->apb2ser3_region = apb2ser3_region;
			dev_info->edac_dev =
			edac_device_alloc_ctl_info(0, dev_info->ctl_name,
			1, dev_info->blk_name, 8, 0, NULL, 0,
			dev_info->edac_idx);
		} else if (strcmp(dev_info->ctl_name, "LSI_CPU") == 0) {
			dev_info->apb2ser3_region = apb2ser3_region;
			dev_info->edac_dev =
			edac_device_alloc_ctl_info(0, dev_info->ctl_name,
			1, dev_info->blk_name, num_possible_cpus(), 0, NULL,
			0, dev_info->edac_idx);
		} else if (strcmp(dev_info->ctl_name, "LSI_L2") == 0) {
			dev_info->apb2ser3_region = apb2ser3_region;
			dev_info->edac_dev =
			edac_device_alloc_ctl_info(0, dev_info->ctl_name,
				1, dev_info->blk_name, nr_cluster_ids, 0, NULL,
				0, dev_info->edac_idx);
		} else {
			dev_info->edac_dev =
			edac_device_alloc_ctl_info(0, dev_info->ctl_name,
			1, dev_info->blk_name, 1, 0,
			NULL, 0, dev_info->edac_idx);
		}
		if (!dev_info->edac_dev) {
			pr_info("No memory for edac device\n");
			goto err1;
		}

		dev_info->edac_dev->pvt_info = dev_info;
		dev_info->edac_dev->dev = &dev_info->pdev->dev;
		dev_info->edac_dev->ctl_name = dev_info->ctl_name;
		dev_info->edac_dev->mod_name = LSI_EDAC_MOD_STR;
		dev_info->edac_dev->dev_name = dev_name(&dev_info->pdev->dev);

		if (edac_op_state == EDAC_OPSTATE_POLL)
			dev_info->edac_dev->edac_check = dev_info->check;

		if (dev_info->init)
			dev_info->init(dev_info);

		if (edac_device_add_device(dev_info->edac_dev) > 0) {
			pr_info("Unable to add edac device for %s\n",
					dev_info->ctl_name);
			goto err2;
		}
		pr_info("Successfully added edac device for %s\n",
				dev_info->ctl_name);

		continue;
err2:
		if (dev_info->exit)
			dev_info->exit(dev_info);
		edac_device_free_ctl_info(dev_info->edac_dev);
err1:
		platform_device_unregister(dev_info->pdev);
	}
}


static int lsi_edac_probe(struct platform_device *pdev)
{
	edac_op_state = EDAC_OPSTATE_POLL;
	lsi_add_edac_devices(pdev);
	return 0;
}

static int lsi_edac_remove(struct platform_device *pdev)
{
	platform_device_unregister(pdev);
	return 0;
}

static struct of_device_id lsi_edac_match[] = {
	{
	.type   = "edac",
	.compatible = "lsi,edac",
	},
	{},
};

static struct platform_driver lsi_edac_driver = {
	.probe = lsi_edac_probe,
	.remove = lsi_edac_remove,
	.driver = {
		.name = "lsi_edac",
		.of_match_table = lsi_edac_match,
	}
};

module_platform_driver(lsi_edac_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sangeetha Rao <sangeetha.rao@avagotech.com>");
