/*-
 * Copyright (c) 1994,1995 Stefan Esser, Wolfgang StanglMeier
 * Copyright (c) 2000 Michael Smith <msmith@freebsd.org>
 * Copyright (c) 2000 BSDi
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
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
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

/*
 * PCI:PCI bridge support.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pci_private.h>
#include <dev/pci/pcib_private.h>

#include "pcib_if.h"

static int		pcib_probe(device_t dev);
static int		pcib_suspend(device_t dev);
static int		pcib_resume(device_t dev);
static int		pcib_power_for_sleep(device_t pcib, device_t dev,
			    int *pstate);

static device_method_t pcib_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,		pcib_probe),
    DEVMETHOD(device_attach,		pcib_attach),
    DEVMETHOD(device_detach,		bus_generic_detach),
    DEVMETHOD(device_shutdown,		bus_generic_shutdown),
    DEVMETHOD(device_suspend,		pcib_suspend),
    DEVMETHOD(device_resume,		pcib_resume),

    /* Bus interface */
    DEVMETHOD(bus_read_ivar,		pcib_read_ivar),
    DEVMETHOD(bus_write_ivar,		pcib_write_ivar),
    DEVMETHOD(bus_alloc_resource,	pcib_alloc_resource),
#ifdef NEW_PCIB
    DEVMETHOD(bus_adjust_resource,	pcib_adjust_resource),
    DEVMETHOD(bus_release_resource,	pcib_release_resource),
#else
    DEVMETHOD(bus_adjust_resource,	bus_generic_adjust_resource),
    DEVMETHOD(bus_release_resource,	bus_generic_release_resource),
#endif
    DEVMETHOD(bus_activate_resource,	bus_generic_activate_resource),
    DEVMETHOD(bus_deactivate_resource,	bus_generic_deactivate_resource),
    DEVMETHOD(bus_setup_intr,		bus_generic_setup_intr),
    DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),

    /* pcib interface */
    DEVMETHOD(pcib_maxslots,		pcib_maxslots),
    DEVMETHOD(pcib_read_config,		pcib_read_config),
    DEVMETHOD(pcib_write_config,	pcib_write_config),
    DEVMETHOD(pcib_route_interrupt,	pcib_route_interrupt),
    DEVMETHOD(pcib_alloc_msi,		pcib_alloc_msi),
    DEVMETHOD(pcib_release_msi,		pcib_release_msi),
    DEVMETHOD(pcib_alloc_msix,		pcib_alloc_msix),
    DEVMETHOD(pcib_release_msix,	pcib_release_msix),
    DEVMETHOD(pcib_map_msi,		pcib_map_msi),
    DEVMETHOD(pcib_power_for_sleep,	pcib_power_for_sleep),

    DEVMETHOD_END
};

static devclass_t pcib_devclass;

DEFINE_CLASS_0(pcib, pcib_driver, pcib_methods, sizeof(struct pcib_softc));
DRIVER_MODULE(pcib, pci, pcib_driver, pcib_devclass, NULL, NULL);

#ifdef NEW_PCIB
SYSCTL_DECL(_hw_pci);

static int pci_clear_pcib;
TUNABLE_INT("hw.pci.clear_pcib", &pci_clear_pcib);
SYSCTL_INT(_hw_pci, OID_AUTO, clear_pcib, CTLFLAG_RDTUN, &pci_clear_pcib, 0,
    "Clear firmware-assigned resources for PCI-PCI bridge I/O windows.");

/*
 * Is a resource from a child device sub-allocated from one of our
 * resource managers?
 */
static int
pcib_is_resource_managed(struct pcib_softc *sc, int type, struct resource *r)
{

	switch (type) {
#ifdef PCI_RES_BUS
	case PCI_RES_BUS:
		return (rman_is_region_manager(r, &sc->bus.rman));
#endif
	case SYS_RES_IOPORT:
		return (rman_is_region_manager(r, &sc->io.rman));
	case SYS_RES_MEMORY:
		/* Prefetchable resources may live in either memory rman. */
		if (rman_get_flags(r) & RF_PREFETCHABLE &&
		    rman_is_region_manager(r, &sc->pmem.rman))
			return (1);
		return (rman_is_region_manager(r, &sc->mem.rman));
	}
	return (0);
}

static int
pcib_is_window_open(struct pcib_window *pw)
{

	return (pw->valid && pw->base < pw->limit);
}

/*
 * XXX: If RF_ACTIVE did not also imply allocating a bus space tag and
 * handle for the resource, we could pass RF_ACTIVE up to the PCI bus
 * when allocating the resource windows and rely on the PCI bus driver
 * to do this for us.
 */
static void
pcib_activate_window(struct pcib_softc *sc, int type)
{

	PCI_ENABLE_IO(device_get_parent(sc->dev), sc->dev, type);
}

static void
pcib_write_windows(struct pcib_softc *sc, int mask)
{
	device_t dev;
	uint32_t val;

	dev = sc->dev;
	if (sc->io.valid && mask & WIN_IO) {
		val = pci_read_config(dev, PCIR_IOBASEL_1, 1);
		if ((val & PCIM_BRIO_MASK) == PCIM_BRIO_32) {
			pci_write_config(dev, PCIR_IOBASEH_1,
			    sc->io.base >> 16, 2);
			pci_write_config(dev, PCIR_IOLIMITH_1,
			    sc->io.limit >> 16, 2);
		}
		pci_write_config(dev, PCIR_IOBASEL_1, sc->io.base >> 8, 1);
		pci_write_config(dev, PCIR_IOLIMITL_1, sc->io.limit >> 8, 1);
	}

	if (mask & WIN_MEM) {
		pci_write_config(dev, PCIR_MEMBASE_1, sc->mem.base >> 16, 2);
		pci_write_config(dev, PCIR_MEMLIMIT_1, sc->mem.limit >> 16, 2);
	}

	if (sc->pmem.valid && mask & WIN_PMEM) {
		val = pci_read_config(dev, PCIR_PMBASEL_1, 2);
		if ((val & PCIM_BRPM_MASK) == PCIM_BRPM_64) {
			pci_write_config(dev, PCIR_PMBASEH_1,
			    sc->pmem.base >> 32, 4);
			pci_write_config(dev, PCIR_PMLIMITH_1,
			    sc->pmem.limit >> 32, 4);
		}
		pci_write_config(dev, PCIR_PMBASEL_1, sc->pmem.base >> 16, 2);
		pci_write_config(dev, PCIR_PMLIMITL_1, sc->pmem.limit >> 16, 2);
	}
}

/*
 * This is used to reject I/O port allocations that conflict with an
 * ISA alias range.
 */
static int
pcib_is_isa_range(struct pcib_softc *sc, u_long start, u_long end, u_long count)
{
	u_long next_alias;

	if (!(sc->bridgectl & PCIB_BCR_ISA_ENABLE))
		return (0);

	/* Only check fixed ranges for overlap. */
	if (start + count - 1 != end)
		return (0);

	/* ISA aliases are only in the lower 64KB of I/O space. */
	if (start >= 65536)
		return (0);

	/* Check for overlap with 0x000 - 0x0ff as a special case. */
	if (start < 0x100)
		goto alias;

	/*
	 * If the start address is an alias, the range is an alias.
	 * Otherwise, compute the start of the next alias range and
	 * check if it is before the end of the candidate range.
	 */
	if ((start & 0x300) != 0)
		goto alias;
	next_alias = (start & ~0x3fful) | 0x100;
	if (next_alias <= end)
		goto alias;
	return (0);

alias:
	if (bootverbose)
		device_printf(sc->dev,
		    "I/O range %#lx-%#lx overlaps with an ISA alias\n", start,
		    end);
	return (1);
}

static void
pcib_add_window_resources(struct pcib_window *w, struct resource **res,
    int count)
{
	struct resource **newarray;
	int error, i;

	newarray = malloc(sizeof(struct resource *) * (w->count + count),
	    M_DEVBUF, M_WAITOK);
	if (w->res != NULL)
		bcopy(w->res, newarray, sizeof(struct resource *) * w->count);
	bcopy(res, newarray + w->count, sizeof(struct resource *) * count);
	free(w->res, M_DEVBUF);
	w->res = newarray;
	w->count += count;
	
	for (i = 0; i < count; i++) {
		error = rman_manage_region(&w->rman, rman_get_start(res[i]),
		    rman_get_end(res[i]));
		if (error)
			panic("Failed to add resource to rman");
	}
}

typedef void (nonisa_callback)(u_long start, u_long end, void *arg);

static void
pcib_walk_nonisa_ranges(u_long start, u_long end, nonisa_callback *cb,
    void *arg)
{
	u_long next_end;

	/*
	 * If start is within an ISA alias range, move up to the start
	 * of the next non-alias range.  As a special case, addresses
	 * in the range 0x000 - 0x0ff should also be skipped since
	 * those are used for various system I/O devices in ISA
	 * systems.
	 */
	if (start <= 65535) {
		if (start < 0x100 || (start & 0x300) != 0) {
			start &= ~0x3ff;
			start += 0x400;
		}
	}

	/* ISA aliases are only in the lower 64KB of I/O space. */
	while (start <= MIN(end, 65535)) {
		next_end = MIN(start | 0xff, end);
		cb(start, next_end, arg);
		start += 0x400;
	}

	if (start <= end)
		cb(start, end, arg);
}

static void
count_ranges(u_long start, u_long end, void *arg)
{
	int *countp;

	countp = arg;
	(*countp)++;
}

struct alloc_state {
	struct resource **res;
	struct pcib_softc *sc;
	int count, error;
};

static void
alloc_ranges(u_long start, u_long end, void *arg)
{
	struct alloc_state *as;
	struct pcib_window *w;
	int rid;

	as = arg;
	if (as->error != 0)
		return;

	w = &as->sc->io;
	rid = w->reg;
	if (bootverbose)
		device_printf(as->sc->dev,
		    "allocating non-ISA range %#lx-%#lx\n", start, end);
	as->res[as->count] = bus_alloc_resource(as->sc->dev, SYS_RES_IOPORT,
	    &rid, start, end, end - start + 1, 0);
	if (as->res[as->count] == NULL)
		as->error = ENXIO;
	else
		as->count++;
}

static int
pcib_alloc_nonisa_ranges(struct pcib_softc *sc, u_long start, u_long end)
{
	struct alloc_state as;
	int i, new_count;

	/* First, see how many ranges we need. */
	new_count = 0;
	pcib_walk_nonisa_ranges(start, end, count_ranges, &new_count);

	/* Second, allocate the ranges. */
	as.res = malloc(sizeof(struct resource *) * new_count, M_DEVBUF,
	    M_WAITOK);
	as.sc = sc;
	as.count = 0;
	as.error = 0;
	pcib_walk_nonisa_ranges(start, end, alloc_ranges, &as);
	if (as.error != 0) {
		for (i = 0; i < as.count; i++)
			bus_release_resource(sc->dev, SYS_RES_IOPORT,
			    sc->io.reg, as.res[i]);
		free(as.res, M_DEVBUF);
		return (as.error);
	}
	KASSERT(as.count == new_count, ("%s: count mismatch", __func__));

	/* Third, add the ranges to the window. */
	pcib_add_window_resources(&sc->io, as.res, as.count);
	free(as.res, M_DEVBUF);
	return (0);
}

static void
pcib_alloc_window(struct pcib_softc *sc, struct pcib_window *w, int type,
    int flags, pci_addr_t max_address)
{
	struct resource *res;
	char buf[64];
	int error, rid;

	if (max_address != (u_long)max_address)
		max_address = ~0ul;
	w->rman.rm_start = 0;
	w->rman.rm_end = max_address;
	w->rman.rm_type = RMAN_ARRAY;
	snprintf(buf, sizeof(buf), "%s %s window",
	    device_get_nameunit(sc->dev), w->name);
	w->rman.rm_descr = strdup(buf, M_DEVBUF);
	error = rman_init(&w->rman);
	if (error)
		panic("Failed to initialize %s %s rman",
		    device_get_nameunit(sc->dev), w->name);

	if (!pcib_is_window_open(w))
		return;

	if (w->base > max_address || w->limit > max_address) {
		device_printf(sc->dev,
		    "initial %s window has too many bits, ignoring\n", w->name);
		return;
	}
	if (type == SYS_RES_IOPORT && sc->bridgectl & PCIB_BCR_ISA_ENABLE)
		(void)pcib_alloc_nonisa_ranges(sc, w->base, w->limit);
	else {
		rid = w->reg;
		res = bus_alloc_resource(sc->dev, type, &rid, w->base, w->limit,
		    w->limit - w->base + 1, flags);
		if (res != NULL)
			pcib_add_window_resources(w, &res, 1);
	}
	if (w->res == NULL) {
		device_printf(sc->dev,
		    "failed to allocate initial %s window: %#jx-%#jx\n",
		    w->name, (uintmax_t)w->base, (uintmax_t)w->limit);
		w->base = max_address;
		w->limit = 0;
		pcib_write_windows(sc, w->mask);
		return;
	}
	pcib_activate_window(sc, type);
}

/*
 * Initialize I/O windows.
 */
static void
pcib_probe_windows(struct pcib_softc *sc)
{
	pci_addr_t max;
	device_t dev;
	uint32_t val;

	dev = sc->dev;

	if (pci_clear_pcib) {
		pci_write_config(dev, PCIR_IOBASEL_1, 0xff, 1);
		pci_write_config(dev, PCIR_IOBASEH_1, 0xffff, 2);
		pci_write_config(dev, PCIR_IOLIMITL_1, 0, 1);
		pci_write_config(dev, PCIR_IOLIMITH_1, 0, 2);
		pci_write_config(dev, PCIR_MEMBASE_1, 0xffff, 2);
		pci_write_config(dev, PCIR_MEMLIMIT_1, 0, 2);
		pci_write_config(dev, PCIR_PMBASEL_1, 0xffff, 2);
		pci_write_config(dev, PCIR_PMBASEH_1, 0xffffffff, 4);
		pci_write_config(dev, PCIR_PMLIMITL_1, 0, 2);
		pci_write_config(dev, PCIR_PMLIMITH_1, 0, 4);
	}

	/* Determine if the I/O port window is implemented. */
	val = pci_read_config(dev, PCIR_IOBASEL_1, 1);
	if (val == 0) {
		/*
		 * If 'val' is zero, then only 16-bits of I/O space
		 * are supported.
		 */
		pci_write_config(dev, PCIR_IOBASEL_1, 0xff, 1);
		if (pci_read_config(dev, PCIR_IOBASEL_1, 1) != 0) {
			sc->io.valid = 1;
			pci_write_config(dev, PCIR_IOBASEL_1, 0, 1);
		}
	} else
		sc->io.valid = 1;

	/* Read the existing I/O port window. */
	if (sc->io.valid) {
		sc->io.reg = PCIR_IOBASEL_1;
		sc->io.step = 12;
		sc->io.mask = WIN_IO;
		sc->io.name = "I/O port";
		if ((val & PCIM_BRIO_MASK) == PCIM_BRIO_32) {
			sc->io.base = PCI_PPBIOBASE(
			    pci_read_config(dev, PCIR_IOBASEH_1, 2), val);
			sc->io.limit = PCI_PPBIOLIMIT(
			    pci_read_config(dev, PCIR_IOLIMITH_1, 2),
			    pci_read_config(dev, PCIR_IOLIMITL_1, 1));
			max = 0xffffffff;
		} else {
			sc->io.base = PCI_PPBIOBASE(0, val);
			sc->io.limit = PCI_PPBIOLIMIT(0,
			    pci_read_config(dev, PCIR_IOLIMITL_1, 1));
			max = 0xffff;
		}
		pcib_alloc_window(sc, &sc->io, SYS_RES_IOPORT, 0, max);
	}

	/* Read the existing memory window. */
	sc->mem.valid = 1;
	sc->mem.reg = PCIR_MEMBASE_1;
	sc->mem.step = 20;
	sc->mem.mask = WIN_MEM;
	sc->mem.name = "memory";
	sc->mem.base = PCI_PPBMEMBASE(0,
	    pci_read_config(dev, PCIR_MEMBASE_1, 2));
	sc->mem.limit = PCI_PPBMEMLIMIT(0,
	    pci_read_config(dev, PCIR_MEMLIMIT_1, 2));
	pcib_alloc_window(sc, &sc->mem, SYS_RES_MEMORY, 0, 0xffffffff);

	/* Determine if the prefetchable memory window is implemented. */
	val = pci_read_config(dev, PCIR_PMBASEL_1, 2);
	if (val == 0) {
		/*
		 * If 'val' is zero, then only 32-bits of memory space
		 * are supported.
		 */
		pci_write_config(dev, PCIR_PMBASEL_1, 0xffff, 2);
		if (pci_read_config(dev, PCIR_PMBASEL_1, 2) != 0) {
			sc->pmem.valid = 1;
			pci_write_config(dev, PCIR_PMBASEL_1, 0, 2);
		}
	} else
		sc->pmem.valid = 1;

	/* Read the existing prefetchable memory window. */
	if (sc->pmem.valid) {
		sc->pmem.reg = PCIR_PMBASEL_1;
		sc->pmem.step = 20;
		sc->pmem.mask = WIN_PMEM;
		sc->pmem.name = "prefetch";
		if ((val & PCIM_BRPM_MASK) == PCIM_BRPM_64) {
			sc->pmem.base = PCI_PPBMEMBASE(
			    pci_read_config(dev, PCIR_PMBASEH_1, 4), val);
			sc->pmem.limit = PCI_PPBMEMLIMIT(
			    pci_read_config(dev, PCIR_PMLIMITH_1, 4),
			    pci_read_config(dev, PCIR_PMLIMITL_1, 2));
			max = 0xffffffffffffffff;
		} else {
			sc->pmem.base = PCI_PPBMEMBASE(0, val);
			sc->pmem.limit = PCI_PPBMEMLIMIT(0,
			    pci_read_config(dev, PCIR_PMLIMITL_1, 2));
			max = 0xffffffff;
		}
		pcib_alloc_window(sc, &sc->pmem, SYS_RES_MEMORY,
		    RF_PREFETCHABLE, max);
	}
}

#ifdef PCI_RES_BUS
/*
 * Allocate a suitable secondary bus for this bridge if needed and
 * initialize the resource manager for the secondary bus range.  Note
 * that the minimum count is a desired value and this may allocate a
 * smaller range.
 */
void
pcib_setup_secbus(device_t dev, struct pcib_secbus *bus, int min_count)
{
	char buf[64];
	int error, rid;

	switch (pci_read_config(dev, PCIR_HDRTYPE, 1) & PCIM_HDRTYPE) {
	case PCIM_HDRTYPE_BRIDGE:
		bus->sub_reg = PCIR_SUBBUS_1;
		break;
	case PCIM_HDRTYPE_CARDBUS:
		bus->sub_reg = PCIR_SUBBUS_2;
		break;
	default:
		panic("not a PCI bridge");
	}
	bus->dev = dev;
	bus->rman.rm_start = 0;
	bus->rman.rm_end = PCI_BUSMAX;
	bus->rman.rm_type = RMAN_ARRAY;
	snprintf(buf, sizeof(buf), "%s bus numbers", device_get_nameunit(dev));
	bus->rman.rm_descr = strdup(buf, M_DEVBUF);
	error = rman_init(&bus->rman);
	if (error)
		panic("Failed to initialize %s bus number rman",
		    device_get_nameunit(dev));

	/*
	 * Allocate a bus range.  This will return an existing bus range
	 * if one exists, or a new bus range if one does not.
	 */
	rid = 0;
	bus->res = bus_alloc_resource(dev, PCI_RES_BUS, &rid, 0ul, ~0ul,
	    min_count, 0);
	if (bus->res == NULL) {
		/*
		 * Fall back to just allocating a range of a single bus
		 * number.
		 */
		bus->res = bus_alloc_resource(dev, PCI_RES_BUS, &rid, 0ul, ~0ul,
		    1, 0);
	} else if (rman_get_size(bus->res) < min_count)
		/*
		 * Attempt to grow the existing range to satisfy the
		 * minimum desired count.
		 */
		(void)bus_adjust_resource(dev, PCI_RES_BUS, bus->res,
		    rman_get_start(bus->res), rman_get_start(bus->res) +
		    min_count - 1);

	/*
	 * Add the initial resource to the rman.
	 */
	if (bus->res != NULL) {
		error = rman_manage_region(&bus->rman, rman_get_start(bus->res),
		    rman_get_end(bus->res));
		if (error)
			panic("Failed to add resource to rman");
		bus->sec = rman_get_start(bus->res);
		bus->sub = rman_get_end(bus->res);
	}
}

static struct resource *
pcib_suballoc_bus(struct pcib_secbus *bus, device_t child, int *rid,
    u_long start, u_long end, u_long count, u_int flags)
{
	struct resource *res;

	res = rman_reserve_resource(&bus->rman, start, end, count, flags,
	    child);
	if (res == NULL)
		return (NULL);

	if (bootverbose)
		device_printf(bus->dev,
		    "allocated bus range (%lu-%lu) for rid %d of %s\n",
		    rman_get_start(res), rman_get_end(res), *rid,
		    pcib_child_name(child));
	rman_set_rid(res, *rid);
	return (res);
}

/*
 * Attempt to grow the secondary bus range.  This is much simpler than
 * for I/O windows as the range can only be grown by increasing
 * subbus.
 */
static int
pcib_grow_subbus(struct pcib_secbus *bus, u_long new_end)
{
	u_long old_end;
	int error;

	old_end = rman_get_end(bus->res);
	KASSERT(new_end > old_end, ("attempt to shrink subbus"));
	error = bus_adjust_resource(bus->dev, PCI_RES_BUS, bus->res,
	    rman_get_start(bus->res), new_end);
	if (error)
		return (error);
	if (bootverbose)
		device_printf(bus->dev, "grew bus range to %lu-%lu\n",
		    rman_get_start(bus->res), rman_get_end(bus->res));
	error = rman_manage_region(&bus->rman, old_end + 1,
	    rman_get_end(bus->res));
	if (error)
		panic("Failed to add resource to rman");
	bus->sub = rman_get_end(bus->res);
	pci_write_config(bus->dev, bus->sub_reg, bus->sub, 1);
	return (0);
}

struct resource *
pcib_alloc_subbus(struct pcib_secbus *bus, device_t child, int *rid,
    u_long start, u_long end, u_long count, u_int flags)
{
	struct resource *res;
	u_long start_free, end_free, new_end;

	/*
	 * First, see if the request can be satisified by the existing
	 * bus range.
	 */
	res = pcib_suballoc_bus(bus, child, rid, start, end, count, flags);
	if (res != NULL)
		return (res);

	/*
	 * Figure out a range to grow the bus range.  First, find the
	 * first bus number after the last allocated bus in the rman and
	 * enforce that as a minimum starting point for the range.
	 */
	if (rman_last_free_region(&bus->rman, &start_free, &end_free) != 0 ||
	    end_free != bus->sub)
		start_free = bus->sub + 1;
	if (start_free < start)
		start_free = start;
	new_end = start_free + count - 1;

	/*
	 * See if this new range would satisfy the request if it
	 * succeeds.
	 */
	if (new_end > end)
		return (NULL);

	/* Finally, attempt to grow the existing resource. */
	if (bootverbose) {
		device_printf(bus->dev,
		    "attempting to grow bus range for %lu buses\n", count);
		printf("\tback candidate range: %lu-%lu\n", start_free,
		    new_end);
	}
	if (pcib_grow_subbus(bus, new_end) == 0)
		return (pcib_suballoc_bus(bus, child, rid, start, end, count,
		    flags));
	return (NULL);
}
#endif

#else

/*
 * Is the prefetch window open (eg, can we allocate memory in it?)
 */
static int
pcib_is_prefetch_open(struct pcib_softc *sc)
{
	return (sc->pmembase > 0 && sc->pmembase < sc->pmemlimit);
}

/*
 * Is the nonprefetch window open (eg, can we allocate memory in it?)
 */
static int
pcib_is_nonprefetch_open(struct pcib_softc *sc)
{
	return (sc->membase > 0 && sc->membase < sc->memlimit);
}

/*
 * Is the io window open (eg, can we allocate ports in it?)
 */
static int
pcib_is_io_open(struct pcib_softc *sc)
{
	return (sc->iobase > 0 && sc->iobase < sc->iolimit);
}

/*
 * Get current I/O decode.
 */
static void
pcib_get_io_decode(struct pcib_softc *sc)
{
	device_t	dev;
	uint32_t	iolow;

	dev = sc->dev;

	iolow = pci_read_config(dev, PCIR_IOBASEL_1, 1);
	if ((iolow & PCIM_BRIO_MASK) == PCIM_BRIO_32)
		sc->iobase = PCI_PPBIOBASE(
		    pci_read_config(dev, PCIR_IOBASEH_1, 2), iolow);
	else
		sc->iobase = PCI_PPBIOBASE(0, iolow);

	iolow = pci_read_config(dev, PCIR_IOLIMITL_1, 1);
	if ((iolow & PCIM_BRIO_MASK) == PCIM_BRIO_32)
		sc->iolimit = PCI_PPBIOLIMIT(
		    pci_read_config(dev, PCIR_IOLIMITH_1, 2), iolow);
	else
		sc->iolimit = PCI_PPBIOLIMIT(0, iolow);
}

/*
 * Get current memory decode.
 */
static void
pcib_get_mem_decode(struct pcib_softc *sc)
{
	device_t	dev;
	pci_addr_t	pmemlow;

	dev = sc->dev;

	sc->membase = PCI_PPBMEMBASE(0,
	    pci_read_config(dev, PCIR_MEMBASE_1, 2));
	sc->memlimit = PCI_PPBMEMLIMIT(0,
	    pci_read_config(dev, PCIR_MEMLIMIT_1, 2));

	pmemlow = pci_read_config(dev, PCIR_PMBASEL_1, 2);
	if ((pmemlow & PCIM_BRPM_MASK) == PCIM_BRPM_64)
		sc->pmembase = PCI_PPBMEMBASE(
		    pci_read_config(dev, PCIR_PMBASEH_1, 4), pmemlow);
	else
		sc->pmembase = PCI_PPBMEMBASE(0, pmemlow);

	pmemlow = pci_read_config(dev, PCIR_PMLIMITL_1, 2);
	if ((pmemlow & PCIM_BRPM_MASK) == PCIM_BRPM_64)	
		sc->pmemlimit = PCI_PPBMEMLIMIT(
		    pci_read_config(dev, PCIR_PMLIMITH_1, 4), pmemlow);
	else
		sc->pmemlimit = PCI_PPBMEMLIMIT(0, pmemlow);
}

/*
 * Restore previous I/O decode.
 */
static void
pcib_set_io_decode(struct pcib_softc *sc)
{
	device_t	dev;
	uint32_t	iohi;

	dev = sc->dev;

	iohi = sc->iobase >> 16;
	if (iohi > 0)
		pci_write_config(dev, PCIR_IOBASEH_1, iohi, 2);
	pci_write_config(dev, PCIR_IOBASEL_1, sc->iobase >> 8, 1);

	iohi = sc->iolimit >> 16;
	if (iohi > 0)
		pci_write_config(dev, PCIR_IOLIMITH_1, iohi, 2);
	pci_write_config(dev, PCIR_IOLIMITL_1, sc->iolimit >> 8, 1);
}

/*
 * Restore previous memory decode.
 */
static void
pcib_set_mem_decode(struct pcib_softc *sc)
{
	device_t	dev;
	pci_addr_t	pmemhi;

	dev = sc->dev;

	pci_write_config(dev, PCIR_MEMBASE_1, sc->membase >> 16, 2);
	pci_write_config(dev, PCIR_MEMLIMIT_1, sc->memlimit >> 16, 2);

	pmemhi = sc->pmembase >> 32;
	if (pmemhi > 0)
		pci_write_config(dev, PCIR_PMBASEH_1, pmemhi, 4);
	pci_write_config(dev, PCIR_PMBASEL_1, sc->pmembase >> 16, 2);

	pmemhi = sc->pmemlimit >> 32;
	if (pmemhi > 0)
		pci_write_config(dev, PCIR_PMLIMITH_1, pmemhi, 4);
	pci_write_config(dev, PCIR_PMLIMITL_1, sc->pmemlimit >> 16, 2);
}
#endif

/*
 * Get current bridge configuration.
 */
static void
pcib_cfg_save(struct pcib_softc *sc)
{
	device_t	dev;

	dev = sc->dev;

	sc->command = pci_read_config(dev, PCIR_COMMAND, 2);
	sc->pribus = pci_read_config(dev, PCIR_PRIBUS_1, 1);
	sc->bus.sec = pci_read_config(dev, PCIR_SECBUS_1, 1);
	sc->bus.sub = pci_read_config(dev, PCIR_SUBBUS_1, 1);
	sc->bridgectl = pci_read_config(dev, PCIR_BRIDGECTL_1, 2);
	sc->seclat = pci_read_config(dev, PCIR_SECLAT_1, 1);
#ifndef NEW_PCIB
	if (sc->command & PCIM_CMD_PORTEN)
		pcib_get_io_decode(sc);
	if (sc->command & PCIM_CMD_MEMEN)
		pcib_get_mem_decode(sc);
#endif
}

/*
 * Restore previous bridge configuration.
 */
static void
pcib_cfg_restore(struct pcib_softc *sc)
{
	device_t	dev;

	dev = sc->dev;

	pci_write_config(dev, PCIR_COMMAND, sc->command, 2);
	pci_write_config(dev, PCIR_PRIBUS_1, sc->pribus, 1);
	pci_write_config(dev, PCIR_SECBUS_1, sc->bus.sec, 1);
	pci_write_config(dev, PCIR_SUBBUS_1, sc->bus.sub, 1);
	pci_write_config(dev, PCIR_BRIDGECTL_1, sc->bridgectl, 2);
	pci_write_config(dev, PCIR_SECLAT_1, sc->seclat, 1);
#ifdef NEW_PCIB
	pcib_write_windows(sc, WIN_IO | WIN_MEM | WIN_PMEM);
#else
	if (sc->command & PCIM_CMD_PORTEN)
		pcib_set_io_decode(sc);
	if (sc->command & PCIM_CMD_MEMEN)
		pcib_set_mem_decode(sc);
#endif
}

/*
 * Generic device interface
 */
static int
pcib_probe(device_t dev)
{
    if ((pci_get_class(dev) == PCIC_BRIDGE) &&
	(pci_get_subclass(dev) == PCIS_BRIDGE_PCI)) {
	device_set_desc(dev, "PCI-PCI bridge");
	return(-10000);
    }
    return(ENXIO);
}

void
pcib_attach_common(device_t dev)
{
    struct pcib_softc	*sc;
    struct sysctl_ctx_list *sctx;
    struct sysctl_oid	*soid;
    int comma;

    sc = device_get_softc(dev);
    sc->dev = dev;

    /*
     * Get current bridge configuration.
     */
    sc->domain = pci_get_domain(dev);
    sc->secstat = pci_read_config(dev, PCIR_SECSTAT_1, 2);
    pcib_cfg_save(sc);

    /*
     * The primary bus register should always be the bus of the
     * parent.
     */
    sc->pribus = pci_get_bus(dev);
    pci_write_config(dev, PCIR_PRIBUS_1, sc->pribus, 1);

    /*
     * Setup sysctl reporting nodes
     */
    sctx = device_get_sysctl_ctx(dev);
    soid = device_get_sysctl_tree(dev);
    SYSCTL_ADD_UINT(sctx, SYSCTL_CHILDREN(soid), OID_AUTO, "domain",
      CTLFLAG_RD, &sc->domain, 0, "Domain number");
    SYSCTL_ADD_UINT(sctx, SYSCTL_CHILDREN(soid), OID_AUTO, "pribus",
      CTLFLAG_RD, &sc->pribus, 0, "Primary bus number");
    SYSCTL_ADD_UINT(sctx, SYSCTL_CHILDREN(soid), OID_AUTO, "secbus",
      CTLFLAG_RD, &sc->bus.sec, 0, "Secondary bus number");
    SYSCTL_ADD_UINT(sctx, SYSCTL_CHILDREN(soid), OID_AUTO, "subbus",
      CTLFLAG_RD, &sc->bus.sub, 0, "Subordinate bus number");

    /*
     * Quirk handling.
     */
    switch (pci_get_devid(dev)) {
#if !defined(NEW_PCIB) && !defined(PCI_RES_BUS)
    case 0x12258086:		/* Intel 82454KX/GX (Orion) */
	{
	    uint8_t	supbus;

	    supbus = pci_read_config(dev, 0x41, 1);
	    if (supbus != 0xff) {
		sc->bus.sec = supbus + 1;
		sc->bus.sub = supbus + 1;
	    }
	    break;
	}
#endif

    /*
     * The i82380FB mobile docking controller is a PCI-PCI bridge,
     * and it is a subtractive bridge.  However, the ProgIf is wrong
     * so the normal setting of PCIB_SUBTRACTIVE bit doesn't
     * happen.  There's also a Toshiba bridge that behaves this
     * way.
     */
    case 0x124b8086:		/* Intel 82380FB Mobile */
    case 0x060513d7:		/* Toshiba ???? */
	sc->flags |= PCIB_SUBTRACTIVE;
	break;

#if !defined(NEW_PCIB) && !defined(PCI_RES_BUS)
    /* Compaq R3000 BIOS sets wrong subordinate bus number. */
    case 0x00dd10de:
	{
	    char *cp;

	    if ((cp = getenv("smbios.planar.maker")) == NULL)
		break;
	    if (strncmp(cp, "Compal", 6) != 0) {
		freeenv(cp);
		break;
	    }
	    freeenv(cp);
	    if ((cp = getenv("smbios.planar.product")) == NULL)
		break;
	    if (strncmp(cp, "08A0", 4) != 0) {
		freeenv(cp);
		break;
	    }
	    freeenv(cp);
	    if (sc->bus.sub < 0xa) {
		pci_write_config(dev, PCIR_SUBBUS_1, 0xa, 1);
		sc->bus.sub = pci_read_config(dev, PCIR_SUBBUS_1, 1);
	    }
	    break;
	}
#endif
    }

    if (pci_msi_device_blacklisted(dev))
	sc->flags |= PCIB_DISABLE_MSI;

    if (pci_msix_device_blacklisted(dev))
	sc->flags |= PCIB_DISABLE_MSIX;

    /*
     * Intel 815, 845 and other chipsets say they are PCI-PCI bridges,
     * but have a ProgIF of 0x80.  The 82801 family (AA, AB, BAM/CAM,
     * BA/CA/DB and E) PCI bridges are HUB-PCI bridges, in Intelese.
     * This means they act as if they were subtractively decoding
     * bridges and pass all transactions.  Mark them and real ProgIf 1
     * parts as subtractive.
     */
    if ((pci_get_devid(dev) & 0xff00ffff) == 0x24008086 ||
      pci_read_config(dev, PCIR_PROGIF, 1) == PCIP_BRIDGE_PCI_SUBTRACTIVE)
	sc->flags |= PCIB_SUBTRACTIVE;

#ifdef NEW_PCIB
#ifdef PCI_RES_BUS
    pcib_setup_secbus(dev, &sc->bus, 1);
#endif
    pcib_probe_windows(sc);
#endif
    if (bootverbose) {
	device_printf(dev, "  domain            %d\n", sc->domain);
	device_printf(dev, "  secondary bus     %d\n", sc->bus.sec);
	device_printf(dev, "  subordinate bus   %d\n", sc->bus.sub);
#ifdef NEW_PCIB
	if (pcib_is_window_open(&sc->io))
	    device_printf(dev, "  I/O decode        0x%jx-0x%jx\n",
	      (uintmax_t)sc->io.base, (uintmax_t)sc->io.limit);
	if (pcib_is_window_open(&sc->mem))
	    device_printf(dev, "  memory decode     0x%jx-0x%jx\n",
	      (uintmax_t)sc->mem.base, (uintmax_t)sc->mem.limit);
	if (pcib_is_window_open(&sc->pmem))
	    device_printf(dev, "  prefetched decode 0x%jx-0x%jx\n",
	      (uintmax_t)sc->pmem.base, (uintmax_t)sc->pmem.limit);
#else
	if (pcib_is_io_open(sc))
	    device_printf(dev, "  I/O decode        0x%x-0x%x\n",
	      sc->iobase, sc->iolimit);
	if (pcib_is_nonprefetch_open(sc))
	    device_printf(dev, "  memory decode     0x%jx-0x%jx\n",
	      (uintmax_t)sc->membase, (uintmax_t)sc->memlimit);
	if (pcib_is_prefetch_open(sc))
	    device_printf(dev, "  prefetched decode 0x%jx-0x%jx\n",
	      (uintmax_t)sc->pmembase, (uintmax_t)sc->pmemlimit);
#endif
	if (sc->bridgectl & (PCIB_BCR_ISA_ENABLE | PCIB_BCR_VGA_ENABLE) ||
	    sc->flags & PCIB_SUBTRACTIVE) {
		device_printf(dev, "  special decode    ");
		comma = 0;
		if (sc->bridgectl & PCIB_BCR_ISA_ENABLE) {
			printf("ISA");
			comma = 1;
		}
		if (sc->bridgectl & PCIB_BCR_VGA_ENABLE) {
			printf("%sVGA", comma ? ", " : "");
			comma = 1;
		}
		if (sc->flags & PCIB_SUBTRACTIVE)
			printf("%ssubtractive", comma ? ", " : "");
		printf("\n");
	}
    }

    /*
     * Always enable busmastering on bridges so that transactions
     * initiated on the secondary bus are passed through to the
     * primary bus.
     */
    pci_enable_busmaster(dev);
}

int
pcib_attach(device_t dev)
{
    struct pcib_softc	*sc;
    device_t		child;

    pcib_attach_common(dev);
    sc = device_get_softc(dev);
    if (sc->bus.sec != 0) {
	child = device_add_child(dev, "pci", sc->bus.sec);
	if (child != NULL)
	    return(bus_generic_attach(dev));
    }

    /* no secondary bus; we should have fixed this */
    return(0);
}

int
pcib_suspend(device_t dev)
{
	device_t	pcib;
	int		dstate, error;

	pcib_cfg_save(device_get_softc(dev));
	error = bus_generic_suspend(dev);
	if (error == 0 && pci_do_power_suspend) {
		dstate = PCI_POWERSTATE_D3;
		pcib = device_get_parent(device_get_parent(dev));
		if (PCIB_POWER_FOR_SLEEP(pcib, dev, &dstate) == 0)
			pci_set_powerstate(dev, dstate);
	}
	return (error);
}

int
pcib_resume(device_t dev)
{
	device_t	pcib;

	if (pci_do_power_resume) {
		pcib = device_get_parent(device_get_parent(dev));
		if (PCIB_POWER_FOR_SLEEP(pcib, dev, NULL) == 0)
			pci_set_powerstate(dev, PCI_POWERSTATE_D0);
	}
	pcib_cfg_restore(device_get_softc(dev));
	return (bus_generic_resume(dev));
}

int
pcib_read_ivar(device_t dev, device_t child, int which, uintptr_t *result)
{
    struct pcib_softc	*sc = device_get_softc(dev);
    
    switch (which) {
    case PCIB_IVAR_DOMAIN:
	*result = sc->domain;
	return(0);
    case PCIB_IVAR_BUS:
	*result = sc->bus.sec;
	return(0);
    }
    return(ENOENT);
}

int
pcib_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{

    switch (which) {
    case PCIB_IVAR_DOMAIN:
	return(EINVAL);
    case PCIB_IVAR_BUS:
	return(EINVAL);
    }
    return(ENOENT);
}

#ifdef NEW_PCIB
/*
 * Attempt to allocate a resource from the existing resources assigned
 * to a window.
 */
static struct resource *
pcib_suballoc_resource(struct pcib_softc *sc, struct pcib_window *w,
    device_t child, int type, int *rid, u_long start, u_long end, u_long count,
    u_int flags)
{
	struct resource *res;

	if (!pcib_is_window_open(w))
		return (NULL);

	res = rman_reserve_resource(&w->rman, start, end, count,
	    flags & ~RF_ACTIVE, child);
	if (res == NULL)
		return (NULL);

	if (bootverbose)
		device_printf(sc->dev,
		    "allocated %s range (%#lx-%#lx) for rid %x of %s\n",
		    w->name, rman_get_start(res), rman_get_end(res), *rid,
		    pcib_child_name(child));
	rman_set_rid(res, *rid);

	/*
	 * If the resource should be active, pass that request up the
	 * tree.  This assumes the parent drivers can handle
	 * activating sub-allocated resources.
	 */
	if (flags & RF_ACTIVE) {
		if (bus_activate_resource(child, type, *rid, res) != 0) {
			rman_release_resource(res);
			return (NULL);
		}
	}

	return (res);
}

/* Allocate a fresh resource range for an unconfigured window. */
static int
pcib_alloc_new_window(struct pcib_softc *sc, struct pcib_window *w, int type,
    u_long start, u_long end, u_long count, u_int flags)
{
	struct resource *res;
	u_long base, limit, wmask;
	int rid;

	/*
	 * If this is an I/O window on a bridge with ISA enable set
	 * and the start address is below 64k, then try to allocate an
	 * initial window of 0x1000 bytes long starting at address
	 * 0xf000 and walking down.  Note that if the original request
	 * was larger than the non-aliased range size of 0x100 our
	 * caller would have raised the start address up to 64k
	 * already.
	 */
	if (type == SYS_RES_IOPORT && sc->bridgectl & PCIB_BCR_ISA_ENABLE &&
	    start < 65536) {
		for (base = 0xf000; (long)base >= 0; base -= 0x1000) {
			limit = base + 0xfff;

			/*
			 * Skip ranges that wouldn't work for the
			 * original request.  Note that the actual
			 * window that overlaps are the non-alias
			 * ranges within [base, limit], so this isn't
			 * quite a simple comparison.
			 */
			if (start + count > limit - 0x400)
				continue;
			if (base == 0) {
				/*
				 * The first open region for the window at
				 * 0 is 0x400-0x4ff.
				 */
				if (end - count + 1 < 0x400)
					continue;
			} else {
				if (end - count + 1 < base)
					continue;
			}

			if (pcib_alloc_nonisa_ranges(sc, base, limit) == 0) {
				w->base = base;
				w->limit = limit;
				return (0);
			}
		}
		return (ENOSPC);		
	}
	
	wmask = (1ul << w->step) - 1;
	if (RF_ALIGNMENT(flags) < w->step) {
		flags &= ~RF_ALIGNMENT_MASK;
		flags |= RF_ALIGNMENT_LOG2(w->step);
	}
	start &= ~wmask;
	end |= wmask;
	count = roundup2(count, 1ul << w->step);
	rid = w->reg;
	res = bus_alloc_resource(sc->dev, type, &rid, start, end, count,
	    flags & ~RF_ACTIVE);
	if (res == NULL)
		return (ENOSPC);
	pcib_add_window_resources(w, &res, 1);
	pcib_activate_window(sc, type);
	w->base = rman_get_start(res);
	w->limit = rman_get_end(res);
	return (0);
}

/* Try to expand an existing window to the requested base and limit. */
static int
pcib_expand_window(struct pcib_softc *sc, struct pcib_window *w, int type,
    u_long base, u_long limit)
{
	struct resource *res;
	int error, i, force_64k_base;

	KASSERT(base <= w->base && limit >= w->limit,
	    ("attempting to shrink window"));

	/*
	 * XXX: pcib_grow_window() doesn't try to do this anyway and
	 * the error handling for all the edge cases would be tedious.
	 */
	KASSERT(limit == w->limit || base == w->base,
	    ("attempting to grow both ends of a window"));

	/*
	 * Yet more special handling for requests to expand an I/O
	 * window behind an ISA-enabled bridge.  Since I/O windows
	 * have to grow in 0x1000 increments and the end of the 0xffff
	 * range is an alias, growing a window below 64k will always
	 * result in allocating new resources and never adjusting an
	 * existing resource.
	 */
	if (type == SYS_RES_IOPORT && sc->bridgectl & PCIB_BCR_ISA_ENABLE &&
	    (limit <= 65535 || (base <= 65535 && base != w->base))) {
		KASSERT(limit == w->limit || limit <= 65535,
		    ("attempting to grow both ends across 64k ISA alias"));

		if (base != w->base)
			error = pcib_alloc_nonisa_ranges(sc, base, w->base - 1);
		else
			error = pcib_alloc_nonisa_ranges(sc, w->limit + 1,
			    limit);
		if (error == 0) {
			w->base = base;
			w->limit = limit;
		}
		return (error);
	}

	/*
	 * Find the existing resource to adjust.  Usually there is only one,
	 * but for an ISA-enabled bridge we might be growing the I/O window
	 * above 64k and need to find the existing resource that maps all
	 * of the area above 64k.
	 */
	for (i = 0; i < w->count; i++) {
		if (rman_get_end(w->res[i]) == w->limit)
			break;
	}
	KASSERT(i != w->count, ("did not find existing resource"));
	res = w->res[i];

	/*
	 * Usually the resource we found should match the window's
	 * existing range.  The one exception is the ISA-enabled case
	 * mentioned above in which case the resource should start at
	 * 64k.
	 */
	if (type == SYS_RES_IOPORT && sc->bridgectl & PCIB_BCR_ISA_ENABLE &&
	    w->base <= 65535) {
		KASSERT(rman_get_start(res) == 65536,
		    ("existing resource mismatch"));
		force_64k_base = 1;
	} else {
		KASSERT(w->base == rman_get_start(res),
		    ("existing resource mismatch"));
		force_64k_base = 0;
	}	

	error = bus_adjust_resource(sc->dev, type, res, force_64k_base ?
	    rman_get_start(res) : base, limit);
	if (error)
		return (error);

	/* Add the newly allocated region to the resource manager. */
	if (w->base != base) {
		error = rman_manage_region(&w->rman, base, w->base - 1);
		w->base = base;
	} else {
		error = rman_manage_region(&w->rman, w->limit + 1, limit);
		w->limit = limit;
	}
	if (error) {
		if (bootverbose)
			device_printf(sc->dev,
			    "failed to expand %s resource manager\n", w->name);
		(void)bus_adjust_resource(sc->dev, type, res, force_64k_base ?
		    rman_get_start(res) : w->base, w->limit);
	}
	return (error);
}

/*
 * Attempt to grow a window to make room for a given resource request.
 */
static int
pcib_grow_window(struct pcib_softc *sc, struct pcib_window *w, int type,
    u_long start, u_long end, u_long count, u_int flags)
{
	u_long align, start_free, end_free, front, back, wmask;
	int error;

	/*
	 * Clamp the desired resource range to the maximum address
	 * this window supports.  Reject impossible requests.
	 *
	 * For I/O port requests behind a bridge with the ISA enable
	 * bit set, force large allocations to start above 64k.
	 */
	if (!w->valid)
		return (EINVAL);
	if (sc->bridgectl & PCIB_BCR_ISA_ENABLE && count > 0x100 &&
	    start < 65536)
		start = 65536;
	if (end > w->rman.rm_end)
		end = w->rman.rm_end;
	if (start + count - 1 > end || start + count < start)
		return (EINVAL);
	wmask = (1ul << w->step) - 1;

	/*
	 * If there is no resource at all, just try to allocate enough
	 * aligned space for this resource.
	 */
	if (w->res == NULL) {
		error = pcib_alloc_new_window(sc, w, type, start, end, count,
		    flags);
		if (error) {
			if (bootverbose)
				device_printf(sc->dev,
		    "failed to allocate initial %s window (%#lx-%#lx,%#lx)\n",
				    w->name, start, end, count);
			return (error);
		}
		if (bootverbose)
			device_printf(sc->dev,
			    "allocated initial %s window of %#jx-%#jx\n",
			    w->name, (uintmax_t)w->base, (uintmax_t)w->limit);
		goto updatewin;
	}

	/*
	 * See if growing the window would help.  Compute the minimum
	 * amount of address space needed on both the front and back
	 * ends of the existing window to satisfy the allocation.
	 *
	 * For each end, build a candidate region adjusting for the
	 * required alignment, etc.  If there is a free region at the
	 * edge of the window, grow from the inner edge of the free
	 * region.  Otherwise grow from the window boundary.
	 *
	 * Growing an I/O window below 64k for a bridge with the ISA
	 * enable bit doesn't require any special magic as the step
	 * size of an I/O window (1k) always includes multiple
	 * non-alias ranges when it is grown in either direction.
	 *
	 * XXX: Special case: if w->res is completely empty and the
	 * request size is larger than w->res, we should find the
	 * optimal aligned buffer containing w->res and allocate that.
	 */
	if (bootverbose)
		device_printf(sc->dev,
		    "attempting to grow %s window for (%#lx-%#lx,%#lx)\n",
		    w->name, start, end, count);
	align = 1ul << RF_ALIGNMENT(flags);
	if (start < w->base) {
		if (rman_first_free_region(&w->rman, &start_free, &end_free) !=
		    0 || start_free != w->base)
			end_free = w->base;
		if (end_free > end)
			end_free = end + 1;

		/* Move end_free down until it is properly aligned. */
		end_free &= ~(align - 1);
		end_free--;
		front = end_free - (count - 1);

		/*
		 * The resource would now be allocated at (front,
		 * end_free).  Ensure that fits in the (start, end)
		 * bounds.  end_free is checked above.  If 'front' is
		 * ok, ensure it is properly aligned for this window.
		 * Also check for underflow.
		 */
		if (front >= start && front <= end_free) {
			if (bootverbose)
				printf("\tfront candidate range: %#lx-%#lx\n",
				    front, end_free);
			front &= ~wmask;
			front = w->base - front;
		} else
			front = 0;
	} else
		front = 0;
	if (end > w->limit) {
		if (rman_last_free_region(&w->rman, &start_free, &end_free) !=
		    0 || end_free != w->limit)
			start_free = w->limit + 1;
		if (start_free < start)
			start_free = start;

		/* Move start_free up until it is properly aligned. */
		start_free = roundup2(start_free, align);
		back = start_free + count - 1;

		/*
		 * The resource would now be allocated at (start_free,
		 * back).  Ensure that fits in the (start, end)
		 * bounds.  start_free is checked above.  If 'back' is
		 * ok, ensure it is properly aligned for this window.
		 * Also check for overflow.
		 */
		if (back <= end && start_free <= back) {
			if (bootverbose)
				printf("\tback candidate range: %#lx-%#lx\n",
				    start_free, back);
			back |= wmask;
			back -= w->limit;
		} else
			back = 0;
	} else
		back = 0;

	/*
	 * Try to allocate the smallest needed region first.
	 * If that fails, fall back to the other region.
	 */
	error = ENOSPC;
	while (front != 0 || back != 0) {
		if (front != 0 && (front <= back || back == 0)) {
			error = pcib_expand_window(sc, w, type, w->base - front,
			    w->limit);
			if (error == 0)
				break;
			front = 0;
		} else {
			error = pcib_expand_window(sc, w, type, w->base,
			    w->limit + back);
			if (error == 0)
				break;
			back = 0;
		}
	}

	if (error)
		return (error);
	if (bootverbose)
		device_printf(sc->dev, "grew %s window to %#jx-%#jx\n",
		    w->name, (uintmax_t)w->base, (uintmax_t)w->limit);

updatewin:
	/* Write the new window. */
	KASSERT((w->base & wmask) == 0, ("start address is not aligned"));
	KASSERT((w->limit & wmask) == wmask, ("end address is not aligned"));
	pcib_write_windows(sc, w->mask);
	return (0);
}

/*
 * We have to trap resource allocation requests and ensure that the bridge
 * is set up to, or capable of handling them.
 */
struct resource *
pcib_alloc_resource(device_t dev, device_t child, int type, int *rid,
    u_long start, u_long end, u_long count, u_int flags)
{
	struct pcib_softc *sc;
	struct resource *r;

	sc = device_get_softc(dev);

	/*
	 * VGA resources are decoded iff the VGA enable bit is set in
	 * the bridge control register.  VGA resources do not fall into
	 * the resource windows and are passed up to the parent.
	 */
	if ((type == SYS_RES_IOPORT && pci_is_vga_ioport_range(start, end)) ||
	    (type == SYS_RES_MEMORY && pci_is_vga_memory_range(start, end))) {
		if (sc->bridgectl & PCIB_BCR_VGA_ENABLE)
			return (bus_generic_alloc_resource(dev, child, type,
			    rid, start, end, count, flags));
		else
			return (NULL);
	}

	switch (type) {
#ifdef PCI_RES_BUS
	case PCI_RES_BUS:
		return (pcib_alloc_subbus(&sc->bus, child, rid, start, end,
		    count, flags));
#endif
	case SYS_RES_IOPORT:
		if (pcib_is_isa_range(sc, start, end, count))
			return (NULL);
		r = pcib_suballoc_resource(sc, &sc->io, child, type, rid, start,
		    end, count, flags);
		if (r != NULL || (sc->flags & PCIB_SUBTRACTIVE) != 0)
			break;
		if (pcib_grow_window(sc, &sc->io, type, start, end, count,
		    flags) == 0)
			r = pcib_suballoc_resource(sc, &sc->io, child, type,
			    rid, start, end, count, flags);
		break;
	case SYS_RES_MEMORY:
		/*
		 * For prefetchable resources, prefer the prefetchable
		 * memory window, but fall back to the regular memory
		 * window if that fails.  Try both windows before
		 * attempting to grow a window in case the firmware
		 * has used a range in the regular memory window to
		 * map a prefetchable BAR.
		 */
		if (flags & RF_PREFETCHABLE) {
			r = pcib_suballoc_resource(sc, &sc->pmem, child, type,
			    rid, start, end, count, flags);
			if (r != NULL)
				break;
		}
		r = pcib_suballoc_resource(sc, &sc->mem, child, type, rid,
		    start, end, count, flags);
		if (r != NULL || (sc->flags & PCIB_SUBTRACTIVE) != 0)
			break;
		if (flags & RF_PREFETCHABLE) {
			if (pcib_grow_window(sc, &sc->pmem, type, start, end,
			    count, flags) == 0) {
				r = pcib_suballoc_resource(sc, &sc->pmem, child,
				    type, rid, start, end, count, flags);
				if (r != NULL)
					break;
			}
		}
		if (pcib_grow_window(sc, &sc->mem, type, start, end, count,
		    flags & ~RF_PREFETCHABLE) == 0)
			r = pcib_suballoc_resource(sc, &sc->mem, child, type,
			    rid, start, end, count, flags);
		break;
	default:
		return (bus_generic_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));
	}

	/*
	 * If attempts to suballocate from the window fail but this is a
	 * subtractive bridge, pass the request up the tree.
	 */
	if (sc->flags & PCIB_SUBTRACTIVE && r == NULL)
		return (bus_generic_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));
	return (r);
}

int
pcib_adjust_resource(device_t bus, device_t child, int type, struct resource *r,
    u_long start, u_long end)
{
	struct pcib_softc *sc;

	sc = device_get_softc(bus);
	if (pcib_is_resource_managed(sc, type, r))
		return (rman_adjust_resource(r, start, end));
	return (bus_generic_adjust_resource(bus, child, type, r, start, end));
}

int
pcib_release_resource(device_t dev, device_t child, int type, int rid,
    struct resource *r)
{
	struct pcib_softc *sc;
	int error;

	sc = device_get_softc(dev);
	if (pcib_is_resource_managed(sc, type, r)) {
		if (rman_get_flags(r) & RF_ACTIVE) {
			error = bus_deactivate_resource(child, type, rid, r);
			if (error)
				return (error);
		}
		return (rman_release_resource(r));
	}
	return (bus_generic_release_resource(dev, child, type, rid, r));
}
#else
/*
 * We have to trap resource allocation requests and ensure that the bridge
 * is set up to, or capable of handling them.
 */
struct resource *
pcib_alloc_resource(device_t dev, device_t child, int type, int *rid, 
    u_long start, u_long end, u_long count, u_int flags)
{
	struct pcib_softc	*sc = device_get_softc(dev);
	const char *name, *suffix;
	int ok;

	/*
	 * Fail the allocation for this range if it's not supported.
	 */
	name = device_get_nameunit(child);
	if (name == NULL) {
		name = "";
		suffix = "";
	} else
		suffix = " ";
	switch (type) {
	case SYS_RES_IOPORT:
		ok = 0;
		if (!pcib_is_io_open(sc))
			break;
		ok = (start >= sc->iobase && end <= sc->iolimit);

		/*
		 * Make sure we allow access to VGA I/O addresses when the
		 * bridge has the "VGA Enable" bit set.
		 */
		if (!ok && pci_is_vga_ioport_range(start, end))
			ok = (sc->bridgectl & PCIB_BCR_VGA_ENABLE) ? 1 : 0;

		if ((sc->flags & PCIB_SUBTRACTIVE) == 0) {
			if (!ok) {
				if (start < sc->iobase)
					start = sc->iobase;
				if (end > sc->iolimit)
					end = sc->iolimit;
				if (start < end)
					ok = 1;
			}
		} else {
			ok = 1;
#if 0
			/*
			 * If we overlap with the subtractive range, then
			 * pick the upper range to use.
			 */
			if (start < sc->iolimit && end > sc->iobase)
				start = sc->iolimit + 1;
#endif
		}
		if (end < start) {
			device_printf(dev, "ioport: end (%lx) < start (%lx)\n",
			    end, start);
			start = 0;
			end = 0;
			ok = 0;
		}
		if (!ok) {
			device_printf(dev, "%s%srequested unsupported I/O "
			    "range 0x%lx-0x%lx (decoding 0x%x-0x%x)\n",
			    name, suffix, start, end, sc->iobase, sc->iolimit);
			return (NULL);
		}
		if (bootverbose)
			device_printf(dev,
			    "%s%srequested I/O range 0x%lx-0x%lx: in range\n",
			    name, suffix, start, end);
		break;

	case SYS_RES_MEMORY:
		ok = 0;
		if (pcib_is_nonprefetch_open(sc))
			ok = ok || (start >= sc->membase && end <= sc->memlimit);
		if (pcib_is_prefetch_open(sc))
			ok = ok || (start >= sc->pmembase && end <= sc->pmemlimit);

		/*
		 * Make sure we allow access to VGA memory addresses when the
		 * bridge has the "VGA Enable" bit set.
		 */
		if (!ok && pci_is_vga_memory_range(start, end))
			ok = (sc->bridgectl & PCIB_BCR_VGA_ENABLE) ? 1 : 0;

		if ((sc->flags & PCIB_SUBTRACTIVE) == 0) {
			if (!ok) {
				ok = 1;
				if (flags & RF_PREFETCHABLE) {
					if (pcib_is_prefetch_open(sc)) {
						if (start < sc->pmembase)
							start = sc->pmembase;
						if (end > sc->pmemlimit)
							end = sc->pmemlimit;
					} else {
						ok = 0;
					}
				} else {	/* non-prefetchable */
					if (pcib_is_nonprefetch_open(sc)) {
						if (start < sc->membase)
							start = sc->membase;
						if (end > sc->memlimit)
							end = sc->memlimit;
					} else {
						ok = 0;
					}
				}
			}
		} else if (!ok) {
			ok = 1;	/* subtractive bridge: always ok */
#if 0
			if (pcib_is_nonprefetch_open(sc)) {
				if (start < sc->memlimit && end > sc->membase)
					start = sc->memlimit + 1;
			}
			if (pcib_is_prefetch_open(sc)) {
				if (start < sc->pmemlimit && end > sc->pmembase)
					start = sc->pmemlimit + 1;
			}
#endif
		}
		if (end < start) {
			device_printf(dev, "memory: end (%lx) < start (%lx)\n",
			    end, start);
			start = 0;
			end = 0;
			ok = 0;
		}
		if (!ok && bootverbose)
			device_printf(dev,
			    "%s%srequested unsupported memory range %#lx-%#lx "
			    "(decoding %#jx-%#jx, %#jx-%#jx)\n",
			    name, suffix, start, end,
			    (uintmax_t)sc->membase, (uintmax_t)sc->memlimit,
			    (uintmax_t)sc->pmembase, (uintmax_t)sc->pmemlimit);
		if (!ok)
			return (NULL);
		if (bootverbose)
			device_printf(dev,"%s%srequested memory range "
			    "0x%lx-0x%lx: good\n",
			    name, suffix, start, end);
		break;

	default:
		break;
	}
	/*
	 * Bridge is OK decoding this resource, so pass it up.
	 */
	return (bus_generic_alloc_resource(dev, child, type, rid, start, end,
	    count, flags));
}
#endif

/*
 * PCIB interface.
 */
int
pcib_maxslots(device_t dev)
{
    return(PCI_SLOTMAX);
}

/*
 * Since we are a child of a PCI bus, its parent must support the pcib interface.
 */
uint32_t
pcib_read_config(device_t dev, u_int b, u_int s, u_int f, u_int reg, int width)
{
    return(PCIB_READ_CONFIG(device_get_parent(device_get_parent(dev)), b, s, f, reg, width));
}

void
pcib_write_config(device_t dev, u_int b, u_int s, u_int f, u_int reg, uint32_t val, int width)
{
    PCIB_WRITE_CONFIG(device_get_parent(device_get_parent(dev)), b, s, f, reg, val, width);
}

/*
 * Route an interrupt across a PCI bridge.
 */
int
pcib_route_interrupt(device_t pcib, device_t dev, int pin)
{
    device_t	bus;
    int		parent_intpin;
    int		intnum;

    /*	
     *
     * The PCI standard defines a swizzle of the child-side device/intpin to
     * the parent-side intpin as follows.
     *
     * device = device on child bus
     * child_intpin = intpin on child bus slot (0-3)
     * parent_intpin = intpin on parent bus slot (0-3)
     *
     * parent_intpin = (device + child_intpin) % 4
     */
    parent_intpin = (pci_get_slot(dev) + (pin - 1)) % 4;

    /*
     * Our parent is a PCI bus.  Its parent must export the pcib interface
     * which includes the ability to route interrupts.
     */
    bus = device_get_parent(pcib);
    intnum = PCIB_ROUTE_INTERRUPT(device_get_parent(bus), pcib, parent_intpin + 1);
    if (PCI_INTERRUPT_VALID(intnum) && bootverbose) {
	device_printf(pcib, "slot %d INT%c is routed to irq %d\n",
	    pci_get_slot(dev), 'A' + pin - 1, intnum);
    }
    return(intnum);
}

/* Pass request to alloc MSI/MSI-X messages up to the parent bridge. */
int
pcib_alloc_msi(device_t pcib, device_t dev, int count, int maxcount, int *irqs)
{
	struct pcib_softc *sc = device_get_softc(pcib);
	device_t bus;

	if (sc->flags & PCIB_DISABLE_MSI)
		return (ENXIO);
	bus = device_get_parent(pcib);
	return (PCIB_ALLOC_MSI(device_get_parent(bus), dev, count, maxcount,
	    irqs));
}

/* Pass request to release MSI/MSI-X messages up to the parent bridge. */
int
pcib_release_msi(device_t pcib, device_t dev, int count, int *irqs)
{
	device_t bus;

	bus = device_get_parent(pcib);
	return (PCIB_RELEASE_MSI(device_get_parent(bus), dev, count, irqs));
}

/* Pass request to alloc an MSI-X message up to the parent bridge. */
int
pcib_alloc_msix(device_t pcib, device_t dev, int *irq)
{
	struct pcib_softc *sc = device_get_softc(pcib);
	device_t bus;

	if (sc->flags & PCIB_DISABLE_MSIX)
		return (ENXIO);
	bus = device_get_parent(pcib);
	return (PCIB_ALLOC_MSIX(device_get_parent(bus), dev, irq));
}

/* Pass request to release an MSI-X message up to the parent bridge. */
int
pcib_release_msix(device_t pcib, device_t dev, int irq)
{
	device_t bus;

	bus = device_get_parent(pcib);
	return (PCIB_RELEASE_MSIX(device_get_parent(bus), dev, irq));
}

/* Pass request to map MSI/MSI-X message up to parent bridge. */
int
pcib_map_msi(device_t pcib, device_t dev, int irq, uint64_t *addr,
    uint32_t *data)
{
	device_t bus;
	int error;

	bus = device_get_parent(pcib);
	error = PCIB_MAP_MSI(device_get_parent(bus), dev, irq, addr, data);
	if (error)
		return (error);

	pci_ht_map_msi(pcib, *addr);
	return (0);
}

/* Pass request for device power state up to parent bridge. */
int
pcib_power_for_sleep(device_t pcib, device_t dev, int *pstate)
{
	device_t bus;

	bus = device_get_parent(pcib);
	return (PCIB_POWER_FOR_SLEEP(bus, dev, pstate));
}
