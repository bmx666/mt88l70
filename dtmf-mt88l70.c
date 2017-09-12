/*
 * Copyright (C) 2017 Adakta Ltd
 *
 * Author: Maxim Paymushkin <maxim.paymushkin@gmail.com>
 *
 * License Terms: GNU General Public License, version 2
 *
 * MT88L70 DTMF driver
 */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/input/matrix_keypad.h>

#define MT88L70_GPIO_NUM 4

/**
 * enum mt88l70_version - indicates the MT88L70 version
 */
enum mt88l70_version {
	MT88L70,
};

/**
 * struct mt88l70_dtmf - platform specific dtmf data
 * @keymap_data:		matrix scan code table for keycodes
 * @dev:				pointer to device
 * @input:				pointer to input device object
 * @std_active_low:		for set irq trigger edge
 * @irq:				irq issued by device
 * @gpios:				gpios for states to keycode
 * @gpios_active_low:	gpios active level
 * @keymap:				matrix scan code table for keycodes
 */
struct mt88l70_dtmf {
	const struct matrix_keymap_data *keymap_data;
	struct device *dev;
	struct input_dev *input;
	enum of_gpio_flags std_active_low;
	int irq;
	int gpios[MT88L70_GPIO_NUM];
	enum of_gpio_flags gpios_active_low[MT88L70_GPIO_NUM];
	unsigned short *keymap;
};

static irqreturn_t mt88l70_irq(int irq, void *dev)
{
	struct mt88l70_dtmf *dtmf = dev;
	int i, val;
	u8 code;

	code = 0;
	for (i = 0; i < MT88L70_GPIO_NUM; ++i) {
		if (!gpio_is_valid(dtmf->gpios[i]))
			continue;
		val = gpio_get_value(dtmf->gpios[i]);
		val &= 1;
		if (dtmf->gpios_active_low[i])
			val ^= val;
		val <<= i;
		code |= val;
	}

	input_event(dtmf->input, EV_MSC, MSC_SCAN, code);
	input_report_key(dtmf->input, dtmf->keymap[code], 1);
	input_report_key(dtmf->input, dtmf->keymap[code], 0);
	input_sync(dtmf->input);

	return IRQ_HANDLED;
}

static int mt88l70_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct mt88l70_dtmf *dtmf;
	struct input_dev *input;
	int i, ret;
	int gpio;
	int proplen;
	unsigned int rows, cols;

	if (!np)
		return -ENODEV;

	if (of_gpio_named_count(np, "q-gpios") != MT88L70_GPIO_NUM) {
		dev_err(&pdev->dev, "property %s must contain %d gpios\n",
					"q-gpios", MT88L70_GPIO_NUM);
		return -ENOENT;
	}

	if (!of_get_property(np, "linux,keymap", &proplen)) {
		dev_err(&pdev->dev, "property linux,keymap not found\n");
		return -ENOENT;
	}

	dtmf = devm_kzalloc(&pdev->dev, sizeof(struct mt88l70_dtmf),
			      GFP_KERNEL);
	if (!dtmf) {
		dev_err(&pdev->dev, "failed to allocate device\n");
		return -ENOMEM;
	}

	gpio = of_get_named_gpio_flags(np,
				"std-gpios", 0, &dtmf->std_active_low);
	if (!gpio_is_valid(gpio)) {
		dev_err(&pdev->dev, "invalid std gpio, error %d\n", gpio);
		return gpio;
	}

	dtmf->irq = gpio_to_irq(gpio);
	irq_set_status_flags(dtmf->irq,
				dtmf->std_active_low
					? IRQ_TYPE_EDGE_FALLING
					: IRQ_TYPE_EDGE_RISING);

	for (i = 0; i < MT88L70_GPIO_NUM; ++i) {
		gpio = of_get_named_gpio_flags(np,
					"q-gpios", i, &dtmf->gpios_active_low[i]);
		if (!gpio_is_valid(gpio)) {
			dev_warn(&pdev->dev, "invalid Q%d gpio, error %d\n", i, gpio);
			continue;
		}
		ret = gpio_direction_input(gpio);
		if (ret) {
			dev_err(&pdev->dev, "failed to set input direction"
						" for Q%d gpio, error %d\n", i, ret);
			return ret;
		}
		dtmf->gpios[i] = gpio;
	}

	input = devm_input_allocate_device(&pdev->dev);
	if (!input) {
		dev_err(&pdev->dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	dtmf->dev = &pdev->dev;
	dtmf->input = input;

	input->id.bustype = BUS_HOST;
	input->name = pdev->name;
	input->dev.parent = &pdev->dev;

	ret = matrix_keypad_parse_of_params(&pdev->dev, &rows, &cols);
	if (ret)
		return ret;

	ret = matrix_keypad_build_keymap(dtmf->keymap_data, NULL,
					   rows, cols, NULL, input);
	if (ret) {
		dev_err(&pdev->dev, "Failed to build keymap\n");
		return ret;
	}

	dtmf->keymap = input->keycode;

	input_set_capability(input, EV_MSC, MSC_SCAN);
	input_set_drvdata(input, dtmf);

	ret = devm_request_threaded_irq(&pdev->dev, dtmf->irq,
				mt88l70_irq, NULL,
				dtmf->std_active_low
					? IRQF_TRIGGER_FALLING
					: IRQF_TRIGGER_RISING,
				"mt88l70-dtmf", dtmf);
	if (ret) {
		dev_err(&pdev->dev,
				"Could not allocate irq %d, error %d\n",
				dtmf->irq, ret);
		return ret;
	}

	ret = input_register_device(input);
	if (ret) {
		dev_err(&pdev->dev, "Could not register input device\n");
		return ret;
	}

	platform_set_drvdata(pdev, dtmf);

	return 0;
}

#ifdef CONFIG_PM
static int mt88l70_suspend(struct device *dev)
{
	struct mt88l70_dtmf *dtmf = dev_get_drvdata(dev);

	disable_irq(dtmf->irq);

	if (device_may_wakeup(dtmf->dev))
		enable_irq_wake(dtmf->irq);

	return 0;
}

static int mt88l70_resume(struct device *dev)
{
	struct mt88l70_dtmf *dtmf = dev_get_drvdata(dev);

	if (device_may_wakeup(dtmf->dev))
		disable_irq_wake(dtmf->irq);

	enable_irq(dtmf->irq);

	return 0;
}

static const struct dev_pm_ops mt88l70_dev_pm_ops = {
	.suspend = mt88l70_suspend,
	.resume  = mt88l70_resume,
};
#endif

static const struct of_device_id mt88l70_match[] = {
	/* Legacy compatible string */
	{ .compatible = "microsemi,mt88l70", .data = (void *) MT88L70 },
	{ }
};
MODULE_DEVICE_TABLE(of, mt88l70_match);

static struct platform_driver mt88l70_driver = {
	.driver	= {
		.name			= "mt88l70",
		.owner			= THIS_MODULE,
		.of_match_table	= of_match_ptr(mt88l70_match),
#ifdef CONFIG_PM
		.pm				= &mt88l70_dev_pm_ops,
#endif
	},
	.probe	= mt88l70_probe,
};
module_platform_driver(mt88l70_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MT88L70 DTMF driver");
MODULE_AUTHOR("Maxim Paymushkin");
