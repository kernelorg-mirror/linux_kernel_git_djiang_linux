// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2026 Intel Corporation. All rights reserved.

#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <cxl/mailbox.h>
#include <cxl/cxl.h>
#include <cxlmem.h>

static unsigned int probe_delay_ms;
module_param(probe_delay_ms, uint, 0644);
MODULE_PARM_DESC(probe_delay_ms, "delay between devm_cxl_probe_mem() and HPA use");

static bool fail_probe;
module_param(fail_probe, bool, 0644);
MODULE_PARM_DESC(fail_probe, "fail probe after devm_cxl_probe_mem() succeeds");

static unsigned int fn_probe_delay_ms;
module_param(fn_probe_delay_ms, uint, 0644);
MODULE_PARM_DESC(fn_probe_delay_ms, "function probe delay after linking to the region");

static unsigned int revoke_delay_ms = 20;
module_param(revoke_delay_ms, uint, 0644);
MODULE_PARM_DESC(revoke_delay_ms, "time revoke spends draining HPA access");

struct mock_cxl_accel {
	struct cxl_dev_state cxlds;
	struct cxl_memdev *cxlmd;
	struct mutex lock; /* protects the state below */
	bool hpa_live;
	bool removed;
	int revokes;
};

/* Other functions sharing the region must be unbound before PF0 hears */
static int cxl_mock_fn_check_unbound(struct device *dev, void *pf0)
{
	if (!strstarts(dev_name(dev), "cxl_type2_accel_fn"))
		return 0;
	if (READ_ONCE(dev->driver))
		dev_err(pf0, "VIOLATION: %s still bound at revoke\n",
			dev_name(dev));
	return 0;
}

static void cxl_mock_accel_revoke(struct cxl_dev_state *cxlds)
{
	struct mock_cxl_accel *cxl_accel =
		container_of(cxlds, struct mock_cxl_accel, cxlds);

	guard(mutex)(&cxl_accel->lock);
	cxl_accel->revokes++;
	dev_info(cxlds->dev, "revoke #%d hpa_live=%d removed=%d\n",
		 cxl_accel->revokes, cxl_accel->hpa_live, cxl_accel->removed);
	if (cxl_accel->removed)
		dev_err(cxlds->dev, "VIOLATION: revoke after remove\n");
	if (cxl_accel->revokes > 1)
		dev_err(cxlds->dev, "VIOLATION: revoke called %d times\n",
			cxl_accel->revokes);
	bus_for_each_dev(&platform_bus_type, NULL, cxlds->dev,
			 cxl_mock_fn_check_unbound);
	/* emulate draining in-flight HPA access */
	msleep(revoke_delay_ms);
	cxl_accel->hpa_live = false;
}

static int cxl_mock_accel_probe(struct platform_device *pdev)
{
	struct mock_cxl_accel *cxl_accel;
	struct device *dev = &pdev->dev;
	struct cxl_dev_state *cxlds;
	struct cxl_memdev *cxlmd;
	struct range mock_range;
	int rc;

	cxl_accel = devm_cxl_dev_state_create(&pdev->dev, CXL_DEVTYPE_DEVMEM,
					      pdev->id + 1, 0,
					      struct mock_cxl_accel, cxlds,
					      false);
	if (!cxl_accel)
		return -ENOMEM;
	mutex_init(&cxl_accel->lock);
	platform_set_drvdata(pdev, cxl_accel);

	cxlds = &cxl_accel->cxlds;
	cxlds->media_ready = true;
	rc = cxl_set_capacity(cxlds, SZ_512M);
	if (rc)
		return rc;

	cxlmd = devm_cxl_probe_mem(cxlds, &mock_range, cxl_mock_accel_revoke);
	if (IS_ERR(cxlmd))
		return PTR_ERR(cxlmd);
	cxl_accel->cxlmd = cxlmd;

	if (probe_delay_ms) {
		dev_info(dev, "probe: sleeping %u ms before using HPA\n",
			 probe_delay_ms);
		msleep(probe_delay_ms);
	}

	scoped_guard(mutex, &cxl_accel->lock) {
		if (cxl_accel->revokes) {
			dev_info(dev, "probe: region revoked during probe\n");
			return -ENXIO;
		}
		cxl_accel->hpa_live = true;
		if (fail_probe) {
			dev_info(dev, "probe: failing after HPA setup\n");
			cxl_accel->hpa_live = false;
			cxl_accel->removed = true;
			return -EIO;
		}
	}

	dev_info(dev, "probe: using HPA %pra\n", &mock_range);
	return 0;
}

static void cxl_mock_accel_remove(struct platform_device *pdev)
{
	struct mock_cxl_accel *cxl_accel = platform_get_drvdata(pdev);

	guard(mutex)(&cxl_accel->lock);
	dev_info(&pdev->dev, "remove: hpa_live=%d revokes=%d\n",
		 cxl_accel->hpa_live, cxl_accel->revokes);
	cxl_accel->hpa_live = false;
	cxl_accel->removed = true;
}

struct mock_cxl_fn {
	struct mutex lock; /* protects the state below */
	bool hpa_live;
};

static int cxl_mock_fn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mock_cxl_fn *fn;
	struct range range;
	int rc;

	fn = devm_kzalloc(dev, sizeof(*fn), GFP_KERNEL);
	if (!fn)
		return -ENOMEM;
	mutex_init(&fn->lock);
	platform_set_drvdata(pdev, fn);

	struct device *pf0 __free(put_device) =
		bus_find_device_by_name(&platform_bus_type, NULL,
					"cxl_type2_accel.0");
	if (!pf0)
		return -EPROBE_DEFER;

	rc = cxl_get_range_and_link(pf0, dev, &range);
	dev_info(dev, "fn probe: link rc=%d\n", rc);
	if (rc)
		return rc;

	if (fn_probe_delay_ms) {
		dev_info(dev, "fn probe: sleeping %u ms after link\n",
			 fn_probe_delay_ms);
		msleep(fn_probe_delay_ms);
	}

	scoped_guard(mutex, &fn->lock)
		fn->hpa_live = true;
	dev_info(dev, "fn probe: using HPA %pra\n", &range);
	return 0;
}

static void cxl_mock_fn_remove(struct platform_device *pdev)
{
	struct mock_cxl_fn *fn = platform_get_drvdata(pdev);

	guard(mutex)(&fn->lock);
	dev_info(&pdev->dev, "fn remove: hpa_live=%d\n", fn->hpa_live);
	fn->hpa_live = false;
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
