/*-
 * Copyright (c) 2005 Nate Lawson
 * Copyright (c) 2000 Mitsuru IWASAKI <iwasaki@jp.freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_acpi.h"
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/ioccom.h>
#include <sys/sysctl.h>

#include "acpi.h"
#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

/* Default seconds before re-sampling the battery state. */
#define	ACPI_BATTERY_INFO_EXPIRE	5

static int	acpi_batteries_initted;
static int	acpi_battery_info_expire = ACPI_BATTERY_INFO_EXPIRE;
static struct	acpi_battinfo	acpi_battery_battinfo;
static struct	sysctl_ctx_list	acpi_battery_sysctl_ctx;
static struct	sysctl_oid	*acpi_battery_sysctl_tree;

ACPI_SERIAL_DECL(battery, "ACPI generic battery");

static void acpi_reset_battinfo(struct acpi_battinfo *info);
static int acpi_battery_ioctl(u_long cmd, caddr_t addr, void *arg);
static int acpi_battery_sysctl(SYSCTL_HANDLER_ARGS);
static int acpi_battery_units_sysctl(SYSCTL_HANDLER_ARGS);
static int acpi_battery_init(void);

int
acpi_battery_register(device_t dev)
{
    int error;

    error = 0;
    ACPI_SERIAL_BEGIN(battery);
    if (!acpi_batteries_initted)
	error = acpi_battery_init();
    ACPI_SERIAL_END(battery);
    return (error);
}

int
acpi_battery_remove(device_t dev)
{

    return (0);
}

int
acpi_battery_get_units(void)
{
    devclass_t batt_dc;

    batt_dc = devclass_find("battery");
    if (batt_dc == NULL)
	return (0);
    return (devclass_get_count(batt_dc));
}

int
acpi_battery_get_info_expire(void)
{

    return (acpi_battery_info_expire);
}

/* Check _BST results for validity. */
int
acpi_battery_bst_valid(struct acpi_bst *bst)
{
    if (bst->state >= ACPI_BATT_STAT_MAX || bst->cap == 0xffffffff ||
	bst->volt == 0xffffffff)
	return (FALSE);
    else
	return (TRUE);
}

/* Check _BIF results for validity. */
int
acpi_battery_bif_valid(struct acpi_bif *bif)
{
    if (bif->lfcap == 0)
	return (FALSE);
    else
	return (TRUE);
}

/* Get info about one or all batteries. */
int
acpi_battery_get_battinfo(device_t dev, struct acpi_battinfo *battinfo)
{
    int	batt_stat, devcount, dev_idx, error, i;
    int total_cap, total_min, valid_rate, valid_units;
    devclass_t batt_dc;
    device_t batt_dev;
    struct acpi_bst *bst;
    struct acpi_bif *bif;
    struct acpi_battinfo *bi;

    /*
     * Get the battery devclass and number of devices.  If there are none
     * or error, return immediately.
     */
    batt_dc = devclass_find("battery");
    if (batt_dc == NULL)
	return (ENXIO);
    devcount = devclass_get_count(batt_dc);
    if (devcount == 0)
	return (ENXIO);

    /*
     * Allocate storage for all _BST data, their derived battinfo data,
     * and the current battery's _BIF data.
     */
    bst = malloc(devcount * sizeof(*bst), M_TEMP, M_WAITOK);
    bi = malloc(devcount * sizeof(*bi), M_TEMP, M_WAITOK);
    bif = malloc(sizeof(*bif), M_TEMP, M_WAITOK);

    /*
     * Pass 1:  for each battery that is present and valid, get its status,
     * calculate percent capacity remaining, and sum all the current
     * discharge rates.
     */
    dev_idx = -1;
    batt_stat = valid_rate = valid_units = 0;
    for (i = 0; i < devcount; i++) {
	/* Find the device.  If it disappeared, the user can try again. */
	batt_dev = devclass_get_device(batt_dc, i);
	if (batt_dev == NULL) {
	    error = ENOMEM;
	    goto out;
	}

	/* Default info for every battery is "not present". */
	acpi_reset_battinfo(&bi[i]);

	/* If examining a specific battery and this is it, record its index. */
	if (dev != NULL && dev == batt_dev)
	    dev_idx = i;

	/* Be sure we can get various info from the battery. */
	if (!acpi_BatteryIsPresent(batt_dev) ||
	    ACPI_BATT_GET_STATUS(batt_dev, &bst[i]) != 0 ||
	    ACPI_BATT_GET_INFO(batt_dev, bif) != 0)
	    continue;

	/* If a battery is not installed, we sometimes get strange values. */
	if (!acpi_battery_bst_valid(&bst[i]) ||
	    !acpi_battery_bif_valid(bif))
	    continue;

	/* Record state and calculate percent capacity remaining. */
	valid_units++;
	batt_stat |= bst[i].state;
	bi[i].state = bst[i].state;
	bi[i].cap = 100 * bst[i].cap / bif->lfcap;

	/*
	 * Some laptops report the "design-capacity" instead of the
	 * "real-capacity" when the battery is fully charged.  That breaks
	 * the above arithmetic as it needs to be 100% maximum.
	 */
	if (bi[i].cap > 100)
	    bi[i].cap = 100;

	/*
	 * On systems with more than one battery, they may get used
	 * sequentially, thus bst.rate may only signify the one currently
	 * in use.  For the remaining batteries, bst.rate will be zero,
	 * which makes it impossible to calculate the total remaining time.
	 * Therefore, we sum the bst.rate for batteries in the discharging
	 * state and use the sum to calculate the total remaining time.
	 */
	if (bst[i].rate > 0 && (bst[i].state & ACPI_BATT_STAT_DISCHARG))
	    valid_rate += bst[i].rate;
    }

    /* If the caller asked for a device but we didn't find it, error. */
    if (dev != NULL && dev_idx < 0) {
	error = ENXIO;
	goto out;
    }

    /* Pass 2:  calculate capacity and remaining time for all batteries. */
    total_cap = total_min = 0;
    for (i = 0; i < devcount; i++) {
	/*
	 * If any batteries are discharging, use the sum of the bst.rate
	 * values.  Otherwise, we are on AC power, and there is infinite
	 * time remaining for this battery until we go offline.
	 */
	if (valid_rate > 0)
	    bi[i].min = 60 * bst[i].cap / valid_rate;
	else
	    bi[i].min = 0;
	total_min += bi[i].min;
	total_cap += bi[i].cap;
    }

    /*
     * Return total battery percent and time remaining.  If there are
     * no valid batteries, report values as unknown.
     */
    if (valid_units > 0) {
	if (dev == NULL) {
	    battinfo->cap = total_cap / valid_units;
	    battinfo->min = total_min;
	    battinfo->state = batt_stat;
	    battinfo->rate = valid_rate;
	} else {
	    battinfo->cap = bi[dev_idx].cap;
	    battinfo->min = bi[dev_idx].min;
	    battinfo->state = bi[dev_idx].state;
	    battinfo->rate = bst[dev_idx].rate;
	}

	/*
	 * If the queried battery has no discharge rate or is charging,
	 * report that we don't know the remaining time.
	 */
	if (valid_rate == 0 || (battinfo->state & ACPI_BATT_STAT_CHARGING))
	    battinfo->min = -1;
    } else
	acpi_reset_battinfo(battinfo);

    error = 0;

out:
    if (bi)
	free(bi, M_TEMP);
    if (bif)
	free(bif, M_TEMP);
    if (bst)
	free(bst, M_TEMP);
    return (error);
}

static void
acpi_reset_battinfo(struct acpi_battinfo *info)
{
    info->cap = -1;
    info->min = -1;
    info->state = ACPI_BATT_STAT_NOT_PRESENT;
    info->rate = -1;
}

static int
acpi_battery_ioctl(u_long cmd, caddr_t addr, void *arg)
{
    union acpi_battery_ioctl_arg *ioctl_arg;
    int error, unit;
    device_t dev;

    error = ENXIO;
    ioctl_arg = (union acpi_battery_ioctl_arg *)addr;
    unit = ioctl_arg->unit;
    if (unit != ACPI_BATTERY_ALL_UNITS)
	dev = devclass_get_device(devclass_find("battery"), unit);
    else
	dev = NULL;

    /*
     * No security check required: information retrieval only.  If
     * new functions are added here, a check might be required.
     */
    switch (cmd) {
    case ACPIIO_BATT_GET_UNITS:
	*(int *)addr = acpi_battery_get_units();
	break;
    case ACPIIO_BATT_GET_BATTINFO:
	if (dev != NULL || unit == ACPI_BATTERY_ALL_UNITS)
	    error = acpi_battery_get_battinfo(dev, &ioctl_arg->battinfo);
	break;
    case ACPIIO_BATT_GET_BIF:
	if (dev != NULL)
	    error = ACPI_BATT_GET_INFO(dev, &ioctl_arg->bif);
	break;
    case ACPIIO_BATT_GET_BST:
	if (dev != NULL)
	    error = ACPI_BATT_GET_STATUS(dev, &ioctl_arg->bst);
	break;
    default:
	error = EINVAL;
    }

    return (error);
}

static int
acpi_battery_sysctl(SYSCTL_HANDLER_ARGS)
{
    int val, error;

    acpi_battery_get_battinfo(NULL, &acpi_battery_battinfo);
    val = *(u_int *)oidp->oid_arg1;
    error = sysctl_handle_int(oidp, &val, 0, req);
    return (error);
}

static int
acpi_battery_units_sysctl(SYSCTL_HANDLER_ARGS)
{
    int count, error;

    count = acpi_battery_get_units();
    error = sysctl_handle_int(oidp, &count, 0, req);
    return (error);
}

static int
acpi_battery_init(void)
{
    struct acpi_softc	*sc;
    device_t		 dev;
    int	 		 error;

    ACPI_SERIAL_ASSERT(battery);

    error = ENXIO;
    dev = devclass_get_device(devclass_find("acpi"), 0);
    if (dev == NULL)
	goto out;
    sc = device_get_softc(dev);

    error = acpi_register_ioctl(ACPIIO_BATT_GET_UNITS, acpi_battery_ioctl,
	NULL);
    if (error != 0)
	goto out;
    error = acpi_register_ioctl(ACPIIO_BATT_GET_BATTINFO, acpi_battery_ioctl,
	NULL);
    if (error != 0)
	goto out;
    error = acpi_register_ioctl(ACPIIO_BATT_GET_BIF, acpi_battery_ioctl, NULL);
    if (error != 0)
	goto out;
    error = acpi_register_ioctl(ACPIIO_BATT_GET_BST, acpi_battery_ioctl, NULL);
    if (error != 0)
	goto out;

    sysctl_ctx_init(&acpi_battery_sysctl_ctx);
    acpi_battery_sysctl_tree = SYSCTL_ADD_NODE(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(sc->acpi_sysctl_tree), OID_AUTO, "battery", CTLFLAG_RD,
	0, "");
    SYSCTL_ADD_PROC(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(acpi_battery_sysctl_tree),
	OID_AUTO, "life", CTLTYPE_INT | CTLFLAG_RD,
	&acpi_battery_battinfo.cap, 0, acpi_battery_sysctl, "I", "");
    SYSCTL_ADD_PROC(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(acpi_battery_sysctl_tree),
	OID_AUTO, "time", CTLTYPE_INT | CTLFLAG_RD,
	&acpi_battery_battinfo.min, 0, acpi_battery_sysctl, "I", "");
    SYSCTL_ADD_PROC(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(acpi_battery_sysctl_tree),
	OID_AUTO, "state", CTLTYPE_INT | CTLFLAG_RD,
	&acpi_battery_battinfo.state, 0, acpi_battery_sysctl, "I", "");
    SYSCTL_ADD_PROC(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(acpi_battery_sysctl_tree),
	OID_AUTO, "units", CTLTYPE_INT | CTLFLAG_RD,
	NULL, 0, acpi_battery_units_sysctl, "I", "");
    SYSCTL_ADD_INT(&acpi_battery_sysctl_ctx,
	SYSCTL_CHILDREN(acpi_battery_sysctl_tree),
	OID_AUTO, "info_expire", CTLFLAG_RD | CTLFLAG_RW,
	&acpi_battery_info_expire, 0, "");

    acpi_batteries_initted = TRUE;

out:
    if (error != 0) {
	acpi_deregister_ioctl(ACPIIO_BATT_GET_UNITS, acpi_battery_ioctl);
	acpi_deregister_ioctl(ACPIIO_BATT_GET_BATTINFO, acpi_battery_ioctl);
	acpi_deregister_ioctl(ACPIIO_BATT_GET_BIF, acpi_battery_ioctl);
	acpi_deregister_ioctl(ACPIIO_BATT_GET_BST, acpi_battery_ioctl);
    }
    return (error);
}
