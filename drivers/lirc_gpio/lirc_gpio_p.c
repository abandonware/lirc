/*
 * Remote control driver for the TV-card
 * key codes are obtained from GPIO port
 * 
 * (L) by Artur Lipowski <lipowski@comarch.pl>
 *     patch for the AverMedia by Santiago Garcia Mantinan <manty@i.am>
 *                            and Christoph Bartelmus <lirc@bartelmus.de>
 * This code is licensed under GNU GPL
 *
 * $Id: lirc_gpio_p.c,v 1.5 2000/05/10 19:03:02 columbus Exp $
 *
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 2, 4)
#error "!!! Sorry, this driver needs kernel version 2.2.4 or higher !!!"
#endif

#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/wrapper.h>

#include "../lirc_dev/lirc_dev.h"
#include "../drivers/char/bttv.h"

/* default parameters value are suitable for the PixelView Play TVPro card
 */

static int debug = 0;
static int card = 0;
static int minor = -1;
static unsigned long gpio_mask = 0;
static unsigned long gpio_lock_mask = 0;
static unsigned long gpio_xor_mask = 0;
static unsigned int soft_gap = 0;
static unsigned char sample_rate = 10;

MODULE_PARM(debug,"i");
MODULE_PARM(card,"i");
MODULE_PARM(minor,"i");
MODULE_PARM(gpio_mask,"l");
MODULE_PARM(gpio_lock_mask,"l");
MODULE_PARM(gpio_xor_mask,"l");
MODULE_PARM(soft_gap,"i");
MODULE_PARM(sample_rate,"b");

#define dprintk  if (debug) printk

struct rcv_info {
	int bttv_id;
	unsigned long gpio_mask;
	unsigned long gpio_lock_mask;
	unsigned long gpio_xor_mask;
	unsigned int soft_gap;
	unsigned char sample_rate;
	unsigned char code_length;
};

static struct rcv_info rcv_infos[] = {
	{BTTV_UNKNOWN, 0, 0, 0, 0, 1, 0},
#ifdef BTTV_PXELVWPLTVPRO
	{BTTV_PXELVWPLTVPRO, 0x1f00, 0x8000, 0, 500, 12, 0},
#endif
#ifdef BTTV_AVERMEDIA
	{BTTV_AVERMEDIA, 0x0f88000, 0x010000, 0x010000, 0, 10, 0},
#endif
#ifdef BTTV_AVERMEDIA98
	{BTTV_AVERMEDIA98, 0x0f88000, 0x010000, 0x010000, 0, 10, 32}
#endif
};

static unsigned char code_length = 0;
static unsigned char code_bytes = 1;
static int card_type = 0;

#define MAX_BYTES 8

/* how many bits GPIO value can be shifted right before processing
 * it is computed from the value of gpio_mask_parameter
 */
static unsigned char gpio_pre_shift = 0;


static int build_key(unsigned long gpio_val, unsigned char codes[MAX_BYTES])
{
	unsigned long mask = gpio_mask;
	unsigned char shift = 0;

	gpio_val ^= gpio_xor_mask;
	
	if (gpio_lock_mask && (gpio_val & gpio_lock_mask)) {
		return -1;
	}

	switch(rcv_infos[card_type].bttv_id)
	{
	case BTTV_AVERMEDIA98:
		if(gpio_val&0x8000) {
			codes[0] = 0xc0;
			codes[1] = 0x3f;
		} else {
			codes[0] = 0x40;
			codes[1] = 0xbf;
		}
		gpio_val &= gpio_mask;
		gpio_val >>= 16;
		codes[2] = gpio_val   & 0xff;
		codes[3] = ~(gpio_val)& 0xff;
		break;

	default:
		/* extract bits from "raw" GPIO value using gpio_mask */
		codes[0] = 0;
		gpio_val >>= gpio_pre_shift;
		while (mask) {
			if (mask & 1u) {
				codes[0] |= (gpio_val & 1u) << shift++;
			}
			mask >>= 1;
			gpio_val >>= 1;
		}
		break;
	}

	return 0;
}

static int get_key(void* data, unsigned char *key, int key_no)
{
	static unsigned long next_time = 0;
	static unsigned char codes[MAX_BYTES];
	unsigned long code = 0;
	unsigned char cur_codes[MAX_BYTES];
	
	if (key_no > 0)
	{
		if (code_bytes < 2 || key_no >= code_bytes) {
			dprintk("lirc_gpio_p %d: something wrong in get_key\n",
				card);
			return -1;
		}
		*key=codes[key_no];
		return 0;
	}
	
	if (bttv_read_gpio(card, &code)) {
		dprintk("lirc_gpio_p %d: cannot read GPIO\n", card);
		return -1;
	}

	if (build_key(code, cur_codes)) {
		return -1;
	}

	if (soft_gap) {
		if (!memcmp(codes, cur_codes, code_bytes) && jiffies < next_time) {
			return -1;
		}
		next_time = jiffies + soft_gap;
	}

	memcpy(codes, cur_codes, code_bytes);

	*key = codes[0];

	return 0;
}

static void set_use_inc(void* data)
{
	MOD_INC_USE_COUNT;
}

static void set_use_dec(void* data)
{
	MOD_DEC_USE_COUNT;
}

static struct lirc_plugin plugin = {
	"lirc_gpio_p",
	0,
	0,
	0,
	NULL,
	get_key,
	NULL,
	set_use_inc,
	set_use_dec
};

/*
 *
 */
int gpio_remote_init(void)
{  	
	int ret;
	unsigned int mask;

	/* "normalize" gpio_mask
	 * this means shift it right until first bit is set
	 */
	while (!(gpio_mask & 1u)) {
		gpio_pre_shift++;
		gpio_mask >>= 1;
	}

	if (code_length) {
		plugin.code_length = code_length;
	} else {
		/* calculte scan code length in bits if needed*/
		plugin.code_length = 1;
		mask = gpio_mask >> 1;
		while(mask) {
			if (mask & 1u) {
				plugin.code_length++;
			}
			mask >>= 1;
		}
	}

	code_bytes = (plugin.code_length/8) + (plugin.code_length%8 ? 1 : 0);
	if (MAX_BYTES < code_bytes) {
		printk ("lirc_gpio_p %d: scan code too long (%d bytes)\n",
			minor, code_bytes);
		return -1;
	}

	/* translate ms to jiffies */
	soft_gap = (soft_gap*HZ) / 1000;

	plugin.minor = minor;
	plugin.sample_rate = sample_rate;

	ret = lirc_register_plugin(&plugin);
	
	if (0 > ret) {
		printk ("lirc_gpio_p %d: device registration failed with %d\n",
			minor, ret);
		return ret;
	}
	
	minor = ret;
	printk("lirc_gpio_p %d: driver registered\n", minor);

	return 0;
}

EXPORT_NO_SYMBOLS; 

/* Dont try to use it as a static version !  */

#ifdef MODULE
MODULE_DESCRIPTION("Driver module for remote control (data from bt848 GPIO port)");
MODULE_AUTHOR("Artur Lipowski");

/*
 *
 */
int init_module(void)
{
	int type;

	if (MAX_IRCTL_DEVICES < minor) {
		printk("lirc_gpio_p: parameter minor (%d) must be lesst han %d!\n",
		       minor, MAX_IRCTL_DEVICES-1);
		return -1;
	}
	
	request_module("bttv");

	/* if gpio_mask not zero then use module parameters 
	 * instead of autodetecting TV card
	 */
	if (gpio_mask) {
		if (2 > sample_rate || 50 < sample_rate) {
			printk("lirc_gpio_p %d: parameter sample_rate "
			       "must be beetween 2 and 50!\n", minor);
			return -1;
		}

		if (soft_gap && ((2000/sample_rate) > soft_gap || 1000 < soft_gap)) {
			printk("lirc_gpio_p %d: parameter soft_gap "
			       "must be beetween %d and 1000!\n",
			       minor, 2000/sample_rate);
			return -1;
		}
	} else {
		type = bttv_get_id(card);

		if (type == BTTV_UNKNOWN) {
			printk("lirc_gpio_p %d: cannot detect TV card nr %d!\n",
			       minor, card);
			return -1;
		}
		for (card_type=1; 
		     card_type < sizeof(rcv_infos)/sizeof(struct rcv_info); 
		     card_type++) {
			if (rcv_infos[card_type].bttv_id == type) {
				gpio_mask = rcv_infos[card_type].gpio_mask;
				gpio_lock_mask = rcv_infos[card_type].gpio_lock_mask;
				gpio_xor_mask = rcv_infos[card_type].gpio_xor_mask;
				soft_gap = rcv_infos[card_type].soft_gap;
				sample_rate = rcv_infos[card_type].sample_rate;
				code_length = rcv_infos[card_type].code_length;
				break;
			}
		}
		if (card_type == sizeof(rcv_infos)/sizeof(struct rcv_info)) {
			printk("lirc_gpio_p %d: TV card type %x not supported!\n",
			       minor, type);
			return -1;
		}
	}

	request_module("lirc_dev");

	return gpio_remote_init();
}

/*
 *
 */
void cleanup_module(void)
{
	int ret;

	ret = lirc_unregister_plugin(minor);
 
	if (0 > ret) {
		printk("lirc_gpio_p %d: error in lirc_unregister_minor: %d\n"
		       "Trying again...\n",
		       minor, ret);

		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ);

		ret = lirc_unregister_plugin(minor);
 
		if (0 > ret) {
			printk("lirc_gpio_p %d: error in lirc_unregister_minor: %d!!!\n",
			       minor, ret);
			return;
		}
	}

	dprintk("lirc_gpio_p %d: module successfully unloaded\n", minor);
}
#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */