/*
 * Greybus connections
 *
 * Copyright 2014 Google Inc.
 * Copyright 2014 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include <linux/workqueue.h>

#include "greybus.h"


static void gb_connection_kref_release(struct kref *kref);


static DEFINE_SPINLOCK(gb_connections_lock);
static DEFINE_MUTEX(gb_connection_mutex);


/* Caller holds gb_connection_mutex. */
static struct gb_connection *
gb_connection_intf_find(struct gb_interface *intf, u16 cport_id)
{
	struct gb_host_device *hd = intf->hd;
	struct gb_connection *connection;

	list_for_each_entry(connection, &hd->connections, hd_links) {
		if (connection->intf == intf &&
				connection->intf_cport_id == cport_id)
			return connection;
	}

	return NULL;
}

static void gb_connection_get(struct gb_connection *connection)
{
	kref_get(&connection->kref);
}

static void gb_connection_put(struct gb_connection *connection)
{
	kref_put(&connection->kref, gb_connection_kref_release);
}

/*
 * Returns a reference-counted pointer to the connection if found.
 */
static struct gb_connection *
gb_connection_hd_find(struct gb_host_device *hd, u16 cport_id)
{
	struct gb_connection *connection;
	unsigned long flags;

	spin_lock_irqsave(&gb_connections_lock, flags);
	list_for_each_entry(connection, &hd->connections, hd_links)
		if (connection->hd_cport_id == cport_id) {
			gb_connection_get(connection);
			goto found;
		}
	connection = NULL;
found:
	spin_unlock_irqrestore(&gb_connections_lock, flags);

	return connection;
}

/*
 * Callback from the host driver to let us know that data has been
 * received on the bundle.
 */
void greybus_data_rcvd(struct gb_host_device *hd, u16 cport_id,
			u8 *data, size_t length)
{
	struct gb_connection *connection;

	connection = gb_connection_hd_find(hd, cport_id);
	if (!connection) {
		dev_err(&hd->dev,
			"nonexistent connection (%zu bytes dropped)\n", length);
		return;
	}
	gb_connection_recv(connection, data, length);
	gb_connection_put(connection);
}
EXPORT_SYMBOL_GPL(greybus_data_rcvd);

static void gb_connection_kref_release(struct kref *kref)
{
	struct gb_connection *connection;

	connection = container_of(kref, struct gb_connection, kref);

	kfree(connection);
}

static void gb_connection_init_name(struct gb_connection *connection)
{
	u16 hd_cport_id = connection->hd_cport_id;
	u16 cport_id = 0;
	u8 intf_id = 0;

	if (connection->intf) {
		intf_id = connection->intf->interface_id;
		cport_id = connection->intf_cport_id;
	}

	snprintf(connection->name, sizeof(connection->name),
			"%u/%u:%u", hd_cport_id, intf_id, cport_id);
}

/*
 * gb_connection_create() - create a Greybus connection
 * @hd:			host device of the connection
 * @hd_cport_id:	host-device cport id, or -1 for dynamic allocation
 * @intf:		remote interface, or NULL for static connections
 * @bundle:		remote-interface bundle (may be NULL)
 * @cport_id:		remote-interface cport id, or 0 for static connections
 *
 * Create a Greybus connection, representing the bidirectional link
 * between a CPort on a (local) Greybus host device and a CPort on
 * another Greybus interface.
 *
 * A connection also maintains the state of operations sent over the
 * connection.
 *
 * Serialised against concurrent create and destroy using the
 * gb_connection_mutex.
 *
 * Return: A pointer to the new connection if successful, or NULL otherwise.
 */
static struct gb_connection *
gb_connection_create(struct gb_host_device *hd, int hd_cport_id,
				struct gb_interface *intf,
				struct gb_bundle *bundle, int cport_id)
{
	struct gb_connection *connection;
	struct ida *id_map = &hd->cport_id_map;
	int ida_start, ida_end;

	if (hd_cport_id < 0) {
		ida_start = 0;
		ida_end = hd->num_cports;
	} else if (hd_cport_id < hd->num_cports) {
		ida_start = hd_cport_id;
		ida_end = hd_cport_id + 1;
	} else {
		dev_err(&hd->dev, "cport %d not available\n", hd_cport_id);
		return NULL;
	}

	mutex_lock(&gb_connection_mutex);

	if (intf && gb_connection_intf_find(intf, cport_id)) {
		dev_err(&intf->dev, "cport %u already in use\n", cport_id);
		goto err_unlock;
	}

	hd_cport_id = ida_simple_get(id_map, ida_start, ida_end, GFP_KERNEL);
	if (hd_cport_id < 0)
		goto err_unlock;

	connection = kzalloc(sizeof(*connection), GFP_KERNEL);
	if (!connection)
		goto err_remove_ida;

	connection->hd_cport_id = hd_cport_id;
	connection->intf_cport_id = cport_id;
	connection->hd = hd;
	connection->intf = intf;

	connection->bundle = bundle;
	connection->state = GB_CONNECTION_STATE_DISABLED;

	atomic_set(&connection->op_cycle, 0);
	mutex_init(&connection->mutex);
	spin_lock_init(&connection->lock);
	INIT_LIST_HEAD(&connection->operations);

	connection->wq = alloc_workqueue("%s:%d", WQ_UNBOUND, 1,
					 dev_name(&hd->dev), hd_cport_id);
	if (!connection->wq)
		goto err_free_connection;

	kref_init(&connection->kref);

	gb_connection_init_name(connection);

	spin_lock_irq(&gb_connections_lock);
	list_add(&connection->hd_links, &hd->connections);

	if (bundle)
		list_add(&connection->bundle_links, &bundle->connections);
	else
		INIT_LIST_HEAD(&connection->bundle_links);

	spin_unlock_irq(&gb_connections_lock);

	mutex_unlock(&gb_connection_mutex);

	return connection;

err_free_connection:
	kfree(connection);
err_remove_ida:
	ida_simple_remove(id_map, hd_cport_id);
err_unlock:
	mutex_unlock(&gb_connection_mutex);

	return NULL;
}

struct gb_connection *
gb_connection_create_static(struct gb_host_device *hd, u16 hd_cport_id)
{
	return gb_connection_create(hd, hd_cport_id, NULL, NULL, 0);
}

struct gb_connection *
gb_connection_create_control(struct gb_interface *intf)
{
	return gb_connection_create(intf->hd, -1, intf, NULL, 0);
}

struct gb_connection *
gb_connection_create_dynamic(struct gb_bundle *bundle, u16 cport_id)
{
	struct gb_interface *intf = bundle->intf;

	return gb_connection_create(intf->hd, -1, intf, bundle, cport_id);
}
EXPORT_SYMBOL_GPL(gb_connection_create_dynamic);

static int gb_connection_hd_cport_enable(struct gb_connection *connection)
{
	struct gb_host_device *hd = connection->hd;
	int ret;

	if (!hd->driver->cport_enable)
		return 0;

	ret = hd->driver->cport_enable(hd, connection->hd_cport_id);
	if (ret) {
		dev_err(&hd->dev,
			"failed to enable host cport: %d\n", ret);
		return ret;
	}

	return 0;
}

static void gb_connection_hd_cport_disable(struct gb_connection *connection)
{
	struct gb_host_device *hd = connection->hd;

	if (!hd->driver->cport_disable)
		return;

	hd->driver->cport_disable(hd, connection->hd_cport_id);
}

/*
 * Request the SVC to create a connection from AP's cport to interface's
 * cport.
 */
static int
gb_connection_svc_connection_create(struct gb_connection *connection)
{
	struct gb_host_device *hd = connection->hd;
	struct gb_interface *intf;
	int ret;

	if (gb_connection_is_static(connection))
		return 0;

	intf = connection->intf;
	ret = gb_svc_connection_create(hd->svc,
			hd->svc->ap_intf_id,
			connection->hd_cport_id,
			intf->interface_id,
			connection->intf_cport_id,
			intf->boot_over_unipro);
	if (ret) {
		dev_err(&connection->hd->dev,
			"%s: failed to create svc connection: %d\n",
			connection->name, ret);
		return ret;
	}

	return 0;
}

static void
gb_connection_svc_connection_destroy(struct gb_connection *connection)
{
	if (gb_connection_is_static(connection))
		return;

	gb_svc_connection_destroy(connection->hd->svc,
				  connection->hd->svc->ap_intf_id,
				  connection->hd_cport_id,
				  connection->intf->interface_id,
				  connection->intf_cport_id);
}

/* Inform Interface about active CPorts */
static int gb_connection_control_connected(struct gb_connection *connection)
{
	struct gb_control *control;
	u16 cport_id = connection->intf_cport_id;
	int ret;

	if (gb_connection_is_static(connection))
		return 0;

	control = connection->intf->control;

	if (connection == control->connection)
		return 0;

	ret = gb_control_connected_operation(control, cport_id);
	if (ret) {
		dev_err(&connection->bundle->dev,
			"failed to connect cport: %d\n", ret);
		return ret;
	}

	return 0;
}

/* Inform Interface about inactive CPorts */
static void
gb_connection_control_disconnected(struct gb_connection *connection)
{
	struct gb_control *control;
	u16 cport_id = connection->intf_cport_id;
	int ret;

	if (gb_connection_is_static(connection))
		return;

	control = connection->intf->control;

	if (connection == control->connection)
		return;

	ret = gb_control_disconnected_operation(control, cport_id);
	if (ret) {
		dev_warn(&connection->bundle->dev,
			 "failed to disconnect cport: %d\n", ret);
	}
}

/*
 * Cancel all active operations on a connection.
 *
 * Locking: Called with connection lock held and state set to DISABLED.
 */
static void gb_connection_cancel_operations(struct gb_connection *connection,
						int errno)
{
	struct gb_operation *operation;

	while (!list_empty(&connection->operations)) {
		operation = list_last_entry(&connection->operations,
						struct gb_operation, links);
		gb_operation_get(operation);
		spin_unlock_irq(&connection->lock);

		if (gb_operation_is_incoming(operation))
			gb_operation_cancel_incoming(operation, errno);
		else
			gb_operation_cancel(operation, errno);

		gb_operation_put(operation);

		spin_lock_irq(&connection->lock);
	}
}

/*
 * Cancel all active incoming operations on a connection.
 *
 * Locking: Called with connection lock held and state set to ENABLED_TX.
 */
static void
gb_connection_flush_incoming_operations(struct gb_connection *connection,
						int errno)
{
	struct gb_operation *operation;
	bool incoming;

	while (!list_empty(&connection->operations)) {
		incoming = false;
		list_for_each_entry(operation, &connection->operations,
								links) {
			if (gb_operation_is_incoming(operation)) {
				gb_operation_get(operation);
				incoming = true;
				break;
			}
		}

		if (!incoming)
			break;

		spin_unlock_irq(&connection->lock);

		/* FIXME: flush, not cancel? */
		gb_operation_cancel_incoming(operation, errno);
		gb_operation_put(operation);

		spin_lock_irq(&connection->lock);
	}
}

int gb_connection_enable(struct gb_connection *connection,
				gb_request_handler_t handler)
{
	int ret;

	mutex_lock(&connection->mutex);

	if (connection->state == GB_CONNECTION_STATE_ENABLED)
		goto out_unlock;

	if (connection->state == GB_CONNECTION_STATE_ENABLED_TX) {
		if (!handler)
			goto out_unlock;

		spin_lock_irq(&connection->lock);
		connection->handler = handler;
		connection->state = GB_CONNECTION_STATE_ENABLED;
		spin_unlock_irq(&connection->lock);

		goto out_unlock;
	}

	ret = gb_connection_hd_cport_enable(connection);
	if (ret)
		goto err_unlock;

	ret = gb_connection_svc_connection_create(connection);
	if (ret)
		goto err_hd_cport_disable;

	spin_lock_irq(&connection->lock);
	connection->handler = handler;
	if (handler)
		connection->state = GB_CONNECTION_STATE_ENABLED;
	else
		connection->state = GB_CONNECTION_STATE_ENABLED_TX;
	spin_unlock_irq(&connection->lock);

	ret = gb_connection_control_connected(connection);
	if (ret)
		goto err_svc_destroy;

out_unlock:
	mutex_unlock(&connection->mutex);

	return 0;

err_svc_destroy:
	spin_lock_irq(&connection->lock);
	connection->state = GB_CONNECTION_STATE_DISABLED;
	gb_connection_cancel_operations(connection, -ESHUTDOWN);
	connection->handler = NULL;
	spin_unlock_irq(&connection->lock);

	gb_connection_svc_connection_destroy(connection);
err_hd_cport_disable:
	gb_connection_hd_cport_disable(connection);
err_unlock:
	mutex_unlock(&connection->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(gb_connection_enable);

void gb_connection_disable_rx(struct gb_connection *connection)
{
	mutex_lock(&connection->mutex);

	spin_lock_irq(&connection->lock);
	if (connection->state != GB_CONNECTION_STATE_ENABLED) {
		spin_unlock_irq(&connection->lock);
		goto out_unlock;
	}
	connection->state = GB_CONNECTION_STATE_ENABLED_TX;
	gb_connection_flush_incoming_operations(connection, -ESHUTDOWN);
	connection->handler = NULL;
	spin_unlock_irq(&connection->lock);

out_unlock:
	mutex_unlock(&connection->mutex);
}

void gb_connection_disable(struct gb_connection *connection)
{
	mutex_lock(&connection->mutex);

	if (connection->state == GB_CONNECTION_STATE_DISABLED)
		goto out_unlock;

	gb_connection_control_disconnected(connection);

	spin_lock_irq(&connection->lock);
	connection->state = GB_CONNECTION_STATE_DISABLED;
	gb_connection_cancel_operations(connection, -ESHUTDOWN);
	connection->handler = NULL;
	spin_unlock_irq(&connection->lock);

	gb_connection_svc_connection_destroy(connection);
	gb_connection_hd_cport_disable(connection);

out_unlock:
	mutex_unlock(&connection->mutex);
}
EXPORT_SYMBOL_GPL(gb_connection_disable);

/* Caller must have disabled the connection before destroying it. */
void gb_connection_destroy(struct gb_connection *connection)
{
	struct ida *id_map;

	if (!connection)
		return;

	mutex_lock(&gb_connection_mutex);

	spin_lock_irq(&gb_connections_lock);
	list_del(&connection->bundle_links);
	list_del(&connection->hd_links);
	spin_unlock_irq(&gb_connections_lock);

	destroy_workqueue(connection->wq);

	id_map = &connection->hd->cport_id_map;
	ida_simple_remove(id_map, connection->hd_cport_id);
	connection->hd_cport_id = CPORT_ID_BAD;

	mutex_unlock(&gb_connection_mutex);

	gb_connection_put(connection);
}
EXPORT_SYMBOL_GPL(gb_connection_destroy);

void gb_connection_latency_tag_enable(struct gb_connection *connection)
{
	struct gb_host_device *hd = connection->hd;
	int ret;

	if (!hd->driver->latency_tag_enable)
		return;

	ret = hd->driver->latency_tag_enable(hd, connection->hd_cport_id);
	if (ret) {
		dev_err(&connection->hd->dev,
			"%s: failed to enable latency tag: %d\n",
			connection->name, ret);
	}
}
EXPORT_SYMBOL_GPL(gb_connection_latency_tag_enable);

void gb_connection_latency_tag_disable(struct gb_connection *connection)
{
	struct gb_host_device *hd = connection->hd;
	int ret;

	if (!hd->driver->latency_tag_disable)
		return;

	ret = hd->driver->latency_tag_disable(hd, connection->hd_cport_id);
	if (ret) {
		dev_err(&connection->hd->dev,
			"%s: failed to disable latency tag: %d\n",
			connection->name, ret);
	}
}
EXPORT_SYMBOL_GPL(gb_connection_latency_tag_disable);
