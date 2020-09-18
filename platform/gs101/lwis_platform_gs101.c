/*
 * Google LWIS GS101 Platform-Specific Functions
 *
 * Copyright (c) 2020 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "lwis_platform_gs101.h"

#include <linux/iommu.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "lwis_commands.h"
#include "lwis_device_dpm.h"
#include "lwis_debug.h"
#include "lwis_platform.h"

/* Uncomment to let kernel panic when IOMMU hits a page fault. */
/* TODO: Add error handling to propagate SysMMU errors back to userspace,
 * so we don't need to panic here. */
#define ENABLE_PAGE_FAULT_PANIC

int lwis_platform_probe(struct lwis_device *lwis_dev)
{
	struct lwis_platform *platform;

	if (!lwis_dev) {
		return -ENODEV;
	}

	platform = kzalloc(sizeof(struct lwis_platform), GFP_KERNEL);
	if (IS_ERR_OR_NULL(platform)) {
		return -ENOMEM;
	}
	lwis_dev->platform = platform;

	/* Enable runtime power management for the platform device */
	pm_runtime_enable(&lwis_dev->plat_dev->dev);

	return 0;
}

static int __attribute__((unused))
lwis_iommu_fault_handler(struct iommu_fault *fault, void *param)
{
	struct lwis_device *lwis_dev = (struct lwis_device *)param;

	pr_err("############ LWIS IOMMU PAGE FAULT ############\n");
	pr_err("\n");
	pr_err("Device: %s IOMMU Page Fault at Address: %llu Flag: 0x%08x\n",
		lwis_dev->name, fault->event.addr, fault->event.flags);
	pr_err("\n");
	lwis_debug_print_transaction_info(lwis_dev);
	pr_err("\n");
	lwis_debug_print_event_states_info(lwis_dev);
	pr_err("\n");
	lwis_debug_print_buffer_info(lwis_dev);
	pr_err("\n");
	pr_err("###############################################\n");

#ifdef ENABLE_PAGE_FAULT_PANIC
	return NOTIFY_BAD;
#else
	return NOTIFY_OK;
#endif /* ENABLE_PAGE_FAULT_PANIC */
}

int lwis_platform_device_enable(struct lwis_device *lwis_dev)
{
	int ret;
	struct lwis_platform *platform;

	const uint32_t int_qos = 465000;
	const uint32_t mif_qos = 2093000;
	const uint32_t core_clock_qos = 67000;
	/* const uint32_t hpg_qos = 1; */

	if (!lwis_dev) {
		return -ENODEV;
	}

	platform = lwis_dev->platform;
	if (!platform) {
		return -ENODEV;
	}

	/* Upref the runtime power management controls for the platform dev */
	ret = pm_runtime_get_sync(&lwis_dev->plat_dev->dev);
	if (ret < 0) {
		pr_err("Unable to enable platform device\n");
		return ret;
	}

	if (lwis_dev->has_iommu) {
		/* Activate IOMMU for the platform device */
		ret = iommu_register_device_fault_handler(
			&lwis_dev->plat_dev->dev,
			lwis_iommu_fault_handler,
			lwis_dev);
		if (ret < 0) {
			pr_err("Failed to register fault handler for the device: %d\n",
			       ret);
			return ret;
		}
	}

	/*
	 * PM_QOS_CPU_ONLINE_MIN is not defined in 5.4 branch, will need to
	 * revisit and see if a replacement is needed.
	 */
#if 0
	/* Set hardcoded DVFS levels */
	if (!exynos_pm_qos_request_active(&platform->pm_qos_hpg)) {
		exynos_pm_qos_add_request(&platform->pm_qos_hpg,
					  PM_QOS_CPU_ONLINE_MIN, hpg_qos);
	}
#endif

	ret = lwis_platform_update_qos(lwis_dev, mif_qos, CLOCK_FAMILY_MIF);
	if (ret < 0) {
		dev_err(lwis_dev->dev, "Failed to enable MIF clock\n");
		return ret;
	}
	ret = lwis_platform_update_qos(lwis_dev, int_qos, CLOCK_FAMILY_INT);
	if (ret < 0) {
		dev_err(lwis_dev->dev, "Failed to enable INT clock\n");
		return ret;
	}

	if (lwis_dev->clock_family != CLOCK_FAMILY_INVALID &&
	    lwis_dev->clock_family < NUM_CLOCK_FAMILY) {
		ret = lwis_platform_update_qos(lwis_dev, core_clock_qos,
					       lwis_dev->clock_family);
		if (ret < 0) {
			dev_err(lwis_dev->dev, "Failed to enable core clock\n");
			return ret;
		}
	}

	return 0;
}

int lwis_platform_device_disable(struct lwis_device *lwis_dev)
{
	struct lwis_platform *platform;

	if (!lwis_dev) {
		return -ENODEV;
	}

	platform = lwis_dev->platform;
	if (!platform) {
		return -ENODEV;
	}

	/* We can't remove fault handlers, so there's no call corresponding
	 * to the iommu_register_device_fault_handler above */

	lwis_platform_remove_qos(lwis_dev);

	if (lwis_dev->has_iommu) {
		/* Deactivate IOMMU */
		iommu_unregister_device_fault_handler(&lwis_dev->plat_dev->dev);
	}

	/* Disable platform device */
	return pm_runtime_put_sync(&lwis_dev->plat_dev->dev);
}

int lwis_platform_update_qos(struct lwis_device *lwis_dev, uint32_t value,
			     enum lwis_clock_family clock_family)
{
	struct lwis_platform *platform;
	struct exynos_pm_qos_request *qos_req;
	int qos_class;

	if (!lwis_dev) {
		return -ENODEV;
	}

	platform = lwis_dev->platform;
	if (!platform) {
		return -ENODEV;
	}

	if (value == 0) {
		value = EXYNOS_PM_QOS_DEFAULT_VALUE;
	}

	switch (clock_family) {
	case CLOCK_FAMILY_INTCAM:
		qos_req = &platform->pm_qos_int_cam;
		qos_class = PM_QOS_INTCAM_THROUGHPUT;
		break;
	case CLOCK_FAMILY_CAM:
		qos_req = &platform->pm_qos_cam;
		qos_class = PM_QOS_CAM_THROUGHPUT;
		break;
	case CLOCK_FAMILY_TNR:
#if defined(CONFIG_SOC_GS101)
		qos_req = &platform->pm_qos_tnr;
		qos_class = PM_QOS_TNR_THROUGHPUT;
#endif
		break;
	case CLOCK_FAMILY_MIF:
		qos_req = &platform->pm_qos_mem;
		qos_class = PM_QOS_BUS_THROUGHPUT;
		break;
	case CLOCK_FAMILY_INT:
		qos_req = &platform->pm_qos_int;
		qos_class = PM_QOS_DEVICE_THROUGHPUT;
		break;
	default:
		dev_err(lwis_dev->dev, "%s clk family %d is invalid\n",
			lwis_dev->name, lwis_dev->clock_family);
		return -EINVAL;
	}

	if (!exynos_pm_qos_request_active(qos_req)) {
		exynos_pm_qos_add_request(qos_req, qos_class, value);
	} else {
		exynos_pm_qos_update_request(qos_req, value);
	}

	dev_info(lwis_dev->dev,
		 "Updating clock for clock_family %d, freq to %u\n",
		 clock_family, value);

	return 0;
}

int lwis_platform_remove_qos(struct lwis_device *lwis_dev)
{
	struct lwis_platform *platform;

	if (!lwis_dev) {
		return -ENODEV;
	}

	platform = lwis_dev->platform;
	if (!platform) {
		return -ENODEV;
	}

	if (exynos_pm_qos_request_active(&platform->pm_qos_int)) {
		exynos_pm_qos_remove_request(&platform->pm_qos_int);
	}
	if (exynos_pm_qos_request_active(&platform->pm_qos_mem)) {
		exynos_pm_qos_remove_request(&platform->pm_qos_mem);
	}

	/*
	 * pm_qos_hpg is not being used, see comments above regarding
	 * PM_QOS_CPU_ONLINE_MIN
	 */
#if 0
	if (exynos_pm_qos_request_active(&platform->pm_qos_hpg)) {
		exynos_pm_qos_remove_request(&platform->pm_qos_hpg);
	}
#endif

	switch (lwis_dev->clock_family) {
	case CLOCK_FAMILY_INTCAM:
		if (exynos_pm_qos_request_active(&platform->pm_qos_int_cam)) {
			exynos_pm_qos_remove_request(&platform->pm_qos_int_cam);
		}
		break;
	case CLOCK_FAMILY_CAM:
		if (exynos_pm_qos_request_active(&platform->pm_qos_cam)) {
			exynos_pm_qos_remove_request(&platform->pm_qos_cam);
		}
		break;
	case CLOCK_FAMILY_TNR:
#if defined(CONFIG_SOC_GS101)
		if (exynos_pm_qos_request_active(&platform->pm_qos_tnr)) {
			exynos_pm_qos_remove_request(&platform->pm_qos_tnr);
		}
#endif
		break;
	default:
		break;
	}
	return 0;
}

int lwis_platform_update_bts(struct lwis_device *lwis_dev,
			     unsigned int bw_kb_peak, unsigned int bw_kb_read,
			     unsigned int bw_kb_write)
{
	return 0;
}