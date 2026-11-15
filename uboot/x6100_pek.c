// SPDX-License-Identifier: GPL-2.0+
/*
 * pek: the X6100's POWER key (the AXP223 PMIC's PEK) in the boot menu.
 *
 * The AXP223 latches a short press of its key in IRQ status register 3 (0x4A) bit 1 and a long
 * one in bit 0 (Linux's drivers/mfd/axp20x.c: AXP22X PEK_SHORT 2,1 and PEK_LONG 2,0); writing 1
 * clears a bit. The two are enabled in IRQ enable register 3 (0x42) so that they latch.
 *
 *   pek clear   clears both (the press that switched the radio on); always true
 *   pek         true once after a short press since the last call
 */
#include <command.h>
#include <asm/gpio.h>
#include <asm/arch/pmic_bus.h>
#include <linux/delay.h>

#define AXP_IRQ_ENABLE3 0x42
#define AXP_IRQ_STATUS3 0x4a
#define PEK_SHORT       0x02
#define PEK_LONG        0x01

static int do_pek(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	u8 st;

	if (argc > 1) {
		pmic_bus_setbits(AXP_IRQ_ENABLE3, PEK_SHORT | PEK_LONG);
		pmic_bus_write(AXP_IRQ_STATUS3, PEK_SHORT | PEK_LONG);
		return CMD_RET_SUCCESS;
	}
	if (pmic_bus_read(AXP_IRQ_STATUS3, &st))
		return CMD_RET_FAILURE;
	if (st & (PEK_SHORT | PEK_LONG))
		pmic_bus_write(AXP_IRQ_STATUS3, st & (PEK_SHORT | PEK_LONG));
	return (st & PEK_SHORT) ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(pek, 2, 0, do_pek, "X6100 POWER key: true once after a short press",
	   "        - true if the key was pressed since the last call\npek clear  - forget earlier presses");

/*
 * boff: the radio switched off the way its own software does it (X6100Control's
 * x6100_control_poweroff): the base board's register 12 with its power-off bit 0x10000. The base
 * board holds the radio's power: the PMIC's power-off alone left the radio on. The base board sits
 * at 0x72 on the X6100's bit-banged I2C (SCL PH2, SDA PH3, open drain); a write is the register's
 * byte address (register * 4) high byte first, then the 32-bit value low byte first.
 */
static struct gpio_desc scl, sda;

static void line(struct gpio_desc *g, int high)
{
	/* open drain: high is let go (the pull-up), low is driven */
	if (high)
		dm_gpio_set_dir_flags(g, GPIOD_IS_IN);
	else
		dm_gpio_set_dir_flags(g, GPIOD_IS_OUT);       /* GPIOD_IS_OUT drives 0 */
	udelay(5);
}

static void scl_high(void)
{
	int i;

	line(&scl, 1);
	for (i = 0; i < 1000 && !dm_gpio_get_value(&scl); i++)   /* the slave may hold the clock */
		udelay(5);
}

static int put_byte(u8 b)
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		line(&sda, (b >> i) & 1);
		scl_high();
		line(&scl, 0);
	}
	line(&sda, 1);
	scl_high();
	ack = !dm_gpio_get_value(&sda);
	line(&scl, 0);
	return ack;
}

static int base_write(unsigned reg, u32 val)
{
	u16 addr = reg * 4;
	u8 buf[7] = {0x72 << 1, addr >> 8, addr, val, val >> 8, val >> 16, val >> 24};
	int i, ok = 1;

	line(&sda, 1);                                       /* start */
	scl_high();
	line(&sda, 0);
	line(&scl, 0);
	for (i = 0; i < 7 && ok; i++)
		ok = put_byte(buf[i]);
	line(&sda, 0);                                       /* stop */
	scl_high();
	line(&sda, 1);
	return ok;
}

static int do_boff(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	int ok;

	if (dm_gpio_lookup_name("PH2", &scl) || dm_gpio_lookup_name("PH3", &sda))
		return CMD_RET_FAILURE;
	if (dm_gpio_request(&scl, "base-scl") || dm_gpio_request(&sda, "base-sda"))
		return CMD_RET_FAILURE;
	ok = base_write(12, 0x10000);
	dm_gpio_free(scl.dev, &scl);
	dm_gpio_free(sda.dev, &sda);
	if (!ok)
		printf("boff: the base board did not answer\n");
	return ok ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

U_BOOT_CMD(boff, 1, 0, do_boff, "X6100: the base board switches the radio off",
	   "- the base board's power-off (register 12, bit 0x10000), as the radio's own software does");
