// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2020 Intel Corporation. */
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/cxl.h>
#include <cxlmem.h>
#include <cxlpci.h>
#include <pmu.h>

#include "core.h"

/**
 * DOC: cxl registers
 *
 * CXL device capabilities are enumerated by PCI DVSEC (Designated
 * Vendor-specific) and / or descriptors provided by platform firmware.
 * They can be defined as a set like the device and component registers
 * mandated by CXL Section 8.1.12.2 Memory Device PCIe Capabilities and
 * Extended Capabilities, or they can be individual capabilities
 * appended to bridged and endpoint devices.
 *
 * Provide common infrastructure for enumerating and mapping these
 * discrete capabilities.
 */

/**
 * cxl_probe_component_regs() - Detect CXL Component register blocks
 * @dev: Host device of the @base mapping
 * @base: Mapping containing the HDM Decoder Capability Header
 * @map: Map object describing the register block information found
 *
 * See CXL 2.0 8.2.4 Component Register Layout and Definition
 * See CXL 2.0 8.2.5.5 CXL Device Register Interface
 *
 * Probe for component register information and return it in map object.
 */
static void cxl_probe_component_regs(struct device *dev, void __iomem *base,
				     struct cxl_component_reg_map *map)
{
	int cap, cap_count;
	u32 cap_array;

	*map = (struct cxl_component_reg_map) { 0 };

	/*
	 * CXL.cache and CXL.mem registers are at offset 0x1000 as defined in
	 * CXL 2.0 8.2.4 Table 141.
	 */
	base += CXL_CM_OFFSET;

	cap_array = readl(base + CXL_CM_CAP_HDR_OFFSET);

	if (FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, cap_array) != CM_CAP_HDR_CAP_ID) {
		dev_err(dev,
			"Couldn't locate the CXL.cache and CXL.mem capability array header.\n");
		return;
	}

	/* It's assumed that future versions will be backward compatible */
	cap_count = FIELD_GET(CXL_CM_CAP_HDR_ARRAY_SIZE_MASK, cap_array);

	for (cap = 1; cap <= cap_count; cap++) {
		void __iomem *register_block;
		struct pci_reg_map *rmap;
		u16 cap_id, offset;
		u32 length, hdr;

		hdr = readl(base + cap * 0x4);

		cap_id = FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, hdr);
		offset = FIELD_GET(CXL_CM_CAP_PTR_MASK, hdr);
		register_block = base + offset;
		hdr = readl(register_block);

		rmap = NULL;
		switch (cap_id) {
		case CXL_CM_CAP_CAP_ID_HDM: {
			int decoder_cnt;

			dev_dbg(dev, "found HDM decoder capability (0x%x)\n",
				offset);

			decoder_cnt = cxl_hdm_decoder_count(hdr);
			length = 0x20 * decoder_cnt + 0x10;
			rmap = &map->hdm_decoder;
			break;
		}
		case CXL_CM_CAP_CAP_ID_RAS:
			dev_dbg(dev, "found RAS capability (0x%x)\n",
				offset);
			length = CXL_RAS_CAPABILITY_LENGTH;
			rmap = &map->ras;
			break;
		default:
			dev_dbg(dev, "Unknown CM cap ID: %d (0x%x)\n", cap_id,
				offset);
			break;
		}

		if (!rmap)
			continue;
		rmap->valid = true;
		rmap->id = cap_id;
		rmap->offset = CXL_CM_OFFSET + offset;
		rmap->size = length;
	}
}

/**
 * cxl_probe_device_regs() - Detect CXL Device register blocks
 * @dev: Host device of the @base mapping
 * @base: Mapping of CXL 2.0 8.2.8 CXL Device Register Interface
 * @map: Map object describing the register block information found
 *
 * Probe for device register information and return it in map object.
 */
static void cxl_probe_device_regs(struct device *dev, void __iomem *base,
				  struct cxl_device_reg_map *map)
{
	int cap, cap_count;
	u64 cap_array;

	*map = (struct cxl_device_reg_map){ 0 };

	cap_array = readq(base + CXLDEV_CAP_ARRAY_OFFSET);
	if (FIELD_GET(CXLDEV_CAP_ARRAY_ID_MASK, cap_array) !=
	    CXLDEV_CAP_ARRAY_CAP_ID)
		return;

	cap_count = FIELD_GET(CXLDEV_CAP_ARRAY_COUNT_MASK, cap_array);

	for (cap = 1; cap <= cap_count; cap++) {
		struct pci_reg_map *rmap;
		u32 offset, length;
		u16 cap_id;

		cap_id = FIELD_GET(CXLDEV_CAP_HDR_CAP_ID_MASK,
				   readl(base + cap * 0x10));
		offset = readl(base + cap * 0x10 + 0x4);
		length = readl(base + cap * 0x10 + 0x8);

		rmap = NULL;
		switch (cap_id) {
		case CXLDEV_CAP_CAP_ID_DEVICE_STATUS:
			dev_dbg(dev, "found Status capability (0x%x)\n", offset);
			rmap = &map->status;
			break;
		case CXLDEV_CAP_CAP_ID_PRIMARY_MAILBOX:
			dev_dbg(dev, "found Mailbox capability (0x%x)\n", offset);
			rmap = &map->mbox;
			break;
		case CXLDEV_CAP_CAP_ID_SECONDARY_MAILBOX:
			dev_dbg(dev, "found Secondary Mailbox capability (0x%x)\n", offset);
			break;
		case CXLDEV_CAP_CAP_ID_MEMDEV:
			dev_dbg(dev, "found Memory Device capability (0x%x)\n", offset);
			rmap = &map->memdev;
			break;
		default:
			if (cap_id >= 0x8000)
				dev_dbg(dev, "Vendor cap ID: %#x offset: %#x\n", cap_id, offset);
			else
				dev_dbg(dev, "Unknown cap ID: %#x offset: %#x\n", cap_id, offset);
			break;
		}

		if (!rmap)
			continue;
		rmap->valid = true;
		rmap->id = cap_id;
		rmap->offset = offset;
		rmap->size = length;
	}
}

int cxl_map_component_regs(const struct mmio_register_map *map,
			   struct cxl_component_regs *regs,
			   unsigned long map_mask)
{
	struct device *host = map->host;
	struct mapinfo {
		const struct pci_reg_map *rmap;
		void __iomem **addr;
	} mapinfo[] = {
		{ &map->cxl_component_map.hdm_decoder, &regs->hdm_decoder },
		{ &map->cxl_component_map.ras, &regs->ras },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(mapinfo); i++) {
		struct mapinfo *mi = &mapinfo[i];
		resource_size_t addr;
		resource_size_t length;

		if (!mi->rmap->valid)
			continue;
		if (!test_bit(mi->rmap->id, &map_mask))
			continue;
		addr = map->resource + mi->rmap->offset;
		length = mi->rmap->size;
		*mi->addr = devm_pci_mmio_iomap_block(host, addr, length);
		if (!*(mi->addr))
			return -ENOMEM;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_map_component_regs, CXL);

int cxl_map_device_regs(const struct mmio_register_map *map,
			struct cxl_device_regs *regs)
{
	struct device *host = map->host;
	resource_size_t phys_addr = map->resource;
	struct mapinfo {
		const struct pci_reg_map *rmap;
		void __iomem **addr;
	} mapinfo[] = {
		{ &map->cxl_device_map.status, &regs->status, },
		{ &map->cxl_device_map.mbox, &regs->mbox, },
		{ &map->cxl_device_map.memdev, &regs->memdev, },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(mapinfo); i++) {
		struct mapinfo *mi = &mapinfo[i];
		resource_size_t length;
		resource_size_t addr;

		if (!mi->rmap->valid)
			continue;

		addr = phys_addr + mi->rmap->offset;
		length = mi->rmap->size;
		*mi->addr = devm_pci_mmio_iomap_block(host, addr, length);
		if (!*(mi->addr))
			return -ENOMEM;
	}

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_map_device_regs, CXL);

/**
 * cxl_count_regblock() - Count instances of a given regblock type.
 * @pdev: The CXL PCI device to enumerate.
 * @type: Register Block Indicator id
 *
 * Some regblocks may be repeated. Count how many instances.
 *
 * Return: count of matching regblocks.
 */
int cxl_count_regblock(struct pci_dev *pdev, enum cxl_regloc_type type)
{
	struct mmio_register_map map;
	int rc, count = 0;

	while (1) {
		rc = cxl_find_dvsec_regblock_instance(pdev, type, &map, count);
		if (rc)
			return count;
		count++;
	}
}
EXPORT_SYMBOL_NS_GPL(cxl_count_regblock, CXL);

int cxl_map_pmu_regs(struct mmio_register_map *map, struct cxl_pmu_regs *regs)
{
	struct device *dev = map->host;
	resource_size_t phys_addr;

	phys_addr = map->resource;
	regs->pmu = devm_pci_mmio_iomap_block(dev, phys_addr, CXL_PMU_REGMAP_SIZE);
	if (!regs->pmu)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_map_pmu_regs, CXL);

static int cxl_probe_dvsec_regs(struct mmio_register_map *map)
{
	struct cxl_component_reg_map *comp_map;
	struct cxl_device_reg_map *dev_map;
	struct device *host = map->host;
	void __iomem *base = map->base;

	switch (map->reg_type) {
	case CXL_REGLOC_RBI_COMPONENT:
		comp_map = &map->cxl_component_map;
		cxl_probe_component_regs(host, base, comp_map);
		dev_dbg(host, "Set up component registers\n");
		break;
	case CXL_REGLOC_RBI_MEMDEV:
		dev_map = &map->cxl_device_map;
		cxl_probe_device_regs(host, base, dev_map);
		if (!dev_map->status.valid || !dev_map->mbox.valid ||
		    !dev_map->memdev.valid) {
			dev_err(host, "registers not found: %s%s%s\n",
				!dev_map->status.valid ? "status " : "",
				!dev_map->mbox.valid ? "mbox " : "",
				!dev_map->memdev.valid ? "memdev " : "");
			return -ENXIO;
		}

		dev_dbg(host, "Probing device registers...\n");
		break;
	default:
		break;
	}

	return 0;
}

int cxl_setup_dvsec_regs(struct mmio_register_map *map)
{
	return mmio_setup_regs(map, cxl_probe_dvsec_regs);
}
EXPORT_SYMBOL_NS_GPL(cxl_setup_dvsec_regs, CXL);

u16 cxl_rcrb_to_aer(struct device *dev, resource_size_t rcrb)
{
	void __iomem *addr;
	u16 offset = 0;
	u32 cap_hdr;

	if (WARN_ON_ONCE(rcrb == CXL_RESOURCE_NONE))
		return 0;

	if (!request_mem_region(rcrb, SZ_4K, dev_name(dev)))
		return 0;

	addr = ioremap(rcrb, SZ_4K);
	if (!addr)
		goto out;

	cap_hdr = readl(addr + offset);
	while (PCI_EXT_CAP_ID(cap_hdr) != PCI_EXT_CAP_ID_ERR) {
		offset = PCI_EXT_CAP_NEXT(cap_hdr);

		/* Offset 0 terminates capability list. */
		if (!offset)
			break;
		cap_hdr = readl(addr + offset);
	}

	if (offset)
		dev_dbg(dev, "found AER extended capability (0x%x)\n", offset);

	iounmap(addr);
out:
	release_mem_region(rcrb, SZ_4K);

	return offset;
}

resource_size_t __rcrb_to_component(struct device *dev, struct cxl_rcrb_info *ri,
				    enum cxl_rcrb which)
{
	resource_size_t component_reg_phys;
	resource_size_t rcrb = ri->base;
	void __iomem *addr;
	u32 bar0, bar1;
	u16 cmd;
	u32 id;

	if (which == CXL_RCRB_UPSTREAM)
		rcrb += SZ_4K;

	/*
	 * RCRB's BAR[0..1] point to component block containing CXL
	 * subsystem component registers. MEMBAR extraction follows
	 * the PCI Base spec here, esp. 64 bit extraction and memory
	 * ranges alignment (6.0, 7.5.1.2.1).
	 */
	if (!request_mem_region(rcrb, SZ_4K, "CXL RCRB"))
		return CXL_RESOURCE_NONE;
	addr = ioremap(rcrb, SZ_4K);
	if (!addr) {
		dev_err(dev, "Failed to map region %pr\n", addr);
		release_mem_region(rcrb, SZ_4K);
		return CXL_RESOURCE_NONE;
	}

	id = readl(addr + PCI_VENDOR_ID);
	cmd = readw(addr + PCI_COMMAND);
	bar0 = readl(addr + PCI_BASE_ADDRESS_0);
	bar1 = readl(addr + PCI_BASE_ADDRESS_1);
	iounmap(addr);
	release_mem_region(rcrb, SZ_4K);

	/*
	 * Sanity check, see CXL 3.0 Figure 9-8 CXL Device that Does Not
	 * Remap Upstream Port and Component Registers
	 */
	if (id == U32_MAX) {
		if (which == CXL_RCRB_DOWNSTREAM)
			dev_err(dev, "Failed to access Downstream Port RCRB\n");
		return CXL_RESOURCE_NONE;
	}
	if (!(cmd & PCI_COMMAND_MEMORY))
		return CXL_RESOURCE_NONE;
	/* The RCRB is a Memory Window, and the MEM_TYPE_1M bit is obsolete */
	if (bar0 & (PCI_BASE_ADDRESS_MEM_TYPE_1M | PCI_BASE_ADDRESS_SPACE_IO))
		return CXL_RESOURCE_NONE;

	component_reg_phys = bar0 & PCI_BASE_ADDRESS_MEM_MASK;
	if (bar0 & PCI_BASE_ADDRESS_MEM_TYPE_64)
		component_reg_phys |= ((u64)bar1) << 32;

	if (!component_reg_phys)
		return CXL_RESOURCE_NONE;

	/* MEMBAR is block size (64k) aligned. */
	if (!IS_ALIGNED(component_reg_phys, CXL_COMPONENT_REG_BLOCK_SIZE))
		return CXL_RESOURCE_NONE;

	return component_reg_phys;
}

resource_size_t cxl_rcd_component_reg_phys(struct device *dev,
					   struct cxl_dport *dport)
{
	if (!dport->rch)
		return CXL_RESOURCE_NONE;
	return __rcrb_to_component(dev, &dport->rcrb, CXL_RCRB_UPSTREAM);
}
EXPORT_SYMBOL_NS_GPL(cxl_rcd_component_reg_phys, CXL);

/**
 * cxl_find_regblock_instance() - Locate a register block by type / index
 * @pdev: The CXL PCI device to enumerate.
 * @type: Register Block Indicator id
 * @map: Enumeration output, clobbered on error
 * @index: Index into which particular instance of a regblock wanted in the
 *	   order found in register locator DVSEC.
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * A CXL DVSEC may point to one or more register blocks, search for them
 * by @type and @index.
 */
int cxl_find_dvsec_regblock_instance(struct pci_dev *pdev,
				     enum cxl_regloc_type type,
				     struct mmio_register_map *map, int index)
{
	if (type >= CXL_REGLOC_RBI_TYPES || type == CXL_REGLOC_RBI_EMPTY)
		return -EINVAL;

	return mmio_find_regblock_instance(pdev, DEV_TYPE_CXL, type, map, index);
}
EXPORT_SYMBOL_NS_GPL(cxl_find_dvsec_regblock_instance, CXL);

/**
 * cxl_find_dvsec_regblock() - Locate CXL DVSEC register blocks by type
 * @pdev: The PCI device to enumerate
 * @type: Register Block ID
 * @map: Enumeration output, clobbered on error
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * One or more register blocks may exist, search for them by @type.
 */
int cxl_find_dvsec_regblock(struct pci_dev *pdev, enum cxl_regloc_type type,
			    struct mmio_register_map *map)
{
	return cxl_find_dvsec_regblock_instance(pdev, type, map, 0);
}
EXPORT_SYMBOL_NS_GPL(cxl_find_dvsec_regblock, CXL);
