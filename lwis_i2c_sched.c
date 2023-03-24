/*
 * Google LWIS I2C Bus Manager
 *
 * Copyright (c) 2023 Google, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt) KBUILD_MODNAME "-i2c-sched: " fmt

#include "lwis_i2c_sched.h"
#include "lwis_i2c_bus_manager.h"

/*
 * lwis_i2c_process_request_queue_is_empty:
 * Checks if the I2C process request queue is empty
*/
static bool lwis_i2c_process_request_queue_is_empty(struct lwis_i2c_process_queue *process_queue)
{
	if ((!process_queue) || ((process_queue) && (process_queue->number_of_requests == 0))) {
		return true;
	}
	return false;
}

/*
 * lwis_i2c_process_request_queue_initialize:
 * Initializes the I2C process request queue for a given I2C Bus
*/
void lwis_i2c_process_request_queue_initialize(struct lwis_i2c_process_queue *process_queue)
{
	process_queue->number_of_requests = 0;
	INIT_LIST_HEAD(&process_queue->head);
}

/*
 * lwis_i2c_process_request_queue_destroy:
 * Frees all the requests in the queue
*/
void lwis_i2c_process_request_queue_destroy(struct lwis_i2c_process_queue *process_queue)
{
	struct list_head *request;
	struct list_head *request_tmp;
	struct lwis_i2c_process_request *process_request;

	if (!process_queue)
		return;

	if (lwis_i2c_process_request_queue_is_empty(process_queue))
		return;

	list_for_each_safe (request, request_tmp, &process_queue->head) {
		process_request =
			list_entry(request, struct lwis_i2c_process_request, request_node);
		list_del(&process_request->request_node);
		process_request->requesting_device = NULL;
		kfree(process_request);
		process_request = NULL;
		--process_queue->number_of_requests;
	}
}

/*
 * lwis_i2c_process_request_queue_enqueue_request:
 * Enqueues a requesting device on tail of the I2C Scheduler
*/
int lwis_i2c_process_request_queue_enqueue_request(struct lwis_i2c_process_queue *process_queue,
						   struct lwis_device **requesting_device)
{
	int ret = 0;
	struct lwis_i2c_process_request *request;
	struct lwis_device *lwis_dev = *requesting_device;

	if ((!process_queue) || (!requesting_device) || (!lwis_dev)) {
		pr_err("Invalid pointer\n");
		return -EINVAL;
	}

	// Atomic allocation needed here since this memory is allocated within
	// transition and periodic io locks of various I2C devices
	request = kzalloc(sizeof(struct lwis_i2c_process_request), GFP_ATOMIC);
	if (!request) {
		dev_err(lwis_dev->dev, "Failed to allocate I2C Process Request Node memory\n");
		return -ENOMEM;
	}
	request->requesting_device = requesting_device;
	INIT_LIST_HEAD(&request->request_node);
	list_add_tail(&request->request_node, &process_queue->head);
	process_queue->number_of_requests++;

	return ret;
}

/*
 * lwis_i2c_process_request_queue_dequeue_request:
 * Dequeues a lwis device from head of the I2C Scheduler
*/
struct lwis_device **
lwis_i2c_process_request_queue_dequeue_request(struct lwis_i2c_process_queue *process_queue)
{
	struct lwis_i2c_process_request *request;
	struct lwis_device **requested_device = NULL;

	if (lwis_i2c_process_request_queue_is_empty(process_queue)) {
		return requested_device;
	}

	request = list_first_entry_or_null(&process_queue->head, struct lwis_i2c_process_request,
					   request_node);
	if (request) {
		requested_device = request->requesting_device;
		list_del(&request->request_node);
		kfree(request);
		request = NULL;
		process_queue->number_of_requests--;
	}
	return requested_device;
}