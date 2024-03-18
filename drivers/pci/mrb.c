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
