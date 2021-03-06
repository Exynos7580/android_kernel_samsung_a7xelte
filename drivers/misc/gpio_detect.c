/*

 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.


 */

#include <linux/module.h>

#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of.h>
#include <linux/i2c-gpio.h>
#include <asm/gpio.h>

/* GPIO STATUS */
#define GPIO_LOW	0	/* Low */ /*OPEN, 2CA*/
#define GPIO_HIGH	1	/* High */ /*VODA, 3CA*/

enum { EUR_OPEN, EUR_VODA };

int gpio_number[1] = {-1};

static int check_subfpcb_type(struct seq_file *m, void *v)
{
	int retval, support_type_of_subfpcb;

	printk("\n SUBFPCB_TYPE_PIN : %d\n", gpio_number[0]);  //temp log for checking GPIO Setting correctly applyed or not

	retval = gpio_request(gpio_number[0], "SUBFPCB_TYPE_PIN");
	if (retval) {
		pr_err("%s:Failed to reqeust GPIO, code = %d.\n",
				__func__, retval);
		support_type_of_subfpcb = retval;
	} 
	else
	{
		retval = gpio_direction_input(gpio_number[0]);

		if (retval){
			pr_err("%s:Failed to set direction of GPIO, code = %d.\n",
					__func__, retval);
			support_type_of_subfpcb = retval;
		}
		else
		{
			retval = gpio_get_value(gpio_number[0]);

			/* This codes are implemented assumption that count of GPIO about simslot is only one on H/W schematic
				 You may change this codes if count of GPIO about simslot has change */
			switch(retval)
			{
				case GPIO_LOW:
					support_type_of_subfpcb = EUR_OPEN;
					break;
				case GPIO_HIGH:
					support_type_of_subfpcb = EUR_VODA;
					break;
				default:
					support_type_of_subfpcb = -1;
					break;
			}
		}
		gpio_free(gpio_number[0]);
	}

	if(support_type_of_subfpcb < 0)
	{
		pr_err("***** WARNING: Could not check subfpcb_type *****\n");
	}

	printk("%s: Type of subfpcb: %u\n", __func__, support_type_of_subfpcb);
	seq_printf(m, "%u\n", support_type_of_subfpcb);

	return 0;

}


static int check_subfpcb_type_open(struct inode *inode, struct file *file)
{
	return single_open(file, check_subfpcb_type, NULL);
}


static const struct file_operations check_subfpcb_type_fops = {
	.open	= check_subfpcb_type_open,
	.read	= seq_read,
	.llseek	= seq_lseek,
	.release = single_release,
};


static int __init gpiodetect_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	gpio_number[0] = of_get_gpio(dev->of_node, 0);
	if (gpio_number[0] < 0) {
		dev_err(dev, "failed to get proper gpio number\n");
		return -EINVAL;
	}

	if (!proc_create("subfpcb_type", 0, NULL,&check_subfpcb_type_fops))
	{
		pr_err("***** WARNING: Could not make a subfpcb_type file node ******\n");
		return -ENOMEM;
	}

	return 0;
}

#if defined(CONFIG_OF)
static struct of_device_id gpiodetect_dt_ids[] = {
	{ .compatible = "gpio_detect" },
	{ },
};
MODULE_DEVICE_TABLE(of, gpiodetect_dt_ids);
#endif /* CONFIG_OF */

static struct platform_driver gpiodetect_device_driver = {
	.probe		= gpiodetect_probe,
	.driver		= {
		.name	= "gpio_detect",
		.owner	= THIS_MODULE,
#if defined(CONFIG_OF)
		.of_match_table = gpiodetect_dt_ids,
#endif /* CONFIG_OF */
	}
};

static int __init gpio_detect_init(void)
{
	printk("%s start\n", __func__);
	return platform_driver_register(&gpiodetect_device_driver);
}

static void __exit gpio_detect_exit(void)
{
	printk("%s start\n", __func__);
	platform_driver_unregister(&gpiodetect_device_driver);
}

late_initcall(gpio_detect_init);
module_exit(gpio_detect_exit)
