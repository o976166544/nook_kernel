/*
 * Copyright (C) 2005-2007 by Texas Instruments
 * Some code has been taken from tusb6010.c
 * Copyrights for that are attributable to:
 * Copyright (C) 2006 Nokia Corporation
 * Tony Lindgren <tony@atomide.com>
 *
 * This file is part of the Inventra Controller Driver for Linux.
 *
 * The Inventra Controller Driver for Linux is free software; you
 * can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * The Inventra Controller Driver for Linux is distributed in
 * the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with The Inventra Controller Driver for Linux ; if not,
 * write to the Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <asm/mach-types.h>
#include <mach/hardware.h>
#include <plat/mux.h>
#include <plat/omap-pm.h>

#include "musb_core.h"
#include "omap2430.h"

#ifdef CONFIG_ARCH_OMAP3430
#define	get_cpu_rev()	2
#endif


static struct timer_list musb_idle_timer;

void musb_link_save_context(struct otg_transceiver *xceiv);
void musb_link_restore_context(struct otg_transceiver *xceiv);
void musb_link_force_active(int on);

static void musb_do_idle(unsigned long _musb)
{
	struct musb	*musb = (void *)_musb;
	unsigned long	flags;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	u8	power;
#endif
	u8	devctl;

	spin_lock_irqsave(&musb->lock, flags);

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	switch (musb->xceiv->state) {
	case OTG_STATE_A_WAIT_BCON:
		/* Don't reset the DEVCTL_SESSION in forced host mode */
		if (!musb->xceiv->default_a) {
			devctl &= ~MUSB_DEVCTL_SESSION;
			musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

			devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		}
		if (devctl & MUSB_DEVCTL_BDEVICE) {
			musb->xceiv->state = OTG_STATE_B_IDLE;
			MUSB_DEV_MODE(musb);
		} else {
			musb->xceiv->state = OTG_STATE_A_IDLE;
			MUSB_HST_MODE(musb);
		}
		break;
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_SUSPEND:
		/* finish RESUME signaling? */
		if (musb->port1_status & MUSB_PORT_STAT_RESUME) {
			power = musb_readb(musb->mregs, MUSB_POWER);
			power &= ~MUSB_POWER_RESUME;
			DBG(1, "root port resume stopped, power %02x\n", power);
			musb_writeb(musb->mregs, MUSB_POWER, power);
			musb->is_active = 1;
			musb->port1_status &= ~(USB_PORT_STAT_SUSPEND
						| MUSB_PORT_STAT_RESUME);
			musb->port1_status |= USB_PORT_STAT_C_SUSPEND << 16;
			usb_hcd_poll_rh_status(musb_to_hcd(musb));
			/* NOTE: it might really be A_WAIT_BCON ... */
			musb->xceiv->state = OTG_STATE_A_HOST;
		}
		break;
#endif
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case OTG_STATE_A_HOST:
		devctl = musb_readb(musb->mregs, MUSB_DEVCTL);
		if (devctl &  MUSB_DEVCTL_BDEVICE)
			musb->xceiv->state = OTG_STATE_B_IDLE;
		else
			musb->xceiv->state = OTG_STATE_A_WAIT_BCON;
#endif
	default:
		break;
	}
	spin_unlock_irqrestore(&musb->lock, flags);
}


void musb_platform_try_idle(struct musb *musb, unsigned long timeout)
{
	unsigned long		default_timeout = jiffies + msecs_to_jiffies(3);
	static unsigned long	last_timer;

	if (timeout == 0)
		timeout = default_timeout;

	/* Never idle if active, or when VBUS timeout is not set as host */
	if (musb->is_active || ((musb->a_wait_bcon == 0)
			&& (musb->xceiv->state == OTG_STATE_A_WAIT_BCON))) {
		DBG(4, "%s active, deleting timer\n", otg_state_string(musb));
		del_timer(&musb_idle_timer);
		last_timer = jiffies;
		return;
	}

	if (time_after(last_timer, timeout)) {
		if (!timer_pending(&musb_idle_timer))
			last_timer = timeout;
		else {
			DBG(4, "Longer idle timer already pending, ignoring\n");
			return;
		}
	}
	last_timer = timeout;

	DBG(4, "%s inactive, for idle timer for %lu ms\n",
		otg_state_string(musb),
		(unsigned long)jiffies_to_msecs(timeout - jiffies));
	mod_timer(&musb_idle_timer, timeout);
}

void musb_platform_enable(struct musb *musb)
{
}
void musb_platform_disable(struct musb *musb)
{
}
static void omap_vbus_power(struct musb *musb, int is_on, int sleeping)
{
}

void twl4030_kick(int on);
static void omap_set_vbus(struct musb *musb, int is_on)
{
	u8		devctl;
	/* HDRC controls CPEN, but beware current surges during device
	 * connect.  They can trigger transient overcurrent conditions
	 * that must be ignored.
	 */

	devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	if (is_on) {
		musb->is_active = 1;
		musb->xceiv->default_a = 1;
		musb->xceiv->state = OTG_STATE_A_WAIT_VRISE;
		devctl |= MUSB_DEVCTL_SESSION;

		MUSB_HST_MODE(musb);
	} else {
		musb->is_active = 0;

		/* NOTE:  we're skipping A_WAIT_VFALL -> A_IDLE and
		 * jumping right to B_IDLE...
		 */

		musb->xceiv->default_a = 0;
		musb->xceiv->state = OTG_STATE_B_IDLE;
		devctl &= ~MUSB_DEVCTL_SESSION;

		MUSB_DEV_MODE(musb);
	}
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);

	twl4030_kick(is_on);

	DBG(1, "VBUS %s, devctl %02x "
		/* otg %3x conf %08x prcm %08x */ "\n",
		otg_state_string(musb),
		musb_readb(musb->mregs, MUSB_DEVCTL));
}

static int musb_platform_resume(struct musb *musb);

int musb_platform_set_mode(struct musb *musb, u8 musb_mode)
{
	if (musb->board_mode != MUSB_OTG) {
		ERR("Changing mode currently only supported in OTG mode\n");
		return -EINVAL;
	}

	switch (musb_mode) {
#ifdef CONFIG_USB_MUSB_HDRC_HCD
	case MUSB_HOST:		/* Enable vbus */
		omap_set_vbus(musb, 1);
		break;
#endif

#ifdef CONFIG_USB_GADGET_MUSB_HDRC
	case MUSB_PERIPHERAL:	/* disable vbus */
		omap_set_vbus(musb, 0);
		break;
#endif

#ifdef CONFIG_USB_MUSB_OTG
	case MUSB_OTG:		/* Use PHY ID detection */
		break;
#endif

	default:
		DBG(2, "Trying to set mode %i\n", musb_mode);
		return -EINVAL;
	}

	return 0;
}

int __init musb_platform_init(struct musb *musb, void *board_data)
{
	u32 l;
	struct omap_musb_board_data *data = board_data;

#if defined(CONFIG_ARCH_OMAP2430)
	omap_cfg_reg(AE5_2430_USB0HS_STP);
#endif

	/* We require some kind of external transceiver, hooked
	 * up through ULPI.  TWL4030-family PMICs include one,
	 * which needs a driver, drivers aren't always needed.
	 */
	struct otg_transceiver *x = musb->xceiv = otg_get_transceiver();

	if (!musb->xceiv) {
		pr_err("HS USB OTG: no transceiver configured\n");
		return -ENODEV;
	}

	musb_platform_resume(musb);

	l = omap_readl(OTG_SYSCONFIG);
	if (cpu_is_omap3630()) {
		/*
		* Do Forcestdby here, for the case when kernel boots up
		 * without cable attached, force_active(0) won't be called.
		 */
		l |= ENABLEWAKEUP;	/* Enable wakeup */
		l &= ~NOSTDBY;		/* remove possible nostdby */
		l |= SMARTSTDBY;	/* enable smart standby */
		l &= ~AUTOIDLE;		/* disable auto idle */
		l &= ~NOIDLE;		/* remove possible noidle */
		l |= FORCEIDLE;		/* enable force idle */
	} else {
		l &= ~ENABLEWAKEUP;	/* disable wakeup */
		l &= ~NOSTDBY;		/* remove possible nostdby */
		l |= SMARTSTDBY;	/* enable smart standby */
		l &= ~AUTOIDLE;		/* disable auto idle */
		l &= ~NOIDLE;		/* remove possible noidle */
		l |= SMARTIDLE;		/* enable smart idle */
	}
	/*
	 * MUSB AUTOIDLE and SMARTIDLE don't work in 3430.
	 * Workaround by Richard Woodruff/TI
	 */
	if (!cpu_is_omap3430()) {
		l |= AUTOIDLE;		/* enable auto idle */
	}

	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_INTERFSEL);

	if (data->interface_type == MUSB_INTERFACE_UTMI) {
		/* OMAP4 uses Internal PHY GS70 which uses UTMI interface */
		l &= ~ULPI_12PIN;       /* Disable ULPI */
		l |= UTMI_8BIT;         /* Enable UTMI  */
	} else {
		l |= ULPI_12PIN;
	}

	omap_writel(l, OTG_INTERFSEL);

	pr_debug("HS USB OTG: revision 0x%x, sysconfig 0x%02x, "
			"sysstatus 0x%x, intrfsel 0x%x, simenable  0x%x\n",
			omap_readl(OTG_REVISION), omap_readl(OTG_SYSCONFIG),
			omap_readl(OTG_SYSSTATUS), omap_readl(OTG_INTERFSEL),
			omap_readl(OTG_SIMENABLE));

	omap_vbus_power(musb, musb->board_mode == MUSB_HOST, 1);

	if (is_host_enabled(musb))
		musb->board_set_vbus = omap_set_vbus;

	x->link_save_context = musb_link_save_context;
	x->link_restore_context = musb_link_restore_context;
	x->link_force_active = musb_link_force_active;
	x->link = musb;

	otg_put_transceiver(x);

	setup_timer(&musb_idle_timer, musb_do_idle, (unsigned long) musb);

	return 0;
}

#ifdef	CONFIG_PM

struct musb_csr_regs {
	/* FIFO registers */
	u16 txmaxp, txcsr, rxmaxp, rxcsr, rxcount;
	u16 rxfifoadd, txfifoadd;
	u8 txtype, txinterval, rxtype, rxinterval;
	u8 rxfifosz, txfifosz;
};

static struct musb_context_registers {

	u32 off_counter;
	u32 otg_sysconfig, otg_forcestandby;

	u8 faddr, power;
	u16 intrtx, intrrx, intrtxe, intrrxe;
	u8 intrusb, intrusbe;
	u16 frame;
	u8 index, testmode;

	u8 devctl, misc;

	struct musb_csr_regs index_regs[MUSB_C_NUM_EPS];

} musb_context;

void musb_platform_save_context(struct musb *musb)
{
	int i;
	u32 l;
	struct musb_hdrc_platform_data *plat = musb->controller->platform_data;

	DBG(4, "Saving musb_registers\n");

	if (plat->context_loss_counter) {
		musb_context.off_counter =
			plat->context_loss_counter(musb->controller);
	}

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~SMARTSTDBY;       /* disable smart standby */
	l |= NOSTDBY;           /* enable nostdby */
	l &= ~SMARTIDLE;        /* disable smart idle */
	l |= NOIDLE;            /* enable noidle */

	musb_context.otg_sysconfig = l;
	musb_context.otg_forcestandby = omap_readl(OTG_FORCESTDBY) & ~ENABLEFORCE;

	musb_context.faddr = musb_readb(musb->mregs, MUSB_FADDR);
	musb_context.power = musb_readb(musb->mregs, MUSB_POWER);
	musb_context.intrtx = musb_readw(musb->mregs, MUSB_INTRTX);
	musb_context.intrrx = musb_readw(musb->mregs, MUSB_INTRRX);
	musb_context.intrtxe = musb_readw(musb->mregs, MUSB_INTRTXE);
	musb_context.intrrxe = musb_readw(musb->mregs, MUSB_INTRRXE);
	musb_context.intrusb = musb_readb(musb->mregs, MUSB_INTRUSB);
	musb_context.intrusbe = musb_readb(musb->mregs, MUSB_INTRUSBE);
	musb_context.frame = musb_readw(musb->mregs, MUSB_FRAME);
	musb_context.index = musb_readb(musb->mregs, MUSB_INDEX);
	musb_context.testmode = musb_readb(musb->mregs, MUSB_TESTMODE);

	musb_context.devctl = musb_readb(musb->mregs, MUSB_DEVCTL);

	for (i = 0; i < MUSB_C_NUM_EPS; ++i) {
		musb_writeb(musb->mregs, MUSB_INDEX, i);
		musb_context.index_regs[i].txmaxp =
			musb_readw(musb->mregs, 0x10 + MUSB_TXMAXP);
		musb_context.index_regs[i].txcsr =
			musb_readw(musb->mregs, 0x10 + MUSB_TXCSR);
		musb_context.index_regs[i].rxmaxp =
			musb_readw(musb->mregs, 0x10 + MUSB_RXMAXP);
		musb_context.index_regs[i].rxcsr =
			musb_readw(musb->mregs, 0x10 + MUSB_RXCSR);
		musb_context.index_regs[i].rxcount =
			musb_readw(musb->mregs, 0x10 + MUSB_RXCOUNT);
		musb_context.index_regs[i].txtype =
			musb_readb(musb->mregs, 0x10 + MUSB_TXTYPE);
		musb_context.index_regs[i].txinterval =
			musb_readb(musb->mregs, 0x10 + MUSB_TXINTERVAL);
		musb_context.index_regs[i].rxtype =
			musb_readb(musb->mregs, 0x10 + MUSB_RXTYPE);
		musb_context.index_regs[i].rxinterval =
			musb_readb(musb->mregs, 0x10 + MUSB_RXINTERVAL);

		musb_context.index_regs[i].txfifoadd =
			musb_readw(musb->mregs, MUSB_TXFIFOADD);
		musb_context.index_regs[i].rxfifoadd =
			musb_readw(musb->mregs, MUSB_RXFIFOADD);
		musb_context.index_regs[i].txfifosz =
			musb_readw(musb->mregs, MUSB_TXFIFOSZ);
		musb_context.index_regs[i].rxfifosz =
			musb_readw(musb->mregs, MUSB_RXFIFOSZ);
	}

	musb_writeb(musb->mregs, MUSB_INDEX,
				musb_context.index);

}

void musb_platform_restore_context(struct musb *musb)
{
	int i;

	DBG(4, "Restoring musb_registers\n");

	musb_writeb(musb->mregs, MUSB_FADDR,
				musb_context.faddr);
	musb_writeb(musb->mregs, MUSB_POWER,
				musb_context.power);
	musb_writew(musb->mregs, MUSB_INTRTX,
				musb_context.intrtx);
	musb_writew(musb->mregs, MUSB_INTRRX,
				musb_context.intrrx);
	musb_writew(musb->mregs, MUSB_INTRTXE,
				musb_context.intrtxe);
	musb_writew(musb->mregs, MUSB_INTRRXE,
				musb_context.intrrxe);
	musb_writeb(musb->mregs, MUSB_INTRUSB,
				musb_context.intrusb);
	musb_writeb(musb->mregs, MUSB_INTRUSBE,
				musb_context.intrusbe);
	musb_writew(musb->mregs, MUSB_FRAME,
				musb_context.frame);
	musb_writeb(musb->mregs, MUSB_TESTMODE,
				musb_context.testmode);
	musb_writeb(musb->mregs, MUSB_DEVCTL,
				musb_context.devctl);


	for (i = 0; i < MUSB_C_NUM_EPS; ++i) {
		musb_writeb(musb->mregs, MUSB_INDEX, i);
		musb_writew(musb->mregs, 0x10 + MUSB_TXMAXP,
			musb_context.index_regs[i].txmaxp);
		musb_writew(musb->mregs, 0x10 + MUSB_TXCSR,
			musb_context.index_regs[i].txcsr);
		musb_writew(musb->mregs, 0x10 + MUSB_RXMAXP,
			musb_context.index_regs[i].rxmaxp);
		musb_writew(musb->mregs, 0x10 + MUSB_RXCSR,
			musb_context.index_regs[i].rxcsr);
		musb_writew(musb->mregs, 0x10 + MUSB_RXCOUNT,
			musb_context.index_regs[i].rxcount);
		musb_writeb(musb->mregs, 0x10 + MUSB_TXTYPE,
			musb_context.index_regs[i].txtype);
		musb_writeb(musb->mregs, 0x10 + MUSB_TXINTERVAL,
			musb_context.index_regs[i].txinterval);
		musb_writeb(musb->mregs, 0x10 + MUSB_RXTYPE,
			musb_context.index_regs[i].rxtype);
		musb_writeb(musb->mregs, 0x10 + MUSB_RXINTERVAL,
			musb_context.index_regs[i].rxinterval);

		musb_writew(musb->mregs, MUSB_TXFIFOSZ,
			musb_context.index_regs[i].txfifosz);
		musb_writew(musb->mregs, MUSB_RXFIFOSZ,
			musb_context.index_regs[i].rxfifosz);
		musb_writew(musb->mregs, MUSB_TXFIFOADD,
			musb_context.index_regs[i].txfifoadd);
		musb_writew(musb->mregs, MUSB_RXFIFOADD,
			musb_context.index_regs[i].rxfifoadd);
	}

	musb_writeb(musb->mregs, MUSB_INDEX,
				musb_context.index);
#ifndef CONFIG_MACH_OMAP3621_EVT1A
    // For Encore this is resumed via the link_state callback
    // triggered by the twl4030 VBUS interrupt
    omap_writel(musb_context.otg_sysconfig, OTG_SYSCONFIG);
    omap_writel(musb_context.otg_forcestandby, OTG_FORCESTDBY);
#endif
}

void musb_link_save_context(struct otg_transceiver *xceiv)
{

	struct musb	*musb = xceiv->link;

	struct musb_hdrc_platform_data *plat =
			musb->controller->platform_data;

	musb_platform_save_context(musb);

	/* On cable detach remove restriction on CORE domain:
	 * Now core domain can go to off
	 * REVISIT this logic for OMAP4
	 */
	if (cpu_is_omap3630() || cpu_is_omap3430())
		omap_pm_set_max_mpu_wakeup_lat(musb->controller, -1);

	/* Initialize vdd1 to min opp1 constraint  */
	if (plat->set_vdd1_opp)
		plat->set_vdd1_opp(musb->controller, plat->min_vdd1_opp);

}

void musb_link_restore_context(struct otg_transceiver *xceiv)
{
	struct musb	*musb = xceiv->link;
	struct musb_hdrc_platform_data *plat =
			musb->controller->platform_data;

	/* MUSB has no h/w SAR: So restrict CORE domain
	 * from going to OFF mode
	 * So prevent CPUIdle going to C7, restrict to C6
	 * REVISIT this logic for OMAP4
	 */
	if (cpu_is_omap3630() || cpu_is_omap3430())
		omap_pm_set_max_mpu_wakeup_lat(musb->controller, 6250);

	/* Initialize vdd1 to max opp1 constraint  */
	if (plat->set_vdd1_opp)
		plat->set_vdd1_opp(musb->controller, plat->max_vdd1_opp);

	/* No context restore needed in case
	 * OFF transition has not happened
	 */
	if (plat->context_loss_counter) {
		if (musb_context.off_counter ==
			plat->context_loss_counter(musb->controller)) {
				DBG(4, "No context was lost. Not restoring.\n");
				return;
		}
	}

	musb_platform_restore_context(musb);
}

void musb_link_force_active(int enable)
{
	u32 l;

	l = omap_readl(OTG_SYSCONFIG);

	/*
	 * We have encountered enumeration problems on the omap3
	 * when power management has been enabled and the device
	 * is transitioning in and out of CORE retention. To
	 * workaround this problem, we force the musb controller
	 * to be always active (no idle/standby) when the usb
	 * cable is attached and inactive (smart idle/standby)
	 * when the cable is removed.
	 */
	if (enable) {
		l &= ~SMARTSTDBY;       /* disable smart standby */
		l |= NOSTDBY;           /* enable nostdby */
		l &= ~SMARTIDLE;        /* disable smart idle */
		l |= NOIDLE;            /* enable noidle */
		if (cpu_is_omap3630())
			/* Disable Mstandby */
			omap_writel((omap_readl(OTG_FORCESTDBY) & ~ENABLEFORCE),
					 OTG_FORCESTDBY);
	} else {
		if (cpu_is_omap3630()) {
			/*
			 * When the device is disconnected put the OTG to
			 * Forceidle and Forcestdby. There where known issues
			 * on different custom boards, wherein
			 * the OTG idle ack was broken. May be the OTG senses
			 * some fake activity on the lines.
			 * Also, this might be moved to suspend/resume hooks.
			 * Right now, musb doesn't have suspend/resume hooks
			 * plugged in to LDM. REVISIT: Seen only on 3630.
			 */
			l |= ENABLEWAKEUP;	/* Enable wakeup */
			l &= ~SMARTSTDBY;	/* enable smart standby */
			l |= FORCESTDBY;
			l &= ~NOSTDBY;		/* disable nostdby */
			l |= FORCEIDLE;
			l &= ~NOIDLE;		/* disable noidle */

			/* Enable Mstdby */
			omap_writel((omap_readl(OTG_FORCESTDBY) | ENABLEFORCE),
					 OTG_FORCESTDBY);
		} else {
			l &= ~NOSTDBY;		/* disable nostdby */
			l |= SMARTIDLE;		/* enable smart idle */
			l &= ~NOIDLE;		/* disable noidle */
		}
	}

	omap_writel(l, OTG_SYSCONFIG);
}

#else

#define musb_platform_save_context	do {} while (0)
#define musb_platform_restore_context	do {} while (0)
#define musb_link_save_context	do {} while (0)
#define musb_link_restore_context	do {} while (0)
#define musb_link_force_active do {} while (0)

#endif

int musb_platform_suspend(struct musb *musb)
{
	u32 l;

	if (!musb->clock)
		return 0;

	/* in any role */
	l = omap_readl(OTG_FORCESTDBY);
	l |= ENABLEFORCE;	/* enable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	l = omap_readl(OTG_SYSCONFIG);
	l |= ENABLEWAKEUP;	/* enable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	otg_set_suspend(musb->xceiv, 1);

	if (musb->set_clock)
		musb->set_clock(musb->clock, 0);
	else
		clk_disable(musb->clock);

	return 0;
}

static int musb_platform_resume(struct musb *musb)
{
	u32 l;

	if (!musb->clock)
		return 0;

	otg_set_suspend(musb->xceiv, 0);

	if (musb->set_clock)
		musb->set_clock(musb->clock, 1);
	else
		clk_enable(musb->clock);

	l = omap_readl(OTG_SYSCONFIG);
	l &= ~ENABLEWAKEUP;	/* disable wakeup */
	omap_writel(l, OTG_SYSCONFIG);

	l = omap_readl(OTG_FORCESTDBY);
	l &= ~ENABLEFORCE;	/* disable MSTANDBY */
	omap_writel(l, OTG_FORCESTDBY);

	return 0;
}


int musb_platform_exit(struct musb *musb)
{
	struct otg_transceiver *x = otg_get_transceiver();

	del_timer_sync(&musb_idle_timer);

	omap_vbus_power(musb, 0 /*off*/, 1);

	musb_platform_suspend(musb);

	clk_put(musb->clock);
	musb->clock = 0;

	x->link_save_context = NULL;
	x->link_restore_context = NULL;
	x->link = NULL;

	otg_put_transceiver(x);

	return 0;
}
