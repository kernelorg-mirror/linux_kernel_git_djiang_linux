// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2026 Intel Corporation. All rights reserved.

#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sizes.h>
#include <cxl/mailbox.h>
#include <cxl/cxl.h>
#include <cxlmem.h>

static unsigned int remove_delay_ms;
module_param(remove_delay_ms, uint, 0644);
MODULE_PARM_DESC(remove_delay_ms, "time remove() keeps using the HPA");

static unsigned int probe_delay_ms;
module_param(probe_delay_ms, uint, 0644);
MODULE_PARM_DESC(probe_delay_ms, "probe delay after devm_cxl_probe_mem()");

static unsigned int fn_probe_delay_ms;
module_param(fn_probe_delay_ms, uint, 0644);
MODULE_PARM_DESC(fn_probe_delay_ms, "function probe delay after registering as a region user");

static unsigned int fn_remove_delay_ms;
module_param(fn_remove_delay_ms, uint, 0644);
MODULE_PARM_DESC(fn_remove_delay_ms, "time a function's remove() keeps using the HPA");

static bool fn_fail_probe;
module_param(fn_fail_probe, bool, 0644);
MODULE_PARM_DESC(fn_fail_probe, "fail function probe after registering as a region user");

static bool fail_probe;
module_param(fail_probe, bool, 0644);
MODULE_PARM_DESC(fail_probe, "fail probe after devm_cxl_probe_mem() succeeds");

static bool pf0_no_cxl;
module_param(pf0_no_cxl, bool, 0644);
MODULE_PARM_DESC(pf0_no_cxl, "bind PF0 without creating a CXL memdev");

struct mock_cxl_accel {
	struct cxl_dev_state cxlds;
	struct cxl_memdev *cxlmd;
	struct range hpa;
};

/* The HPA must still nest under its region resource while the driver uses it */
static void cxl_mock_accel_check_hpa(struct device *dev, struct range *hpa,
				     const char *when)
{
	resource_size_t len = range_len(hpa);
	struct resource *res;

	res = request_mem_region(hpa->start, len, "cxl_mock_accel");
	if (!res) {
		dev_info(dev, "%s: HPA %pra busy, can't check\n", when, hpa);
		return;
	}
	dev_info(dev, "%s: HPA %pra under %s\n", when, hpa,
		 res->parent ? res->parent->name : "(none)");
	if (!res->parent || !strstarts(res->parent->name, "region"))
		dev_err(dev, "VIOLATION: %s: HPA %pra not reserved by a region\n",
			when, hpa);
	release_mem_region(hpa->start, len);
}

static int cxl_mock_accel_probe(struct platform_device *pdev)
{
	struct mock_cxl_accel *cxl_accel;
	struct device *dev = &pdev->dev;
	struct cxl_dev_state *cxlds;
	struct cxl_memdev *cxlmd;
	struct range mock_range;
	int rc;

	if (pf0_no_cxl) {
		dev_info(dev, "probe: binding without CXL\n");
		return 0;
	}

	cxl_accel = devm_cxl_dev_state_create(&pdev->dev, CXL_DEVTYPE_DEVMEM,
					      pdev->id + 1, 0,
					      struct mock_cxl_accel, cxlds,
					      false);
	if (!cxl_accel)
		return -ENOMEM;

	cxlds = &cxl_accel->cxlds;
	cxlds->media_ready = true;
	rc = cxl_set_capacity(cxlds, SZ_512M);
	if (rc)
		return rc;

	cxlmd = devm_cxl_probe_mem(cxlds, &mock_range);
	if (IS_ERR(cxlmd))
		return PTR_ERR(cxlmd);
	cxl_accel->cxlmd = cxlmd;
	cxl_accel->hpa = mock_range;
	platform_set_drvdata(pdev, cxl_accel);

	if (probe_delay_ms) {
		dev_info(dev, "probe: sleeping %u ms after HPA setup\n",
			 probe_delay_ms);
		msleep(probe_delay_ms);
	}
	cxl_mock_accel_check_hpa(dev, &cxl_accel->hpa, "probe");
	if (fail_probe) {
		dev_info(dev, "probe: failing after HPA setup\n");
		return -EIO;
	}

	return 0;
}

static void cxl_mock_accel_remove(struct platform_device *pdev)
{
	struct mock_cxl_accel *cxl_accel = platform_get_drvdata(pdev);

	/* pf0_no_cxl: bound without CXL */
	if (!cxl_accel)
		return;
	cxl_mock_accel_check_hpa(&pdev->dev, &cxl_accel->hpa, "remove");
	if (remove_delay_ms) {
		dev_info(&pdev->dev, "remove: using HPA for %u ms\n",
			 remove_delay_ms);
		msleep(remove_delay_ms);
		cxl_mock_accel_check_hpa(&pdev->dev, &cxl_accel->hpa,
					 "remove end");
	}
}

struct mock_cxl_fn {
	struct range hpa;
	bool no_cxl;
};

static int cxl_mock_fn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mock_cxl_fn *fn;
	int rc;

	fn = devm_kzalloc(dev, sizeof(*fn), GFP_KERNEL);
	if (!fn)
		return -ENOMEM;
	platform_set_drvdata(pdev, fn);

	struct device *pf0 __free(put_device) =
		bus_find_device_by_name(&platform_bus_type, NULL,
					"cxl_type2_accel.0");
	if (!pf0)
		return -EPROBE_DEFER;

	rc = devm_cxl_add_region_user(pf0, dev, &fn->hpa);
	dev_info(dev, "fn probe: register rc=%d\n", rc);
	if (rc == -ENODEV) {
		/* PF0 doesn't use CXL: bind without the HPA */
		fn->no_cxl = true;
		return 0;
	}
	if (rc)
		return rc;

	if (fn_probe_delay_ms) {
		dev_info(dev, "fn probe: sleeping %u ms after register\n",
			 fn_probe_delay_ms);
		msleep(fn_probe_delay_ms);
	}
	cxl_mock_accel_check_hpa(dev, &fn->hpa, "fn probe");
	if (fn_fail_probe) {
		dev_info(dev, "fn probe: failing after register\n");
		return -EIO;
	}
	return 0;
}

static void cxl_mock_fn_remove(struct platform_device *pdev)
{
	struct mock_cxl_fn *fn = platform_get_drvdata(pdev);

	if (fn->no_cxl)
		return;
	if (fn_remove_delay_ms) {
		dev_info(&pdev->dev, "fn remove: sleeping %u ms\n",
			 fn_remove_delay_ms);
		msleep(fn_remove_delay_ms);
	}
	cxl_mock_accel_check_hpa(&pdev->dev, &fn->hpa, "fn remove");
}

enum { MOCK_PF0, MOCK_FN };

static int cxl_mock_probe(struct platform_device *pdev)
{
	if (platform_get_device_id(pdev)->driver_data == MOCK_FN)
		return cxl_mock_fn_probe(pdev);
	return cxl_mock_accel_probe(pdev);
}

static void cxl_mock_remove(struct platform_device *pdev)
{
	if (platform_get_device_id(pdev)->driver_data == MOCK_FN)
		cxl_mock_fn_remove(pdev);
	else
		cxl_mock_accel_remove(pdev);
}

static const struct platform_device_id cxl_mock_accel_ids[] = {
	{ .name = "cxl_type2_accel", .driver_data = MOCK_PF0 },
	{ .name = "cxl_type2_accel_fn", .driver_data = MOCK_FN },
	{ }
};
MODULE_DEVICE_TABLE(platform, cxl_mock_accel_ids);

static struct platform_driver cxl_mock_accel_driver = {
	.probe = cxl_mock_probe,
	.remove = cxl_mock_remove,
	.id_table = cxl_mock_accel_ids,
	.driver = {
		.name = KBUILD_MODNAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(cxl_mock_accel_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("cxl_test: accelerator device mock module");
MODULE_IMPORT_NS("CXL");
