// SPDX-License-Identifier: GPL-2.0
/*
 * MMIO Register Block
 *	PCIe r6.2, sec 6.35 MMMIO Register Block registers
 *	PCIe r6.2, sec 7.9.30 MMIO Register Block Locator
 *	CXL r3.1, sec 8.2.8 CXL Device Register Interface
 *	CXL r3.1, sec 8.1.9 Register Locator DVSEC
 *
 * Copyright (C) 2024 Intel Corporation
 */
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/cxl.h>
#include "pci.h"

static int mmio_map_regblock(struct mmio_register_map *map)
{
	struct device *host = map->host;

	map->base = ioremap(map->resource, map->max_size);
	if (!map->base) {
		dev_err(host, "failed to map registers\n");
		return -ENOMEM;
	}

	dev_dbg(host, "Mapped PCI MMIO resource %pa\n", &map->resource);

	return 0;
}

static void mmio_unmap_regblock(struct mmio_register_map *map)
{
	iounmap(map->base);
	map->base = NULL;
}

static int pci_probe_mmio_regs(struct mmio_register_map *map)
{
	u32 cap_array1, cap_array2;
	void __iomem *base = map->base;
	int cap, cap_count;

	if (map->reg_type != PCI_REGLOC_RBI_MCAP)
		return -EINVAL;

	cap_array1 = readl(base + PCI_MCAP_ARRAY1_OFFSET);
	if (FIELD_GET(PCI_MCAP_ARRAY1_ID_MASK, cap_array1) !=
	    PCI_MCAP_ARRAY1_CAP_ID)
		return -EINVAL;

	if (FIELD_GET(PCI_MCAP_ARRAY1_TYPE_MASK, cap_array1) != 0)
		return -EINVAL;

	cap_array2 = readl(base + PCI_MCAP_ARRAY2_OFFSET);
	cap_count = FIELD_GET(PCI_MCAP_ARRAY2_COUNT_MASK, cap_array2);

	for (cap = 1; cap <= cap_count; cap++) {
		u32 offset, length, vendor_id;
		struct pci_reg_map *rmap;
		u16 cap_id;

		cap_id = FIELD_GET(PCI_MCAP_HDR_CAP_ID_MASK,
				   readl(base + cap * 0x10));
		offset = readl(base + cap * 0x10 + 0x4);
		length = readl(base + cap * 0x10 + 0x8);
		vendor_id = readl(base + cap * 0x10 + 0xc);

		if (vendor_id != PCI_MCAP_HDR_VENDOR_ID_PCI)
			continue;

		switch (cap_id) {
		case PCI_MCAP_HDR_CAP_ID_MMB:
			rmap = &map->pci_mmio_map.mbox;
			break;
		case PCI_MCAP_HDR_CAP_ID_MMPT:
			rmap = &map->pci_mmio_map.mmpt;
			break;
		default:
			continue;
		}

		rmap->valid = true;
		rmap->offset = offset;
		rmap->size = length;
	}

	return 0;
}

/**
 * mmio_setup_regs() - Probe and decode MMIO block registers
 * @map: MMIO register map
 * @probe_regs: Callback to handle the specific register block
 *
 * Return: 0 if register block is decoded, negative error code otherwise
 */
int mmio_setup_regs(struct mmio_register_map *map,
		    int (*probe_regs)(struct mmio_register_map *map))
{
	int rc;

	rc = mmio_map_regblock(map);
	if (rc)
		return rc;

	rc = probe_regs(map);
	mmio_unmap_regblock(map);

	return rc;
}
EXPORT_SYMBOL_GPL(mmio_setup_regs);

/**
 * pci_mmio_setup_regs() - PCI wrapper for calling mmio_setup_regs()
 * @map: MMIO register map
 *
 * Return: 0 if register block is setup, negative error code otherwise
 */
int pci_mmio_setup_regs(struct mmio_register_map *map)
{
	return mmio_setup_regs(map, pci_probe_mmio_regs);
}
EXPORT_SYMBOL_GPL(pci_mmio_setup_regs);

static bool decode_regblock(struct pci_dev *pdev, u32 reg_lo, u32 reg_hi,
			    struct mmio_register_map *map)
{
	u8 reg_type = FIELD_GET(PCI_MRBL_REG_LOCATOR_BLOCK_ID_MASK, reg_lo);
	int bar = FIELD_GET(PCI_MRBL_REG_LOCATOR_BIR_MASK, reg_lo);
	u64 offset = ((u64)reg_hi << 32) |
		     (reg_lo & PCI_MRBL_REG_LOCATOR_BLOCK_OFF_LOW_MASK);

	if (reg_type == PCI_REGLOC_RBI_EMPTY)
		return false;

	if (offset > pci_resource_len(pdev, bar)) {
		dev_warn(&pdev->dev,
			 "BAR%d: %pr: too small (offset: %pa, type: %d)\n", bar,
			 &pdev->resource[bar], &offset, reg_type);
		return false;
	}

	map->reg_type = reg_type;
	map->resource = pci_resource_start(pdev, bar) + offset;
	map->max_size = pci_resource_len(pdev, bar) - offset;

	return true;
}

/**
 * mmio_find_regblock() - Locate MMIO register blocks by type
 * @pdev: The PCI device to enumerate
 * @type: Register Block ID
 * @map: Enumeration output, clobbered on error
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * One or more register blocks may exist, search for them by @type.
 */
int mmio_find_regblock_instance(struct pci_dev *pdev,
				enum mrb_dev_type dev_type, int type,
				struct mmio_register_map *map,
				int index)
{
	u32 mrbl_size, mrbl_blocks;
	int instance = 0;
	int mloc, i;

	*map = (struct mmio_register_map) {
		.dev_type = dev_type,
		.host = &pdev->dev,
		.resource = MMIO_RESOURCE_NONE,
	};

	switch (dev_type) {
	case DEV_TYPE_PCI:
		mloc = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_MRBL);
		pci_read_config_dword(pdev, mloc + PCI_MRBL_CAP, &mrbl_size);
		mrbl_size = PCI_MRBL_LEN(mrbl_size);
		mloc += PCI_MRBL_REG_LOCATOR_BLOCK1_OFFSET;
		mrbl_blocks = (mrbl_size - PCI_MRBL_REG_LOCATOR_BLOCK1_OFFSET) / 8;
		break;
	case DEV_TYPE_CXL:
		mloc = pci_find_dvsec_capability(pdev, PCI_VENDOR_ID_CXL,
						 CXL_DVSEC_REG_LOCATOR);
		pci_read_config_dword(pdev, mloc + PCI_DVSEC_HEADER1, &mrbl_size);
		mrbl_size = PCI_DVSEC_HEADER1_LEN(mrbl_size);
		mloc += CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET;
		mrbl_blocks = (mrbl_size - CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET) / 8;
		break;
	default:
		return -EINVAL;
	}

	if (!mloc)
		return -ENXIO;

	for (i = 0; i < mrbl_blocks; i++, mloc += 8) {
		u32 reg_lo, reg_hi;

		pci_read_config_dword(pdev, mloc, &reg_lo);
		pci_read_config_dword(pdev, mloc + 4, &reg_hi);

		if (!decode_regblock(pdev, reg_lo, reg_hi, map))
			continue;

		if (map->reg_type == type) {
			if (index == instance)
				return 0;
			instance++;
		}
	}

	map->resource = MMIO_RESOURCE_NONE;

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(mmio_find_regblock_instance);

/**
 * pci_mmio_find_regblock() - Locate PCI MMIO register blocks by type
 * @pdev: The PCI device to enumerate
 * @type: Register Block ID
 * @map: Enumeration output, clobbered on error
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * One or more register blocks may exist, search for them by @type.
 */
int pci_mmio_find_regblock(struct pci_dev *pdev, enum pci_regloc_type type,
			   struct mmio_register_map *map)
{
	if (type >= PCI_REGLOC_RBI_TYPES || type == PCI_REGLOC_RBI_EMPTY)
		return -EINVAL;

	return mmio_find_regblock_instance(pdev, DEV_TYPE_PCI, type, map, 0);
}
EXPORT_SYMBOL_GPL(pci_mmio_find_regblock);

/**
 * devm_pci_mmio_iomap_block() - Helper to request region and map the MMIO block
 * @dev: Device to map against
 * @addr: Start location of the mapping
 * @length: Length of the mapping
 *
 * Return: 0 if MMIO address mapped, negative error code otherwise
 */
void __iomem *devm_pci_mmio_iomap_block(struct device *dev, resource_size_t addr,
					resource_size_t length)
{
	void __iomem *ret_val;
	struct resource *res;

	if (WARN_ON_ONCE(addr == MMIO_RESOURCE_NONE))
		return NULL;

	res = devm_request_mem_region(dev, addr, length, dev_name(dev));
	if (!res) {
		resource_size_t end = addr + length - 1;

		dev_err(dev, "Failed to request region %pa-%pa\n", &addr, &end);
		return NULL;
	}

	ret_val = devm_ioremap(dev, addr, length);
	if (!ret_val)
		dev_err(dev, "Failed to map region %pr\n", res);

	return ret_val;
}
EXPORT_SYMBOL_GPL(devm_pci_mmio_iomap_block);

/**
 * pci_map_mmio_regs() - Mapping function for PCI MMIO mbox and mmpt registers
 * @map: Register map info
 * @regs: struct of pointers to be setup with mapped virtual address
 *
 * Return: 0 if MMIO address mapped, negative error code otherwise
 */
int pci_map_mmio_regs(const struct mmio_register_map *map,
		      struct pci_mmio_regs *regs)
{
	struct device *host = map->host;
	resource_size_t phys_addr = map->resource;
	struct mapinfo {
		const struct pci_reg_map *rmap;
		void __iomem **addr;
	} mapinfo[] = {
		{ &map->pci_mmio_map.mbox, &regs->mbox, },
		{ &map->pci_mmio_map.mmpt, &regs->mmpt, },
	};

	for (int i = 0; i < ARRAY_SIZE(mapinfo); i++) {
		struct mapinfo *mi = &mapinfo[i];
		resource_size_t length;
		resource_size_t addr;

		if (!mi->rmap->valid)
			continue;

		addr = phys_addr + mi->rmap->offset;
		length = mi->rmap->size;
		*mi->addr = devm_pci_mmio_iomap_block(host, addr, length);
		if (!*mi->addr)
			return -ENOMEM;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pci_map_mmio_regs);
