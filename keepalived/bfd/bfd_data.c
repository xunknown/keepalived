/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Dynamic data structure definition
 *
 * Author:      Ilya Voronin, <ivoronin@gmail.com>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2015-2017 Alexandre Cassen, <acassen@gmail.com>
 */

#include "config.h"

#include <assert.h>

#include "bfd.h"
#include "bfd_data.h"
#include "logger.h"
#include "parser.h"
#include "memory.h"
#include "utils.h"
#include "main.h"

/* Global vars */
bfd_data_t *bfd_data;
bfd_data_t *old_bfd_data;
char *bfd_buffer;

/*
 * bfd_t functions
 */
/* Initialize bfd_t */
void
alloc_bfd(char *name)
{
	bfd_t *bfd;

	assert(name);

	bfd = (bfd_t *) MALLOC(sizeof (bfd_t));
	strcpy(bfd->iname, name);

	/* Set defaults */
	bfd->local_min_rx_intv = BFD_MINRX_DEFAULT * 1000;
	bfd->local_min_tx_intv = BFD_MINTX_DEFAULT * 1000;
	bfd->local_idle_tx_intv = BFD_IDLETX_DEFAULT * 1000;
	bfd->local_detect_mult = BFD_MULTIPLIER_DEFAULT;

	bfd->ttl = 0;
	bfd->max_hops = 0;

	/* Initialize internal variables */
	bfd->fd_out = -1;
	bfd->thread_out = NULL;
	bfd->thread_exp = NULL;
	bfd->thread_rst = NULL;
	bfd->sands_out = -1;
	bfd->sands_exp = -1;
	bfd->sands_rst = -1;

	list_add(bfd_data->bfd, bfd);
}

static void
free_bfd(void *data)
{
	assert(data);
	FREE(data);
}

/* Dump BFD instance configuration parameters */
static void
dump_bfd(void *data)
{
	bfd_t *bfd;

	assert(data);
	bfd = (bfd_t *)data;

	log_message(LOG_INFO, " BFD Instance = %s", bfd->iname);
	log_message(LOG_INFO, "   Neighbor IP = %s",
		    inet_sockaddrtos(&bfd->nbr_addr));

	if (bfd->src_addr.ss_family)
		log_message(LOG_INFO, "   Source IP = %s",
			    inet_sockaddrtos(&bfd->src_addr));

	log_message(LOG_INFO, "   Required min RX interval = %i ms",
		    bfd->local_min_rx_intv / TIMER_HZ);
	log_message(LOG_INFO, "   Desired min TX interval = %i ms",
		    bfd->local_min_tx_intv / TIMER_HZ);
	log_message(LOG_INFO, "   Desired idle TX interval = %i ms",
		    bfd->local_idle_tx_intv / TIMER_HZ);
	log_message(LOG_INFO, "   Detection multiplier = %i",
		    bfd->local_detect_mult);
}

/* Looks up bfd instance by name */
static bfd_t *
find_bfd_by_name2(char *name, bfd_data_t *data)
{
	element e;
	bfd_t *bfd;

	assert(name);
	assert(data);
	assert(data->bfd);

	if (LIST_ISEMPTY(data->bfd))
		return NULL;

	for (e = LIST_HEAD(data->bfd); e; ELEMENT_NEXT(e)) {
		bfd = ELEMENT_DATA(e);
		if (!strcmp(name, bfd->iname))
			return bfd;
	}

	return NULL;
}

bfd_t *
find_bfd_by_name(char *name)
{
	return find_bfd_by_name2(name, bfd_data);
}

/* compares old and new timers, returns 0 if they are the same */
static int
bfd_cmp_timers(bfd_t * old_bfd, bfd_t * bfd)
{
	return (old_bfd->local_min_rx_intv != bfd->local_min_rx_intv
		|| old_bfd->local_min_tx_intv != bfd->local_min_tx_intv);
}

/*
 * bfd_data_t functions
 */
bfd_data_t *
alloc_bfd_data(void)
{
	bfd_data_t *data;

	data = (bfd_data_t *) MALLOC(sizeof (bfd_data_t));
	data->bfd = alloc_list(free_bfd, dump_bfd);

	/* Initialize internal variables */
	data->thread_in = NULL;
	data->fd_in = -1;

	return data;
}

void
free_bfd_data(bfd_data_t * data)
{
	assert(data);

	free_list(&data->bfd);
	FREE(data);
}

void
dump_bfd_data(bfd_data_t * data)
{
	assert(data);

	if (!LIST_ISEMPTY(data->bfd)) {
		log_message(LOG_INFO, "------< BFD Topology >------");
		dump_list(data->bfd);
	}
}

void
bfd_complete_init(void)
{
	bfd_t *bfd, *bfd_old;
	element e;

	assert(bfd_data);
	assert(bfd_data->bfd);

	/* Build configuration */
	LIST_FOREACH(bfd_data->bfd, bfd, e) {
		/* If there was an old instance with the same name
		   copy its state and thread sands during reload */
		if (reload && (bfd_old = find_bfd_by_name2(bfd->iname, old_bfd_data))) {
			bfd_copy_state(bfd, bfd_old, true);
			bfd_copy_sands(bfd, bfd_old);
			if (bfd_cmp_timers(bfd_old, bfd))
				bfd_set_poll(bfd);
		} else
			bfd_init_state(bfd);
	}

	/* Copy old input fd on reload */
	if (reload)
		bfd_data->fd_in = old_bfd_data->fd_in;
}

/*
 * bfd_buffer functions
 */
void
alloc_bfd_buffer(void)
{
	bfd_buffer = (char *) MALLOC(BFD_BUFFER_SIZE);
}

void
free_bfd_buffer(void)
{
	FREE(bfd_buffer);
}

/*
 * Lookup functions
 */
/* Looks up bfd instance by neighbor address */
bfd_t *
find_bfd_by_addr(struct sockaddr_storage *addr)
{
	element e;
	bfd_t *bfd;
	assert(addr);
	assert(bfd_data);

	if (LIST_ISEMPTY(bfd_data->bfd))
		return NULL;

	for (e = LIST_HEAD(bfd_data->bfd); e; ELEMENT_NEXT(e)) {
		bfd = ELEMENT_DATA(e);
		if (!inet_sockaddrcmp(&bfd->nbr_addr, addr))
			return bfd;
	}

	return NULL;
}

/* Looks up bfd instance by local discriminator */
bfd_t *
find_bfd_by_discr(uint32_t discr)
{
	element e;
	bfd_t *bfd;

	if (LIST_ISEMPTY(bfd_data->bfd))
		return NULL;

	for (e = LIST_HEAD(bfd_data->bfd); e; ELEMENT_NEXT(e)) {
		bfd = ELEMENT_DATA(e);
		if (bfd->local_discr == discr)
			return bfd;
	}

	return NULL;
}

/*
 * Utility functions
 */
/* Generates a random number in the specified interval */
uint32_t
rand_intv(uint32_t min, uint32_t max)
{
	double scaled = (double) rand() / RAND_MAX;
	return (max - min + 1) * scaled + min;
}

/* Returns random disciminator number */
uint32_t
bfd_get_random_discr(bfd_data_t *data)
{
	bfd_t *bfd;
	uint32_t discr;
	element e;

	assert(data);

	do {
		discr = rand_intv(1, UINT32_MAX);

		/* Check for collisions */
		for (e = LIST_HEAD(data->bfd); e; ELEMENT_NEXT(e)) {
			bfd = ELEMENT_DATA(e);
			if (bfd->local_discr == discr) {
				discr = 0;
				break;
			}
		}
	} while (!discr);

	return discr;
}