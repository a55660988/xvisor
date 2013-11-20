/**
 * Copyright (c) 2013 Anup Patel.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file clk-sunxi.c
 * @author Anup Patel (anup@brainfault.org)
 * @brief SunXi clock implementation
 *
 * Adapted from linux/drivers/clk/sunxi/clk-sunxi.c
 *
 * Copyright 2013 Emilio López
 *
 * Emilio López <emilio@elopez.com.ar>
 *
 * The original source is licensed under GPL.
 */

#include <vmm_compiler.h>
#include <vmm_compiler.h>
#include <vmm_heap.h>
#include <vmm_devtree.h>
#include <libs/bitmap.h>
#include <libs/mathlib.h>
#include <drv/clk-provider.h>
#include <drv/clkdev.h>

#include "clk-factors.h"

static DEFINE_SPINLOCK(clk_lock);

/**
 * sun4i_osc_clk_setup() - Setup function for gatable oscillator
 */

#define SUNXI_OSC24M_GATE	0

static void __init sun4i_osc_clk_setup(struct vmm_devtree_node *node)
{
	struct clk *clk;
	struct clk_fixed_rate *fixed;
	struct clk_gate *gate;
	void *attrval;
	virtual_addr_t reg_va;
	const char *clk_name = node->name;
	u32 rate;

	/* allocate fixed-rate and gate clock structs */
	fixed = vmm_zalloc(sizeof(struct clk_fixed_rate));
	if (!fixed)
		return;
	gate = vmm_zalloc(sizeof(struct clk_gate));
	if (!gate) {
		vmm_free(fixed);
		return;
	}

	attrval = vmm_devtree_attrval(node, "clock-frequency");
	if (!attrval) {
		vmm_free(gate);
		vmm_free(fixed);
		return;
	}
	rate = *(u32 *)attrval;

	/* set up gate and fixed rate properties */
	if (vmm_devtree_regmap(node, &reg_va, 0)) {
		vmm_free(gate);
		vmm_free(fixed);
		return;
	}
	gate->reg = (void *)reg_va;
	gate->bit_idx = SUNXI_OSC24M_GATE;
	gate->lock = &clk_lock;
	fixed->fixed_rate = rate;

	clk = clk_register_composite(NULL, clk_name,
			NULL, 0,
			NULL, NULL,
			&fixed->hw, &clk_fixed_rate_ops,
			&gate->hw, &clk_gate_ops,
			CLK_IS_ROOT);

	if (!clk) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
	}
}
CLK_OF_DECLARE(sun4i_osc, "allwinner,sun4i-osc-clk", sun4i_osc_clk_setup);



/**
 * sun4i_get_pll1_factors() - calculates n, k, m, p factors for PLL1
 * PLL1 rate is calculated as follows
 * rate = (parent_rate * n * (k + 1) >> p) / (m + 1);
 * parent_rate is always 24Mhz
 */

static void sun4i_get_pll1_factors(u32 *freq, u32 parent_rate,
				   u8 *n, u8 *k, u8 *m, u8 *p)
{
	u8 div;

	/* Normalize value to a 6M multiple */
	div = *freq / 6000000;
	*freq = 6000000 * div;

	/* we were called to round the frequency, we can now return */
	if (n == NULL)
		return;

	/* m is always zero for pll1 */
	*m = 0;

	/* k is 1 only on these cases */
	if (*freq >= 768000000 || *freq == 42000000 || *freq == 54000000)
		*k = 1;
	else
		*k = 0;

	/* p will be 3 for divs under 10 */
	if (div < 10)
		*p = 3;

	/* p will be 2 for divs between 10 - 20 and odd divs under 32 */
	else if (div < 20 || (div < 32 && (div & 1)))
		*p = 2;

	/* p will be 1 for even divs under 32, divs under 40 and odd pairs
	 * of divs between 40-62 */
	else if (div < 40 || (div < 64 && (div & 2)))
		*p = 1;

	/* any other entries have p = 0 */
	else
		*p = 0;

	/* calculate a suitable n based on k and p */
	div <<= *p;
	div = udiv32(div, (*k + 1));
	*n = div / 4;
}

/**
 * sun6i_a31_get_pll1_factors() - calculates n, k and m factors for PLL1
 * PLL1 rate is calculated as follows
 * rate = parent_rate * (n + 1) * (k + 1) / (m + 1);
 * parent_rate should always be 24MHz
 */
static void sun6i_a31_get_pll1_factors(u32 *freq, u32 parent_rate,
				       u8 *n, u8 *k, u8 *m, u8 *p)
{
	/*
	 * We can operate only on MHz, this will make our life easier
	 * later.
	 */
	u32 freq_mhz = *freq / 1000000;
	u32 parent_freq_mhz = parent_rate / 1000000;

	/*
	 * Round down the frequency to the closest multiple of either
	 * 6 or 16
	 */
	u32 round_freq_6 = round_down(freq_mhz, 6);
	u32 round_freq_16 = round_down(freq_mhz, 16);

	if (round_freq_6 > round_freq_16)
		freq_mhz = round_freq_6;
	else
		freq_mhz = round_freq_16;

	*freq = freq_mhz * 1000000;

	/*
	 * If the factors pointer are null, we were just called to
	 * round down the frequency.
	 * Exit.
	 */
	if (n == NULL)
		return;

	/* If the frequency is a multiple of 32 MHz, k is always 3 */
	if (!(freq_mhz % 32))
		*k = 3;
	/* If the frequency is a multiple of 9 MHz, k is always 2 */
	else if (!(freq_mhz % 9))
		*k = 2;
	/* If the frequency is a multiple of 8 MHz, k is always 1 */
	else if (!(freq_mhz % 8))
		*k = 1;
	/* Otherwise, we don't use the k factor */
	else
		*k = 0;

	/*
	 * If the frequency is a multiple of 2 but not a multiple of
	 * 3, m is 3. This is the first time we use 6 here, yet we
	 * will use it on several other places.
	 * We use this number because it's the lowest frequency we can
	 * generate (with n = 0, k = 0, m = 3), so every other frequency
	 * somehow relates to this frequency.
	 */
	if ((freq_mhz % 6) == 2 || (freq_mhz % 6) == 4)
		*m = 2;
	/*
	 * If the frequency is a multiple of 6MHz, but the factor is
	 * odd, m will be 3
	 */
	else if ((freq_mhz / 6) & 1)
		*m = 3;
	/* Otherwise, we end up with m = 1 */
	else
		*m = 1;

	/* Calculate n thanks to the above factors we already got */
	*n = udiv32((freq_mhz * (*m + 1)), ((*k + 1) * parent_freq_mhz)) - 1;

	/*
	 * If n end up being outbound, and that we can still decrease
	 * m, do it.
	 */
	if ((*n + 1) > 31 && (*m + 1) > 1) {
		*n = (*n + 1) / 2 - 1;
		*m = (*m + 1) / 2 - 1;
	}
}

/**
 * sun4i_get_apb1_factors() - calculates m, p factors for APB1
 * APB1 rate is calculated as follows
 * rate = (parent_rate >> p) / (m + 1);
 */

static void sun4i_get_apb1_factors(u32 *freq, u32 parent_rate,
				   u8 *n, u8 *k, u8 *m, u8 *p)
{
	u8 calcm, calcp;

	if (parent_rate < *freq)
		*freq = parent_rate;

	parent_rate = udiv32((parent_rate + (*freq - 1)), *freq);

	/* Invalid rate! */
	if (parent_rate > 32)
		return;

	if (parent_rate <= 4)
		calcp = 0;
	else if (parent_rate <= 8)
		calcp = 1;
	else if (parent_rate <= 16)
		calcp = 2;
	else
		calcp = 3;

	calcm = (parent_rate >> calcp) - 1;

	*freq = udiv32((parent_rate >> calcp), (calcm + 1));

	/* we were called to round the frequency, we can now return */
	if (n == NULL)
		return;

	*m = calcm;
	*p = calcp;
}



/**
 * sunxi_factors_clk_setup() - Setup function for factor clocks
 */

struct factors_data {
	struct clk_factors_config *table;
	void (*getter) (u32 *rate, u32 parent_rate, u8 *n, u8 *k, u8 *m, u8 *p);
};

static struct clk_factors_config sun4i_pll1_config = {
	.nshift = 8,
	.nwidth = 5,
	.kshift = 4,
	.kwidth = 2,
	.mshift = 0,
	.mwidth = 2,
	.pshift = 16,
	.pwidth = 2,
};

static struct clk_factors_config sun6i_a31_pll1_config = {
	.nshift	= 8,
	.nwidth = 5,
	.kshift = 4,
	.kwidth = 2,
	.mshift = 0,
	.mwidth = 2,
};

static struct clk_factors_config sun4i_apb1_config = {
	.mshift = 0,
	.mwidth = 5,
	.pshift = 16,
	.pwidth = 2,
};

static const struct factors_data sun4i_pll1_data __initconst = {
	.table = &sun4i_pll1_config,
	.getter = sun4i_get_pll1_factors,
};

static const struct factors_data sun6i_a31_pll1_data __initconst = {
	.table = &sun6i_a31_pll1_config,
	.getter = sun6i_a31_get_pll1_factors,
};

static const struct factors_data sun4i_apb1_data __initconst = {
	.table = &sun4i_apb1_config,
	.getter = sun4i_get_apb1_factors,
};

static void __init sunxi_factors_clk_setup(struct vmm_devtree_node *node,
					   struct factors_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent;
	virtual_addr_t reg_va;
	void *reg;

	if (vmm_devtree_regmap(node, &reg_va, 0)) {
		return;
	}
	reg = (void *)reg_va;

	parent = of_clk_get_parent_name(node, 0);

	clk = clk_register_factors(NULL, clk_name, parent, 0, reg,
				   data->table, data->getter, &clk_lock);

	if (!clk) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
	}
}



/**
 * sunxi_mux_clk_setup() - Setup function for muxes
 */

#define SUNXI_MUX_GATE_WIDTH	2

struct mux_data {
	u8 shift;
};

static const struct mux_data sun4i_cpu_mux_data __initconst = {
	.shift = 16,
};

static const struct mux_data sun6i_a31_ahb1_mux_data __initconst = {
	.shift = 12,
};

static const struct mux_data sun4i_apb1_mux_data __initconst = {
	.shift = 24,
};

static void __init sunxi_mux_clk_setup(struct vmm_devtree_node *node,
				       struct mux_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parents[5];
	virtual_addr_t reg_va;
	void *reg;
	int i = 0;

	if (vmm_devtree_regmap(node, &reg_va, 0)) {
		return;
	}
	reg = (void *)reg_va;

	while (i < 5 && (parents[i] = of_clk_get_parent_name(node, i)) != NULL)
		i++;

	clk = clk_register_mux(NULL, clk_name, parents, i,
			       CLK_SET_RATE_NO_REPARENT, reg,
			       data->shift, SUNXI_MUX_GATE_WIDTH,
			       0, &clk_lock);

	if (clk) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
	}
}



/**
 * sunxi_divider_clk_setup() - Setup function for simple divider clocks
 */

struct div_data {
	u8	shift;
	u8	pow;
	u8	width;
};

static const struct div_data sun4i_axi_data __initconst = {
	.shift	= 0,
	.pow	= 0,
	.width	= 2,
};

static const struct div_data sun4i_ahb_data __initconst = {
	.shift	= 4,
	.pow	= 1,
	.width	= 2,
};

static const struct div_data sun4i_apb0_data __initconst = {
	.shift	= 8,
	.pow	= 1,
	.width	= 2,
};

static const struct div_data sun6i_a31_apb2_div_data __initconst = {
	.shift	= 0,
	.pow	= 0,
	.width	= 4,
};

static void __init sunxi_divider_clk_setup(struct vmm_devtree_node *node,
					   struct div_data *data)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *clk_parent;
	virtual_addr_t reg_va;
	void *reg;

	if (vmm_devtree_regmap(node, &reg_va, 0)) {
		return;
	}
	reg = (void *)reg_va;

	clk_parent = of_clk_get_parent_name(node, 0);

	clk = clk_register_divider(NULL, clk_name, clk_parent, 0,
				   reg, data->shift, data->width,
				   data->pow ? CLK_DIVIDER_POWER_OF_TWO : 0,
				   &clk_lock);
	if (clk) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
	}
}



/**
 * sunxi_gates_clk_setup() - Setup function for leaf gates on clocks
 */

#define SUNXI_GATES_MAX_SIZE	64

struct gates_data {
	DEFINE_BITMAP(mask, SUNXI_GATES_MAX_SIZE);
};

static const struct gates_data sun4i_axi_gates_data __initconst = {
	.mask = {1},
};

static const struct gates_data sun4i_ahb_gates_data __initconst = {
	.mask = {0x7F77FFF, 0x14FB3F},
};

static const struct gates_data sun5i_a10s_ahb_gates_data __initconst = {
	.mask = {0x147667e7, 0x185915},
};

static const struct gates_data sun5i_a13_ahb_gates_data __initconst = {
	.mask = {0x107067e7, 0x185111},
};

static const struct gates_data sun6i_a31_ahb1_gates_data __initconst = {
	.mask = {0xEDFE7F62, 0x794F931},
};

static const struct gates_data sun7i_a20_ahb_gates_data __initconst = {
	.mask = { 0x12f77fff, 0x16ff3f },
};

static const struct gates_data sun4i_apb0_gates_data __initconst = {
	.mask = {0x4EF},
};

static const struct gates_data sun5i_a10s_apb0_gates_data __initconst = {
	.mask = {0x469},
};

static const struct gates_data sun5i_a13_apb0_gates_data __initconst = {
	.mask = {0x61},
};

static const struct gates_data sun7i_a20_apb0_gates_data __initconst = {
	.mask = { 0x4ff },
};

static const struct gates_data sun4i_apb1_gates_data __initconst = {
	.mask = {0xFF00F7},
};

static const struct gates_data sun5i_a10s_apb1_gates_data __initconst = {
	.mask = {0xf0007},
};

static const struct gates_data sun5i_a13_apb1_gates_data __initconst = {
	.mask = {0xa0007},
};

static const struct gates_data sun6i_a31_apb1_gates_data __initconst = {
	.mask = {0x3031},
};

static const struct gates_data sun6i_a31_apb2_gates_data __initconst = {
	.mask = {0x3F000F},
};

static const struct gates_data sun7i_a20_apb1_gates_data __initconst = {
	.mask = { 0xff80ff },
};

static void __init sunxi_gates_clk_setup(struct vmm_devtree_node *node,
					 struct gates_data *data)
{
	struct clk_onecell_data *clk_data;
	const char *clk_parent;
	const char *clk_name;
	virtual_addr_t reg_va;
	void *reg;
	int qty;
	int i = 0;
	int j = 0;
	int ignore;

	if (vmm_devtree_regmap(node, &reg_va, 0)) {
		return;
	}
	reg = (void *)reg_va;

	clk_parent = of_clk_get_parent_name(node, 0);

	/* Worst-case size approximation and memory allocation */
	qty = find_last_bit(data->mask, SUNXI_GATES_MAX_SIZE);
	clk_data = vmm_zalloc(sizeof(struct clk_onecell_data));
	if (!clk_data)
		return;
	clk_data->clks = vmm_zalloc((qty+1) * sizeof(struct clk *));
	if (!clk_data->clks) {
		vmm_free(clk_data);
		return;
	}

	for_each_set_bit(i, data->mask, SUNXI_GATES_MAX_SIZE) {
		vmm_devtree_attrval_string_index(node, "clock-output-names",
						 j, &clk_name);

		/* No driver claims this clock, but it should remain gated */
		ignore = !strcmp("ahb_sdram", clk_name) ? CLK_IGNORE_UNUSED : 0;

		clk_data->clks[i] = clk_register_gate(NULL, clk_name,
						      clk_parent, ignore,
						      reg + 4 * (i/32), i % 32,
						      0, &clk_lock);
		WARN_ON(!clk_data->clks[i]);

		j++;
	}

	/* Adjust to the real max */
	clk_data->clk_num = i;

	of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
}

/* Matches for factors clocks */
static const struct vmm_devtree_nodeid clk_factors_match[] __initconst = {
	{.compatible = "allwinner,sun4i-pll1-clk", .data = &sun4i_pll1_data,},
	{.compatible = "allwinner,sun6i-a31-pll1-clk", .data = &sun6i_a31_pll1_data,},
	{.compatible = "allwinner,sun4i-apb1-clk", .data = &sun4i_apb1_data,},
	{}
};

/* Matches for divider clocks */
static const struct vmm_devtree_nodeid clk_div_match[] __initconst = {
	{.compatible = "allwinner,sun4i-axi-clk", .data = &sun4i_axi_data,},
	{.compatible = "allwinner,sun4i-ahb-clk", .data = &sun4i_ahb_data,},
	{.compatible = "allwinner,sun4i-apb0-clk", .data = &sun4i_apb0_data,},
	{.compatible = "allwinner,sun6i-a31-apb2-div-clk", .data = &sun6i_a31_apb2_div_data,},
	{}
};

/* Matches for mux clocks */
static const struct vmm_devtree_nodeid clk_mux_match[] __initconst = {
	{.compatible = "allwinner,sun4i-cpu-clk", .data = &sun4i_cpu_mux_data,},
	{.compatible = "allwinner,sun4i-apb1-mux-clk", .data = &sun4i_apb1_mux_data,},
	{.compatible = "allwinner,sun6i-a31-ahb1-mux-clk", .data = &sun6i_a31_ahb1_mux_data,},
	{}
};

/* Matches for gate clocks */
static const struct vmm_devtree_nodeid clk_gates_match[] __initconst = {
	{.compatible = "allwinner,sun4i-axi-gates-clk", .data = &sun4i_axi_gates_data,},
	{.compatible = "allwinner,sun4i-ahb-gates-clk", .data = &sun4i_ahb_gates_data,},
	{.compatible = "allwinner,sun5i-a10s-ahb-gates-clk", .data = &sun5i_a10s_ahb_gates_data,},
	{.compatible = "allwinner,sun5i-a13-ahb-gates-clk", .data = &sun5i_a13_ahb_gates_data,},
	{.compatible = "allwinner,sun6i-a31-ahb1-gates-clk", .data = &sun6i_a31_ahb1_gates_data,},
	{.compatible = "allwinner,sun7i-a20-ahb-gates-clk", .data = &sun7i_a20_ahb_gates_data,},
	{.compatible = "allwinner,sun4i-apb0-gates-clk", .data = &sun4i_apb0_gates_data,},
	{.compatible = "allwinner,sun5i-a10s-apb0-gates-clk", .data = &sun5i_a10s_apb0_gates_data,},
	{.compatible = "allwinner,sun5i-a13-apb0-gates-clk", .data = &sun5i_a13_apb0_gates_data,},
	{.compatible = "allwinner,sun7i-a20-apb0-gates-clk", .data = &sun7i_a20_apb0_gates_data,},
	{.compatible = "allwinner,sun4i-apb1-gates-clk", .data = &sun4i_apb1_gates_data,},
	{.compatible = "allwinner,sun5i-a10s-apb1-gates-clk", .data = &sun5i_a10s_apb1_gates_data,},
	{.compatible = "allwinner,sun5i-a13-apb1-gates-clk", .data = &sun5i_a13_apb1_gates_data,},
	{.compatible = "allwinner,sun6i-a31-apb1-gates-clk", .data = &sun6i_a31_apb1_gates_data,},
	{.compatible = "allwinner,sun7i-a20-apb1-gates-clk", .data = &sun7i_a20_apb1_gates_data,},
	{.compatible = "allwinner,sun6i-a31-apb2-gates-clk", .data = &sun6i_a31_apb2_gates_data,},
	{}
};

static void __init of_sunxi_table_clock_setup_found(
					struct vmm_devtree_node *node,
					const struct vmm_devtree_nodeid *match,
					void *data)
{
	const struct div_data *ddata = match->data;
	void (*setup_function)(struct vmm_devtree_node *, const void *) = data;

	setup_function(node, ddata);
}

static void __init of_sunxi_table_clock_setup(const struct vmm_devtree_nodeid *clk_match,
					      void *function)
{
	vmm_devtree_iterate_matching(NULL, clk_match,
				of_sunxi_table_clock_setup_found, function);
}

static void __init sunxi_init_clocks(struct vmm_devtree_node *np)
{
	/* Register factor clocks */
	of_sunxi_table_clock_setup(clk_factors_match, sunxi_factors_clk_setup);

	/* Register divider clocks */
	of_sunxi_table_clock_setup(clk_div_match, sunxi_divider_clk_setup);

	/* Register mux clocks */
	of_sunxi_table_clock_setup(clk_mux_match, sunxi_mux_clk_setup);

	/* Register gate clocks */
	of_sunxi_table_clock_setup(clk_gates_match, sunxi_gates_clk_setup);
}
CLK_OF_DECLARE(sun4i_a10_clk_init, "allwinner,sun4i-a10", sunxi_init_clocks);
CLK_OF_DECLARE(sun5i_a10s_clk_init, "allwinner,sun5i-a10s", sunxi_init_clocks);
CLK_OF_DECLARE(sun5i_a13_clk_init, "allwinner,sun5i-a13", sunxi_init_clocks);
CLK_OF_DECLARE(sun6i_a31_clk_init, "allwinner,sun6i-a31", sunxi_init_clocks);
CLK_OF_DECLARE(sun7i_a20_clk_init, "allwinner,sun7i-a20", sunxi_init_clocks);
