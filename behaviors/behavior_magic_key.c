/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_magic_key

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MAX_HISTORY 16
#define MAX_PATTERN_LEN 8

struct magic_rule {
    const int32_t *pattern;
    size_t pattern_len;

    struct zmk_behavior_binding binding;
};

struct behavior_magic_key_config {
    int serial;

    uint32_t max_delay_ms;

    size_t rules_len;

    const struct magic_rule *rules;
};

struct behavior_magic_key_data {
    struct zmk_behavior_binding *pressed_binding;
};

struct history_entry {
    int32_t code;
    int64_t timestamp;
};

static struct history_entry history[MAX_HISTORY];

static size_t history_head;
static size_t history_len;

static int32_t encode_keycode(
    struct zmk_keycode_state_changed *ev
) {
    return ev->keycode;
}

static void push_history(
    int32_t code,
    int64_t timestamp
) {
    history[history_head].code = code;
    history[history_head].timestamp = timestamp;

    history_head =
        (history_head + 1) % MAX_HISTORY;

    if (history_len < MAX_HISTORY) {
        history_len++;
    }
}

static struct history_entry history_get(
    size_t reverse_index
) {
    size_t idx =
        (history_head + MAX_HISTORY - 1 - reverse_index)
        % MAX_HISTORY;

    return history[idx];
}

static int magic_key_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_magic_key, magic_key_listener);

ZMK_SUBSCRIPTION(
    behavior_magic_key,
    zmk_keycode_state_changed
);

static int magic_key_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Only track keyboard HID usages */
    if (ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Ignore modifiers */
    if (ev->keycode >= 0xE0 && ev->keycode <= 0xE7) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Only record key presses */
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int32_t code = ev->keycode;

    /* Shift history */
    for (int i = MAGIC_KEY_HISTORY_LEN - 1; i > 0; i--) {
        history[i] = history[i - 1];
    }

    /* Insert newest key */
    history[0].code = code;
    history[0].timestamp = ev->timestamp;

    return ZMK_EV_EVENT_BUBBLE;
}

static bool rule_matches(
    const struct magic_rule *rule,
    uint32_t max_delay_ms,
    int64_t now
) {
    if (history_len < rule->pattern_len) {
        return false;
    }

    for (size_t i = 0; i < rule->pattern_len; i++) {

        struct history_entry h =
            history_get(rule->pattern_len - i - 1);

        if (h.code != rule->pattern[i]) {
            return false;
        }

        uint32_t delay_ms =
            ((now - h.timestamp) < 0)
                ? 0
                : (uint32_t)(now - h.timestamp);

        if (delay_ms > max_delay_ms) {
            return false;
        }
    }

    return true;
}

static int on_magic_key_binding_pressed(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    const struct behavior_magic_key_config *cfg =
        dev->config;

    struct behavior_magic_key_data *data =
        dev->data;

    if (data->pressed_binding != NULL) {
        LOG_ERR("Magic key already pressed");
        return -ENOTSUP;
    }

    const struct magic_rule *best_rule = NULL;

    size_t best_len = 0;

    for (size_t i = 0; i < cfg->rules_len; i++) {

        const struct magic_rule *rule =
            &cfg->rules[i];

        if (rule_matches(
                rule,
                cfg->max_delay_ms,
                event.timestamp
            )) {

            if (rule->pattern_len > best_len) {
                best_len = rule->pattern_len;
                best_rule = rule;
            }
        }
    }

    if (!best_rule) {
        return 0;
    }

    data->pressed_binding =
        (struct zmk_behavior_binding *)
            &best_rule->binding;

    return behavior_keymap_binding_pressed(
        data->pressed_binding,
        event
    );
}

static int on_magic_key_binding_released(
    struct zmk_behavior_binding *binding,
    struct zmk_behavior_binding_event event
) {

    const struct device *dev =
        zmk_behavior_get_binding(binding->behavior_dev);

    struct behavior_magic_key_data *data =
        dev->data;

    if (data->pressed_binding == NULL) {
        return 0;
    }

    struct zmk_behavior_binding *pressed =
        data->pressed_binding;

    data->pressed_binding = NULL;

    return behavior_keymap_binding_released(
        pressed,
        event
    );
}

static const struct behavior_driver_api
    behavior_magic_key_driver_api = {
        .binding_pressed =
            on_magic_key_binding_pressed,

        .binding_released =
            on_magic_key_binding_released,
};

static int behavior_magic_key_init(
    const struct device *dev
) {

    const struct behavior_magic_key_config *cfg =
        dev->config;

    LOG_DBG(
        "magic-key %d initialized with %d rules",
        cfg->serial,
        cfg->rules_len
    );

    return 0;
}

/*
 * DTS extraction helpers
 */

#define MAGIC_KEY_EXTRACT_BINDING(idx, node) \
{ \
    .behavior_dev = DEVICE_DT_NAME( \
        DT_PHANDLE_BY_IDX(node, bindings, idx) \
    ), \
    .param1 = COND_CODE_0( \
        DT_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), \
        (0), \
        (DT_PHA_BY_IDX(node, bindings, idx, param1)) \
    ), \
    .param2 = COND_CODE_0( \
        DT_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), \
        (0), \
        (DT_PHA_BY_IDX(node, bindings, idx, param2)) \
    ), \
}

#define MAGIC_KEY_RULE(child) \
{ \
    .pattern = DT_PROP(child, pattern), \
    .pattern_len = DT_PROP_LEN(child, pattern), \
    .binding = MAGIC_KEY_EXTRACT_BINDING(0, child), \
},

#define MAGIC_KEY_RULE_LIST(n) \
{ \
    DT_FOREACH_CHILD( \
        DT_DRV_INST(n), \
        MAGIC_KEY_RULE \
    ) \
}

#define MAGIC_KEY_INST(n) \
\
static const struct magic_rule \
    magic_rules_##n[] = \
        MAGIC_KEY_RULE_LIST(n); \
\
static struct behavior_magic_key_config \
    behavior_magic_key_config_##n = { \
        .serial = n, \
        .max_delay_ms = DT_INST_PROP( \
            n, \
            max_delay_ms \
        ), \
        .rules = magic_rules_##n, \
        .rules_len = ARRAY_SIZE(magic_rules_##n), \
}; \
\
static struct behavior_magic_key_data \
    behavior_magic_key_data_##n = {}; \
\
BEHAVIOR_DT_INST_DEFINE( \
    n, \
    behavior_magic_key_init, \
    NULL, \
    &behavior_magic_key_data_##n, \
    &behavior_magic_key_config_##n, \
    POST_KERNEL, \
    CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
    &behavior_magic_key_driver_api \
);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_INST)

#endif