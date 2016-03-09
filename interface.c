/*
 * Greybus interface code
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include "greybus.h"

/* interface sysfs attributes */
#define gb_interface_attr(field, type)					\
static ssize_t field##_show(struct device *dev,				\
			    struct device_attribute *attr,		\
			    char *buf)					\
{									\
	struct gb_interface *intf = to_gb_interface(dev);		\
	return scnprintf(buf, PAGE_SIZE, type"\n", intf->field);	\
}									\
static DEVICE_ATTR_RO(field)

gb_interface_attr(ddbl1_manufacturer_id, "0x%08x");
gb_interface_attr(ddbl1_product_id, "0x%08x");
gb_interface_attr(interface_id, "%u");
gb_interface_attr(vendor_id, "0x%08x");
gb_interface_attr(product_id, "0x%08x");
gb_interface_attr(vendor_string, "%s");
gb_interface_attr(product_string, "%s");
gb_interface_attr(serial_number, "0x%016llx");

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct gb_interface *intf = to_gb_interface(dev);

	return scnprintf(buf, PAGE_SIZE, "%u.%u\n", intf->version_major,
			 intf->version_minor);
}
static DEVICE_ATTR_RO(version);

static struct attribute *interface_attrs[] = {
	&dev_attr_ddbl1_manufacturer_id.attr,
	&dev_attr_ddbl1_product_id.attr,
	&dev_attr_interface_id.attr,
	&dev_attr_vendor_id.attr,
	&dev_attr_product_id.attr,
	&dev_attr_vendor_string.attr,
	&dev_attr_product_string.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(interface);


/* XXX This could be per-host device */
static DEFINE_SPINLOCK(gb_interfaces_lock);

// FIXME, odds are you don't want to call this function, rework the caller to
// not need it please.
struct gb_interface *gb_interface_find(struct gb_host_device *hd,
				       u8 interface_id)
{
	struct gb_interface *intf;

	list_for_each_entry(intf, &hd->interfaces, links)
		if (intf->interface_id == interface_id)
			return intf;

	return NULL;
}

static void gb_interface_release(struct device *dev)
{
	struct gb_interface *intf = to_gb_interface(dev);

	kfree(intf->product_string);
	kfree(intf->vendor_string);

	if (intf->control)
		gb_control_destroy(intf->control);

	kfree(intf);
}

struct device_type greybus_interface_type = {
	.name =		"greybus_interface",
	.release =	gb_interface_release,
};

/*
 * A Greybus module represents a user-replaceable component on an Ara
 * phone.  An interface is the physical connection on that module.  A
 * module may have more than one interface.
 *
 * Create a gb_interface structure to represent a discovered interface.
 * The position of interface within the Endo is encoded in "interface_id"
 * argument.
 *
 * Returns a pointer to the new interfce or a null pointer if a
 * failure occurs due to memory exhaustion.
 */
struct gb_interface *gb_interface_create(struct gb_host_device *hd,
					 u8 interface_id)
{
	struct gb_interface *intf;

	intf = kzalloc(sizeof(*intf), GFP_KERNEL);
	if (!intf)
		return NULL;

	intf->hd = hd;		/* XXX refcount? */
	intf->interface_id = interface_id;
	INIT_LIST_HEAD(&intf->bundles);
	INIT_LIST_HEAD(&intf->manifest_descs);

	/* Invalid device id to start with */
	intf->device_id = GB_DEVICE_ID_BAD;

	intf->dev.parent = &hd->dev;
	intf->dev.bus = &greybus_bus_type;
	intf->dev.type = &greybus_interface_type;
	intf->dev.groups = interface_groups;
	intf->dev.dma_mask = hd->dev.dma_mask;
	device_initialize(&intf->dev);
	dev_set_name(&intf->dev, "%d-%d", hd->bus_id, interface_id);

	intf->control = gb_control_create(intf);
	if (!intf->control) {
		put_device(&intf->dev);
		return NULL;
	}

	spin_lock_irq(&gb_interfaces_lock);
	list_add(&intf->links, &hd->interfaces);
	spin_unlock_irq(&gb_interfaces_lock);

	return intf;
}

/*
 * Tear down a previously set up interface.
 */
void gb_interface_remove(struct gb_interface *intf)
{
	struct gb_bundle *bundle;
	struct gb_bundle *next;

	/*
	 * Disable the control-connection early to avoid operation timeouts
	 * when the interface is already gone.
	 */
	if (intf->disconnected)
		gb_control_disable(intf->control);

	list_for_each_entry_safe(bundle, next, &intf->bundles, links)
		gb_bundle_destroy(bundle);

	if (device_is_registered(&intf->dev)) {
		device_del(&intf->dev);
		dev_info(&intf->dev, "Interface removed\n");
	}

	gb_control_disable(intf->control);

	spin_lock_irq(&gb_interfaces_lock);
	list_del(&intf->links);
	spin_unlock_irq(&gb_interfaces_lock);

	put_device(&intf->dev);
}

void gb_interfaces_remove(struct gb_host_device *hd)
{
	struct gb_interface *intf, *temp;

	list_for_each_entry_safe(intf, temp, &hd->interfaces, links)
		gb_interface_remove(intf);
}

/**
 * gb_interface_init
 *
 * Create connection for control CPort and then request/parse manifest.
 * Finally initialize all the bundles to set routes via SVC and initialize all
 * connections.
 */
int gb_interface_init(struct gb_interface *intf)
{
	struct gb_bundle *bundle, *tmp;
	int ret, size;
	void *manifest;

	/* Establish control connection */
	ret = gb_control_enable(intf->control);
	if (ret)
		return ret;

	/* Get manifest size using control protocol on CPort */
	size = gb_control_get_manifest_size_operation(intf);
	if (size <= 0) {
		dev_err(&intf->dev, "failed to get manifest size: %d\n", size);
		if (size)
			return size;
		else
			return -EINVAL;
	}

	manifest = kmalloc(size, GFP_KERNEL);
	if (!manifest)
		return -ENOMEM;

	/* Get manifest using control protocol on CPort */
	ret = gb_control_get_manifest_operation(intf, manifest, size);
	if (ret) {
		dev_err(&intf->dev, "failed to get manifest: %d\n", ret);
		goto free_manifest;
	}

	/*
	 * Parse the manifest and build up our data structures representing
	 * what's in it.
	 */
	if (!gb_manifest_parse(intf, manifest, size)) {
		dev_err(&intf->dev, "failed to parse manifest\n");
		ret = -EINVAL;
		goto free_manifest;
	}

	ret = gb_control_get_interface_version_operation(intf);
	if (ret)
		goto free_manifest;

	ret = gb_control_get_bundle_versions(intf->control);
	if (ret)
		goto free_manifest;

	/* Register the interface and its bundles. */
	ret = device_add(&intf->dev);
	if (ret) {
		dev_err(&intf->dev, "failed to register interface: %d\n", ret);
		goto free_manifest;
	}

	dev_info(&intf->dev, "Interface added: VID=0x%08x, PID=0x%08x\n",
		 intf->vendor_id, intf->product_id);
	dev_info(&intf->dev, "DDBL1 Manufacturer=0x%08x, Product=0x%08x\n",
		 intf->ddbl1_manufacturer_id, intf->ddbl1_product_id);

	list_for_each_entry_safe_reverse(bundle, tmp, &intf->bundles, links) {
		ret = gb_bundle_add(bundle);
		if (ret) {
			gb_bundle_destroy(bundle);
			continue;
		}
	}

	ret = 0;

free_manifest:
	kfree(manifest);
	return ret;
}
