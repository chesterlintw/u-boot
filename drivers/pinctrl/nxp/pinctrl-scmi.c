// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2022-2023 NXP
 */

#include <common.h>
#include <dm.h>
#include <scmi.h>
#include <asm/gpio.h>
#include <dm/pinctrl.h>

#include <malloc.h>
#include <linux/bitmap.h>
#include <linux/list.h>

#include <sort.h>

#define SCMI_PINCTRL_NUM_RANGES_MASK 0xFFFF

#define SCMI_PINCTRL_PIN_FROM_PINMUX(v) ((v) >> 4)
#define SCMI_PINCTRL_FUNC_FROM_PINMUX(v) ((v) & 0XF)

/* 128 (channel size) - 28 (SMT header) - 8 (extra space). */
#define SCMI_MAX_BUFFER_SIZE 92

#define PACK_CFG(p, a)		(((p) & 0xFF) | ((a) << 8))
#define UNPACK_PARAM(packed)	((packed) & 0xFF)
#define UNPACK_ARG(packed)	((packed) >> 8)

enum scmi_pinctrl_msg_id {
	SCMI_PINCTRL_PROTOCOL_ATTRIBUTES = 0x1,
	SCMI_PINCTRL_DESCRIBE = 0x3,
	SCMI_PINCTRL_PINMUX_GET = 0x4,
	SCMI_PINCTRL_PINMUX_SET = 0x5,
	SCMI_PINCTRL_PINCONF_GET = 0x6,
	SCMI_PINCTRL_PINCONF_SET_OVR = 0x7,
	SCMI_PINCTRL_PINCONF_SET_APP = 0x8,
};

struct scmi_pinctrl_range {
	u32 begin;
	u32 num_pins;
};

struct scmi_pinctrl_priv {
	struct scmi_pinctrl_range *ranges;
	struct list_head gpio_configs;
	unsigned int num_ranges;
};

struct scmi_pinctrl_pin_cfg {
	u8 num_configs;
	u8 allocated;
	u32 *configs;
};

struct scmi_pinctrl_saved_pin {
	u16 pin;
	u16 func;
	struct scmi_pinctrl_pin_cfg cfg;
	struct list_head list;
};

struct scmi_pinctrl_pinconf_resp {
	s32 status;
	u32 mask;
	u32 boolean_values;
	u32 multi_bit_values[];
};

static const struct pinconf_param scmi_pinctrl_pinconf_params[] = {
	{ "bias-pull-up", PIN_CONFIG_BIAS_PULL_UP, 1 },
	{ "bias-pull-down", PIN_CONFIG_BIAS_PULL_DOWN, 1 },
	{ "bias-disable", PIN_CONFIG_BIAS_DISABLE, 1 },
	{ "input-enable", PIN_CONFIG_INPUT_ENABLE, 1 },
	{ "input-disable", PIN_CONFIG_INPUT_ENABLE, 0 },
	{ "output-enable", PIN_CONFIG_OUTPUT_ENABLE, 1 },
	{ "output-disable", PIN_CONFIG_OUTPUT_ENABLE, 0 },
	{ "slew-rate", PIN_CONFIG_SLEW_RATE, 4 },
	{ "drive-open-drain", PIN_CONFIG_DRIVE_OPEN_DRAIN, 1 },
	{ "drive-push-pull", PIN_CONFIG_DRIVE_PUSH_PULL, 1 },
};

enum converted_pin_param {
	CONV_PIN_CONFIG_BIAS_BUS_HOLD = 0,
	CONV_PIN_CONFIG_BIAS_DISABLE,
	CONV_PIN_CONFIG_BIAS_HIGH_IMPEDANCE,
	CONV_PIN_CONFIG_BIAS_PULL_DOWN,
	CONV_PIN_CONFIG_BIAS_PULL_PIN_DEFAULT,
	CONV_PIN_CONFIG_BIAS_PULL_UP,
	CONV_PIN_CONFIG_DRIVE_OPEN_DRAIN,
	CONV_PIN_CONFIG_DRIVE_OPEN_SOURCE,
	CONV_PIN_CONFIG_DRIVE_PUSH_PULL,
	CONV_PIN_CONFIG_DRIVE_STRENGTH,
	CONV_PIN_CONFIG_DRIVE_STRENGTH_UA,
	CONV_PIN_CONFIG_INPUT_DEBOUNCE,
	CONV_PIN_CONFIG_INPUT_ENABLE,
	CONV_PIN_CONFIG_INPUT_SCHMITT,
	CONV_PIN_CONFIG_INPUT_SCHMITT_ENABLE,
	CONV_PIN_CONFIG_MODE_LOW_POWER,
	CONV_PIN_CONFIG_MODE_PWM,
	CONV_PIN_CONFIG_OUTPUT,
	CONV_PIN_CONFIG_OUTPUT_ENABLE,
	CONV_PIN_CONFIG_PERSIST_STATE,
	CONV_PIN_CONFIG_POWER_SOURCE,
	CONV_PIN_CONFIG_SKEW_DELAY,
	CONV_PIN_CONFIG_SLEEP_HARDWARE_STATE,
	CONV_PIN_CONFIG_SLEW_RATE,

	CONV_PIN_CONFIG_NUM_CONFIGS,

	CONV_PIN_CONFIG_ERROR,
};

static const u32 scmi_pinctrl_multi_bit_cfgs =
	BIT_32(CONV_PIN_CONFIG_SLEW_RATE) |
	BIT_32(CONV_PIN_CONFIG_SKEW_DELAY) |
	BIT_32(CONV_PIN_CONFIG_POWER_SOURCE) |
	BIT_32(CONV_PIN_CONFIG_MODE_LOW_POWER) |
	BIT_32(CONV_PIN_CONFIG_INPUT_SCHMITT) |
	BIT_32(CONV_PIN_CONFIG_INPUT_DEBOUNCE) |
	BIT_32(CONV_PIN_CONFIG_DRIVE_STRENGTH_UA) |
	BIT_32(CONV_PIN_CONFIG_DRIVE_STRENGTH);

static const enum converted_pin_param scmi_pinctrl_convert[] = {
	[PIN_CONFIG_BIAS_BUS_HOLD] = CONV_PIN_CONFIG_BIAS_BUS_HOLD,
	[PIN_CONFIG_BIAS_DISABLE] = CONV_PIN_CONFIG_BIAS_DISABLE,
	[PIN_CONFIG_BIAS_HIGH_IMPEDANCE] = CONV_PIN_CONFIG_BIAS_HIGH_IMPEDANCE,
	[PIN_CONFIG_BIAS_PULL_DOWN] = CONV_PIN_CONFIG_BIAS_PULL_DOWN,
	[PIN_CONFIG_BIAS_PULL_PIN_DEFAULT] =
		CONV_PIN_CONFIG_BIAS_PULL_PIN_DEFAULT,
	[PIN_CONFIG_BIAS_PULL_UP] = CONV_PIN_CONFIG_BIAS_PULL_UP,
	[PIN_CONFIG_DRIVE_OPEN_DRAIN] = CONV_PIN_CONFIG_DRIVE_OPEN_DRAIN,
	[PIN_CONFIG_DRIVE_OPEN_SOURCE] = CONV_PIN_CONFIG_DRIVE_OPEN_SOURCE,
	[PIN_CONFIG_DRIVE_PUSH_PULL] = CONV_PIN_CONFIG_DRIVE_PUSH_PULL,
	[PIN_CONFIG_DRIVE_STRENGTH] = CONV_PIN_CONFIG_DRIVE_STRENGTH,
	[PIN_CONFIG_DRIVE_STRENGTH_UA] = CONV_PIN_CONFIG_DRIVE_STRENGTH_UA,
	[PIN_CONFIG_INPUT_DEBOUNCE] = CONV_PIN_CONFIG_INPUT_DEBOUNCE,
	[PIN_CONFIG_INPUT_ENABLE] = CONV_PIN_CONFIG_INPUT_ENABLE,
	[PIN_CONFIG_INPUT_SCHMITT] = CONV_PIN_CONFIG_INPUT_SCHMITT,
	[PIN_CONFIG_INPUT_SCHMITT_ENABLE] =
		CONV_PIN_CONFIG_INPUT_SCHMITT_ENABLE,
	[PIN_CONFIG_LOW_POWER_MODE] = CONV_PIN_CONFIG_MODE_LOW_POWER,
	[PIN_CONFIG_OUTPUT_ENABLE] = CONV_PIN_CONFIG_OUTPUT_ENABLE,
	[PIN_CONFIG_OUTPUT] = CONV_PIN_CONFIG_OUTPUT,
	[PIN_CONFIG_POWER_SOURCE] = CONV_PIN_CONFIG_POWER_SOURCE,
	[PIN_CONFIG_SLEEP_HARDWARE_STATE] =
		CONV_PIN_CONFIG_SLEEP_HARDWARE_STATE,
	[PIN_CONFIG_SLEW_RATE] = CONV_PIN_CONFIG_SLEW_RATE,
	[PIN_CONFIG_SKEW_DELAY] = CONV_PIN_CONFIG_SKEW_DELAY,
};

static int scmi_pinctrl_add_config(u32 config, struct scmi_pinctrl_pin_cfg *cfg)
{
	void *temp;

	if (cfg->num_configs > CONV_PIN_CONFIG_NUM_CONFIGS)
		return -EINVAL;

	if (cfg->allocated <= cfg->num_configs) {
		cfg->allocated = (u8)2 * cfg->num_configs + 1;
		temp = realloc(cfg->configs, cfg->allocated * sizeof(u32));
		if (!temp)
			return -ENOMEM;
		cfg->configs = temp;
	}

	cfg->configs[cfg->num_configs++] = config;

	return 0;
}

static bool scmi_pinctrl_is_multi_bit_value(enum converted_pin_param p)
{
	return !!(BIT_32(p) & scmi_pinctrl_multi_bit_cfgs);
}

static int scmi_pinctrl_compare_cfgs(const void *a, const void *b)
{
	s32 pa, pb;

	pa = UNPACK_PARAM(*(s32 *)a);
	pb = UNPACK_PARAM(*(s32 *)b);

	return pb - pa;
}

static int scmi_pinctrl_set_configs(struct udevice *scmi_dev, unsigned int pin,
				    struct scmi_pinctrl_pin_cfg *cfg)
{
	u8 buffer[SCMI_MAX_BUFFER_SIZE];
	enum converted_pin_param p;
	struct {
		u16 pin;
		u32 mask;
		u32 boolean_values;
		u32 multi_bit_values[];
	} *r = (void *)buffer;
	struct {
		s32 status;
	} response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PINCONF_SET_OVR,
		.in_msg		= buffer,
		.in_msg_sz	= sizeof(buffer),
		.out_msg	= (u8 *)&response,
		.out_msg_sz	= sizeof(response),
	};
	int ret, i;
	u32 param, arg, index = 0, max_mb_elems;

	max_mb_elems =
		(sizeof(buffer) - sizeof(*r)) / sizeof(r->multi_bit_values[0]);

	memset(buffer, 0, ARRAY_SIZE(buffer));

	if (pin > U16_MAX)
		return -EINVAL;

	r->pin = (u16)pin;
	r->mask = 0;
	r->boolean_values = 0;

	/* Sorting needs to be done in order to lay out
	 * the configs in descending order of their
	 * pinconf parameter value which matches
	 * the protocol specification.
	 */
	qsort(cfg->configs, cfg->num_configs, sizeof(cfg->configs[0]),
	      scmi_pinctrl_compare_cfgs);

	for (i = 0; i < cfg->num_configs; ++i) {
		param = UNPACK_PARAM(cfg->configs[i]);
		arg = UNPACK_ARG(cfg->configs[i]);

		r->mask |= BIT_32(param);

		if (param > CONV_PIN_CONFIG_NUM_CONFIGS)
			return -EINVAL;

		p = (enum converted_pin_param)param;

		if (index >= max_mb_elems)
			return -EINVAL;

		if (scmi_pinctrl_is_multi_bit_value(p))
			r->multi_bit_values[index++] = arg;
		else
			r->boolean_values |= arg ? (u32)BIT_32(param) : 0;
	}

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		pr_err("Error setting pin_config: %d!\n", ret);
		return ret;
	}

	ret = scmi_to_linux_errno(response.status);
	if (ret) {
		pr_err("Error setting pin_config: %d!\n", ret);
		return ret;
	}

	return 0;
}

static int scmi_pinctrl_append_conf(struct udevice *scmi_dev, unsigned int pin,
				    unsigned int param, unsigned int arg)
{
	struct {
		u16 pin;
		u32 mask;
		u32 boolean_values;
		u32 multi_bit_values;
	} request;
	struct {
		s32 status;
	} response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PINCONF_SET_APP,
		.in_msg		= (u8 *)&request,
		.in_msg_sz	= sizeof(request),
		.out_msg	= (u8 *)&response,
		.out_msg_sz	= sizeof(response),
	};
	enum converted_pin_param conv_param;
	int ret;

	if (pin > U16_MAX || param >= CONV_PIN_CONFIG_NUM_CONFIGS)
		return -EINVAL;

	conv_param = (enum converted_pin_param)param;

	request.pin = (u16)pin;
	request.mask = BIT_32(param);
	request.boolean_values = 0;
	request.multi_bit_values = 0;

	if (request.mask >= CONV_PIN_CONFIG_NUM_CONFIGS)
		return -EINVAL;

	if (!scmi_pinctrl_is_multi_bit_value(conv_param)) {
		msg.in_msg_sz -= sizeof(request.multi_bit_values);
		request.boolean_values |= arg << param;
	} else {
		request.multi_bit_values = arg;
	}

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret)
		pr_err("Error getting gpio_mux: %d!\n", ret);

	return ret;
}

static int scmi_pinctrl_set_mux(struct udevice *scmi_dev, u16 pin, u16 func)
{
	struct {
		u8 num_pins;
		u16 pin;
		u16 func;
	} request;
	struct {
		s32 status;
	} response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PINMUX_SET,
		.in_msg		= (u8 *)&request,
		.in_msg_sz	= sizeof(request),
		.out_msg	= (u8 *)&response,
		.out_msg_sz	= sizeof(response),
	};
	int ret;

	request.num_pins = 1;
	request.pin = pin;
	request.func = func;

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		pr_err("Error getting gpio_mux: %d!\n", ret);
		return ret;
	}

	ret = scmi_to_linux_errno(response.status);
	if (ret)
		pr_err("Error getting gpio_mux: %d!\n", ret);

	return ret;
}

static int scmi_pinctrl_push_back_configs(u8 *buffer,
					  struct scmi_pinctrl_pin_cfg *cfg)
{
	struct scmi_pinctrl_pinconf_resp *r = (void *)buffer;
	unsigned int cfg_idx = 0, bit = sizeof(r->mask) * BITS_PER_BYTE - 1;
	enum converted_pin_param p;
	u32 current_cfg;
	int ret = 0;

	do {
		p = (enum converted_pin_param)bit;
		if (p >= CONV_PIN_CONFIG_NUM_CONFIGS)
			return -EINVAL;

		if (scmi_pinctrl_is_multi_bit_value(p)) {
			if (cfg_idx >= hweight32(scmi_pinctrl_multi_bit_cfgs))
				return -EINVAL;

			current_cfg = PACK_CFG(bit,
					       r->multi_bit_values[cfg_idx++]);
		} else {
			current_cfg = PACK_CFG(bit,
					       r->boolean_values & BIT_32(bit));
		}

		ret = scmi_pinctrl_add_config(current_cfg, cfg);
		if (ret)
			return ret;
	} while (bit-- != 0);

	return ret;
}

static int scmi_pinctrl_get_config(struct udevice *scmi_dev, u16 pin,
				   struct scmi_pinctrl_pin_cfg *cfg)
{
	u8 response_buffer[SCMI_MAX_BUFFER_SIZE];
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PINCONF_GET,
		.in_msg		= (u8 *)&pin,
		.in_msg_sz	= sizeof(pin),
		.out_msg	= (u8 *)response_buffer,
		.out_msg_sz	= ARRAY_SIZE(response_buffer),
	};
	struct scmi_pinctrl_pinconf_resp *r = (void *)response_buffer;
	int ret = 0;

	memset(response_buffer, 0, ARRAY_SIZE(response_buffer));

	cfg->allocated = 0;
	cfg->num_configs = 0;
	cfg->configs = NULL;

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		pr_err("Error getting pin_config: %d!\n", ret);
		goto err;
	}

	ret = scmi_to_linux_errno(r->status);
	if (ret) {
		pr_err("Error getting pin_config: %d!\n", ret);
		goto err;
	}

	ret = scmi_pinctrl_push_back_configs(response_buffer,
					     cfg);
	if (ret)
		pr_err("Error getting pin_config: %d!\n", ret);

err:
	if (ret) {
		free(cfg->configs);
		cfg->configs = NULL;
	}

	return ret;
}

static int scmi_pinctrl_get_mux(struct udevice *scmi_dev, u16 pin, u16 *func)
{
	struct {
		u16 pin;
	} request;
	struct {
		s32 status;
		u16 function;
	} response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PINMUX_GET,
		.in_msg		= (u8 *)&request,
		.in_msg_sz	= sizeof(request),
		.out_msg	= (u8 *)&response,
		.out_msg_sz	= sizeof(response),
	};
	int ret;

	request.pin = pin;

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		pr_err("Error getting gpio_mux: %d!\n", ret);
		return ret;
	}

	ret = scmi_to_linux_errno(response.status);
	if (ret) {
		pr_err("Error getting gpio_mux: %d!\n", ret);
		return ret;
	}

	*func = response.function;

	return 0;
}

static int scmi_pinctrl_app_pinconf_setting(struct udevice *dev,
					    struct ofprop property,
					    struct scmi_pinctrl_pin_cfg *cfg)
{
	enum converted_pin_param param;
	const struct pinconf_param *p;
	const char *pname = NULL;
	unsigned int arg, i;
	const void *value;
	int len = 0;

	value = dev_read_prop_by_prop(&property, &pname, &len);
	if (!value)
		return -EINVAL;

	if (len < 0)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(scmi_pinctrl_pinconf_params); ++i) {
		if (strcmp(pname, scmi_pinctrl_pinconf_params[i].property))
			continue;

		p = &scmi_pinctrl_pinconf_params[i];

		if (len == sizeof(fdt32_t)) {
			arg = fdt32_to_cpu(*(fdt32_t *)value);
		} else if (len == 0) {
			arg = p->default_value;
		} else {
			pr_err("Wrong argument size: %s %d\n", pname, len);
			return -EINVAL;
		}

		if (p->param >= ARRAY_SIZE(scmi_pinctrl_convert))
			return -EINVAL;

		param = scmi_pinctrl_convert[p->param];
		if (param == CONV_PIN_CONFIG_ERROR)
			return -EINVAL;

		return scmi_pinctrl_add_config(PACK_CFG(param, arg), cfg);
	}

	return 0;
}

static int scmi_pinctrl_parse_pinmux_len(struct udevice *dev,
					 struct udevice *config)
{
	int size;

	size = dev_read_size(config, "pinmux");
	if (size < 0)
		return size;
	size /= sizeof(fdt32_t);

	return size;
}

static int scmi_pinctrl_set_state_subnode(struct udevice *dev,
					  struct udevice *config)
{
	struct udevice *scmi_dev = dev->parent;
	struct ofprop property;
	u32 pinmux_value = 0, pin, func;
	struct scmi_pinctrl_pin_cfg cfg;
	int ret = 0, i, len;

	cfg.allocated = 0;
	cfg.num_configs = 0;
	cfg.configs = NULL;

	len = scmi_pinctrl_parse_pinmux_len(dev, config);
	if (len <= 0) {
		/* Not a pinmux node. Skip parsing this. */
		return 0;
	}

	dev_for_each_property(property, config) {
		ret = scmi_pinctrl_app_pinconf_setting(dev, property, &cfg);
		if (ret) {
			pr_err("Could not parse property for: %s!\n", config->name);
			break;
		}
	}

	if (ret)
		goto err;

	for (i = 0; i < len; ++i) {
		ret = dev_read_u32_index(config, "pinmux", i, &pinmux_value);
		if (ret) {
			pr_err("Error reading pinmux index: %d\n", i);
			break;
		}

		pin = SCMI_PINCTRL_PIN_FROM_PINMUX(pinmux_value);
		func = SCMI_PINCTRL_FUNC_FROM_PINMUX(pinmux_value);

		if (pin > U16_MAX || func > U16_MAX) {
			pr_err("Invalid pin or func: %u %u!\n", pin, func);
			ret = -EINVAL;
			break;
		}

		ret = scmi_pinctrl_set_mux(scmi_dev, pin, func);
		if (ret) {
			pr_err("Error setting pinmux: %d!\n", ret);
			break;
		}

		ret = scmi_pinctrl_set_configs(scmi_dev, pin, &cfg);
		if (ret) {
			pr_err("Error setting pinconf: %d!\n", ret);
			break;
		}
	}

err:
	free(cfg.configs);
	return ret;
}

static int scmi_pinctrl_set_state(struct udevice *dev, struct udevice *config)
{
	struct udevice *child = NULL;
	int ret;

	ret = scmi_pinctrl_set_state_subnode(dev, config);
	if (ret) {
		pr_err("Error %d parsing: %s\n", ret, config->name);
		return ret;
	}

	device_foreach_child(child, config) {
		ret = scmi_pinctrl_set_state_subnode(dev, child);
		if (ret)
			return ret;
	}

	return 0;
}

static int scmi_pinctrl_pinmux_set(struct udevice *dev,
				   unsigned int pin_selector,
				   unsigned int func_selector)
{
	struct udevice *scmi_dev = dev->parent;

	if (!scmi_dev)
		return -ENXIO;

	if (pin_selector > U16_MAX || func_selector > U16_MAX)
		return -EINVAL;

	return scmi_pinctrl_set_mux(scmi_dev, (u16)pin_selector,
				    (u16)func_selector);
}

static int scmi_pinctrl_pinconf_set(struct udevice *dev,
				    unsigned int pin_selector,
				    unsigned int p, unsigned int arg)
{
	struct udevice *scmi_dev = dev->parent;

	if (!scmi_dev)
		return -ENXIO;

	if (p >= ARRAY_SIZE(scmi_pinctrl_convert))
		return -EINVAL;

	p = scmi_pinctrl_convert[(enum pin_config_param)p];
	if (p == CONV_PIN_CONFIG_ERROR)
		return -EINVAL;

	return scmi_pinctrl_append_conf(scmi_dev, pin_selector, p, arg);
}

static int scmi_pinctrl_gpio_request_enable(struct udevice *dev,
					    unsigned int pin_selector)
{
	struct scmi_pinctrl_priv *priv = dev_get_priv(dev);
	struct udevice *scmi_dev = dev->parent;
	struct scmi_pinctrl_saved_pin *save;
	struct scmi_pinctrl_pin_cfg cfg;
	u16 function;
	int ret;

	cfg.configs = NULL;

	if (pin_selector > U16_MAX)
		return -EINVAL;

	save = malloc(sizeof(*save));
	if (!save)
		return -ENOMEM;

	ret = scmi_pinctrl_get_mux(scmi_dev, pin_selector, &function);
	if (ret)
		goto err;

	ret = scmi_pinctrl_get_config(scmi_dev, pin_selector, &cfg);
	if (ret)
		goto err;

	save->pin = pin_selector;
	save->func = function;
	save->cfg = cfg;

	ret = scmi_pinctrl_set_mux(scmi_dev, pin_selector, 0);
	if (ret)
		goto err;

	list_add(&save->list, &priv->gpio_configs);

err:
	if (ret) {
		free(cfg.configs);
		free(save);
	}

	return ret;
}

static int scmi_pinctrl_gpio_disable_free(struct udevice *dev,
					  unsigned int pin)
{
	struct scmi_pinctrl_priv *priv = dev_get_priv(dev);
	struct udevice *scmi_dev = dev->parent;
	struct scmi_pinctrl_saved_pin *save, *temp;
	int ret = -EINVAL;

	list_for_each_entry_safe(save, temp, &priv->gpio_configs, list) {
		if (save->pin != pin)
			continue;

		ret = scmi_pinctrl_set_mux(scmi_dev, pin, save->func);
		if (ret)
			break;

		ret = scmi_pinctrl_set_configs(scmi_dev, pin,
					       &save->cfg);
		if (ret)
			break;

		list_del(&save->list);
		free(save->cfg.configs);
		free(save);
		return 0;
	}

	return ret;
}

static int scmi_pinctrl_get_gpio_mux(struct udevice *dev,
				     __maybe_unused int banknum,
				     int index)
{
	struct scmi_pinctrl_pin_cfg cfg;
	struct udevice *scmi_dev = dev->parent;
	u16 function;
	int ret, i;
	u32 param, arg;
	bool output = false, input = false;

	if (index > U16_MAX || index < 0)
		return -EINVAL;

	if (!scmi_dev)
		return -ENXIO;

	ret = scmi_pinctrl_get_mux(scmi_dev, (u16)index, &function);
	if (ret)
		return ret;

	if (function != 0)
		return GPIOF_FUNC;

	ret = scmi_pinctrl_get_config(scmi_dev, (u16)index, &cfg);
	if (ret)
		return ret;

	for (i = 0; i < cfg.num_configs; ++i) {
		param = UNPACK_PARAM(cfg.configs[i]);
		arg = UNPACK_ARG(cfg.configs[i]);

		if (param == CONV_PIN_CONFIG_OUTPUT_ENABLE && arg == 1)
			output = true;
		else if (param == CONV_PIN_CONFIG_INPUT_ENABLE && arg == 1)
			input = true;
	}

	free(cfg.configs);

	if (output)
		return GPIOF_OUTPUT;
	else if (input)
		return GPIOF_INPUT;

	return GPIOF_UNKNOWN;
}

static const struct pinctrl_ops scmi_pinctrl_ops = {
	.set_state		= scmi_pinctrl_set_state,
	.gpio_request_enable	= scmi_pinctrl_gpio_request_enable,
	.gpio_disable_free	= scmi_pinctrl_gpio_disable_free,
	.pinmux_set		= scmi_pinctrl_pinmux_set,
	.pinconf_set		= scmi_pinctrl_pinconf_set,
	.get_gpio_mux		= scmi_pinctrl_get_gpio_mux,
	.pinconf_num_params	= ARRAY_SIZE(scmi_pinctrl_pinconf_params),
	.pinconf_params		= scmi_pinctrl_pinconf_params,
};

static int scmi_pinctrl_get_proto_attr(struct udevice *dev,
				       struct udevice *scmi_dev)
{
	struct scmi_pinctrl_priv *priv = dev_get_priv(dev);
	struct {
		s32 status;
		u32 attributes;
	} response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_PROTOCOL_ATTRIBUTES,
		.in_msg		= NULL,
		.in_msg_sz	= 0,
		.out_msg	= (u8 *)&response,
		.out_msg_sz	= sizeof(response),
	};
	int ret;

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			pr_err("Error getting proto attr: %d!\n", ret);
		return ret;
	}

	ret = scmi_to_linux_errno(response.status);
	if (ret) {
		pr_err("Error getting proto attr: %d!\n", ret);
		return ret;
	}

	priv->num_ranges = response.attributes & SCMI_PINCTRL_NUM_RANGES_MASK;

	return 0;
}

static int scmi_pinctrl_get_pin_ranges(struct udevice *dev,
				       struct udevice *scmi_dev)
{
	struct scmi_pinctrl_priv *priv = dev_get_priv(dev);
	struct {
		s32 status;
		struct pr {
			u16 begin;
			u16 num_pins;
		} pin_ranges[];
	} *response;
	struct scmi_msg msg = {
		.protocol_id	= SCMI_PROTOCOL_ID_PINCTRL,
		.message_id	= SCMI_PINCTRL_DESCRIBE,
		.in_msg		= NULL,
		.in_msg_sz	= 0,
	};
	size_t out_msg_sz;
	int i, ret;

	out_msg_sz = sizeof(*response) + priv->num_ranges * sizeof(struct pr);

	response = malloc(out_msg_sz);
	if (!response) {
		pr_err("Error allocating memory!\n");
		ret = -ENOMEM;
		goto err;
	}
	msg.out_msg = (u8 *)response;
	msg.out_msg_sz = out_msg_sz;

	ret = scmi_send_and_process_msg(scmi_dev, &msg);
	if (ret) {
		pr_err("Error getting pin ranges: %d!\n", ret);
		goto err_response;
	}

	ret = scmi_to_linux_errno(response->status);
	if (ret) {
		pr_err("Error getting pin ranges: %d!\n", ret);
		goto err_response;
	}

	priv->ranges = malloc(priv->num_ranges * sizeof(*priv->ranges));
	if (!priv->ranges) {
		ret = -ENOMEM;
		goto err_response;
	}

	for (i = 0; i < priv->num_ranges; ++i) {
		priv->ranges[i].begin = response->pin_ranges[i].begin;
		priv->ranges[i].num_pins = response->pin_ranges[i].num_pins;
	}

err_response:
	free(response);
err:
	return ret;
}

static int scmi_pinctrl_init(struct udevice *dev, struct udevice *scmi_dev)
{
	int ret = 0;

	ret = scmi_pinctrl_get_proto_attr(dev, scmi_dev);
	if (ret)
		return ret;

	return scmi_pinctrl_get_pin_ranges(dev, scmi_dev);
}

static int scmi_pinctrl_probe(struct udevice *dev)
{
	struct udevice *scmi_dev = dev->parent;
	struct scmi_pinctrl_priv *priv = dev_get_priv(dev);
	int ret;

	if (!scmi_dev)
		return -ENXIO;

	ret = scmi_pinctrl_init(dev, scmi_dev);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&priv->gpio_configs);

	return 0;
}

U_BOOT_DRIVER(scmi_pinctrl) = {
	.name = "scmi_pinctrl",
	.id = UCLASS_PINCTRL,
	.probe = scmi_pinctrl_probe,
	.priv_auto_alloc_size = sizeof(struct scmi_pinctrl_priv),
	.ops = &scmi_pinctrl_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
