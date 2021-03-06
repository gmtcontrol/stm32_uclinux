/*
 * (C) Copyright 2011, 2012
 * Emcraft Systems, <www.emcraft.com>
 * Yuri Tikhonov <yur@emcraft.com>
 * Alexander Potashev <aspotashev@emcraft.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <linux/clockchips.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>

#include <asm/hardware/cortexm3.h>
#include <mach/clock.h>
#include <mach/stm32.h>
#include <mach/timer.h>

/*
 * In STM32 we use two 32-bit TIMs to implement Clock Event and Clock
 * Source devices:
 * - TIM2 is a base for Clock Event device (system ticks per HZ);
 * - TIM5 is a base for Clock Source device (system time, etc.).
 *
 * Note: the other STM32 TIMs are 16-bit counters, so be carefull if
 * decide to replace TIM2/5 with some other TIMs here.
 */

/*
 * STM32 RCC reset & enable regs and fields
 */
#define STM32_RCC_ENR_TIM2	(STM32_RCC_BASE +			       \
				 offsetof(struct stm32_rcc_regs, apb1enr))
#define STM32_RCC_RST_TIM2	(STM32_RCC_BASE +			       \
				 offsetof(struct stm32_rcc_regs, apb1rstr))
#define STM32_RCC_MSK_TIM2	(1 << 0)

#define STM32_RCC_ENR_TIM5	(STM32_RCC_BASE +			       \
				 offsetof(struct stm32_rcc_regs, apb1enr))
#define STM32_RCC_RST_TIM5	(STM32_RCC_BASE +			       \
				 offsetof(struct stm32_rcc_regs, apb1rstr))
#define STM32_RCC_MSK_TIM5	(1 << 3)

/*
 * STM32 Timers IRQ numbers
 */
#define STM32_TIM2_IRQ		28
#define STM32_TIM5_IRQ		50

/*
 * STM32 Timer reg bases
 */
#define STM32_TIM2_BASE		(STM32_APB1PERITH_BASE + 0x0000)
#define STM32_TIM5_BASE		(STM32_APB1PERITH_BASE + 0x0C00)

/*
 * STM32 TIM CR1 fields
 */
#define STM32_TIM_CR1_CEN	(1 << 0)	/* Counter enable	      */
#define STM32_TIM_CR1_ARPE	(1 << 7)	/* Auto-reload preload enable */

/*
 * STM32 TIM DIER
 */
#define STM32_TIM_DIER_UIE	(1 << 0)	/* Update interrupt enable    */

/*
 * STM32 TIM SR fields
 */
#define STM32_TIM_SR_UIF	(1 << 0)	/* Update interrupt flag      */

/*
 * STM32 TIM EGR fields
 */
#define STM32_TIM_EGR_UG	(1 << 0)	/* Update generation	      */

/*
 * STM32 Timer register map
 */
struct stm32_tim_regs {
	u16	cr1;		/* Control 1				      */
	u16	rsv0;
	u16	cr2;		/* Control 2				      */
	u16	rsv1;
	u16	smcr;		/* Slave mode control			      */
	u16	rsv2;
	u16	dier;		/* DMA/interrupt enable			      */
	u16	rsv3;
	u16	sr;		/* Status				      */
	u16	rsv4;
	u16	egr;		/* Event generation			      */
	u16	rsv5;
	u16	ccmr1;		/* Capture/compare mode 1		      */
	u16	rsv6;
	u16	ccmr2;		/* Capture/compare mode 2		      */
	u16	rsv7;
	u16	ccer;		/* Capture/compare enable		      */
	u16	rsv8;
	u32	cnt;		/* Counter				      */
	u16	psc;		/* Prescaler				      */
	u16	rsv9;
	u32	arr;		/* Auto-reload				      */
	u16	rcr;		/* Repetition counter			      */
	u16	rsv10;
	u32	ccr1;		/* Capture/compare 1			      */
	u32	ccr2;		/* Capture/compare 2			      */
	u32	ccr3;		/* Capture/compare 3			      */
	u32	ccr4;		/* Capture/compare 4			      */
	u16	bdtr;		/* Break and dead-time			      */
	u16	rsv11;
	u16	dcr;		/* DMA control				      */
	u16	rsv12;
	u16	dmar;		/* Dma address for full transfer	      */
	u16	rsv13;
	u16	or;		/* Option				      */
	u16	rsv14;
};

/*
 * System Tick timer settings
 */
#define TICK_TIM_BASE		STM32_TIM2_BASE
#define TICK_TIM_IRQ		STM32_TIM2_IRQ
#define TICK_TIM_RCC_RST	STM32_RCC_RST_TIM2
#define TICK_TIM_RCC_ENR	STM32_RCC_ENR_TIM2
#define TICK_TIM_RCC_MSK	STM32_RCC_MSK_TIM2
#define TICK_TIM_CLOCK		CLOCK_PTMR1

/*
 * System timer clock event device set mode function
 */
static void tick_tmr_set_mode(enum clock_event_mode mode,
			     struct clock_event_device *clk)
{
	volatile struct stm32_tim_regs *tim;

	tim = (struct stm32_tim_regs *)TICK_TIM_BASE;

	switch (mode) 
	{
		/*
		 * Enable the timer.
		 */
		case CLOCK_EVT_MODE_PERIODIC:
		case CLOCK_EVT_MODE_RESUME:
			tim->cr1 |= STM32_TIM_CR1_CEN;
			break;
		
		/*
		 * Disable the timer.
		 */
		case CLOCK_EVT_MODE_ONESHOT:
		case CLOCK_EVT_MODE_UNUSED:
		case CLOCK_EVT_MODE_SHUTDOWN:
		default:
			tim->cr1 &= ~STM32_TIM_CR1_CEN;
			break;
	}//switch
}//tick_tmr_set_mode

/*
 * Configure the timer to generate an interrupt in the specified amount of ticks
 */
static int tick_tmr_set_next_event(unsigned long delta, 
				struct clock_event_device *clk)
{
	volatile struct stm32_tim_regs	*tim;
	unsigned long flags;

	tim = (struct stm32_tim_regs *)TICK_TIM_BASE;

	raw_local_irq_save(flags);
	tim->arr = delta;
	tim->cnt = 0;
	tim->cr1|= STM32_TIM_CR1_CEN;
	raw_local_irq_restore(flags);

	return 0;
}//tick_tmr_set_next_event

/*
 * STM32 System Timer device
 */
static struct clock_event_device tick_tmr_clockevent = 
{
	.name			= "STM32 System Timer",
	.rating			= 200,
	.irq			= TICK_TIM_IRQ,
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	.set_mode		= tick_tmr_set_mode,
	.set_next_event	= tick_tmr_set_next_event,
	.cpumask		= cpu_all_mask,
};

/*
 * System Timer IRQ handler
 */
static irqreturn_t tick_tmr_irq_handler(int irq, void *dev_id)
{
	volatile struct stm32_tim_regs *tim;
	struct clock_event_device *evt = &tick_tmr_clockevent;

	tim = (struct stm32_tim_regs *)TICK_TIM_BASE;

	/*
	 * Clear the interrupt
	 */
	tim->sr &= ~STM32_TIM_SR_UIF;

	/*
	 * Handle Event
	 */
	evt->event_handler(evt);

	return IRQ_HANDLED;
}//tick_tmr_irq_handler

/*
 * System timer IRQ action
 */
static struct irqaction	tick_tmr_irqaction = 
{
	.name		= "STM32 Kernel Time Tick",
	.flags		= IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= tick_tmr_irq_handler,
};

/*
 * System Timer Clockevents init
 */
static void tick_tmr_init(u32 tmr_clk_freq)
{
	u64 max_delay_in_sec = ((u64)0xFFFFFFFF)/tmr_clk_freq;
	volatile struct stm32_tim_regs *tim;
	volatile u32 *rcc_enr, *rcc_rst;
	struct clock_event_device *evt = &tick_tmr_clockevent;

	/*
	 * Setup reg bases
	 */
	tim = (struct stm32_tim_regs *)TICK_TIM_BASE;
	rcc_enr = (u32 *)TICK_TIM_RCC_ENR;
	rcc_rst = (u32 *)TICK_TIM_RCC_RST;

	/*
	 * Enable timer clock, and deinit registers
	 */
	*rcc_enr |=  TICK_TIM_RCC_MSK;
	*rcc_rst |=  TICK_TIM_RCC_MSK;
	*rcc_rst &= ~TICK_TIM_RCC_MSK;

	/*
	 * Select the counter mode:
	 * - upcounter;
	 * - auto-reload
	 */
	tim->cr1 = 0;
	tim->arr = 0xFFFFFFFF;
	tim->psc = 0;
	tim->cnt = 0;

	/*
	 * Generate an update event to reload the Prescaler value immediately
	 */
	tim->egr = STM32_TIM_EGR_UG;

	/*
	 * Setup, and enable IRQ
	 */
	setup_irq(TICK_TIM_IRQ, &tick_tmr_irqaction);
	tim->dier |= STM32_TIM_DIER_UIE;

	/*
	 * Set the fields required for the set_next_event method (tickless kernel support)
	 */
	clockevents_calc_mult_shift(evt, tmr_clk_freq, max_delay_in_sec);
	//evt->max_delta_ns = NSEC_PER_SEC * max_delay_in_sec;
	//evt->min_delta_ns = clockevent_delta2ns(1, evt);
	evt->max_delta_ns = clockevent_delta2ns(0xFFFFFFF0, evt);
	evt->min_delta_ns = clockevent_delta2ns(0xF, evt);

	clockevents_register_device(evt);
}//tick_tmr_init

/*
 * Initialize the timer systems of the STM32
 */
void __init stm32_timer_init(void)
{
	/*
	 * Configure the STM32 clocks, and get the reference clock value
	 */
	stm32_clock_init();

	/*
	 * Add the Cortex-M3 SysTick timer to the clock source
	 */
	cortex_m3_register_systick_clocksource(stm32_clock_get(CLOCK_HCLK));

	/*
	 * Add the clockevent for system tick
	 */
	tick_tmr_init(stm32_clock_get(TICK_TIM_CLOCK));
}//stm32_timer_init
