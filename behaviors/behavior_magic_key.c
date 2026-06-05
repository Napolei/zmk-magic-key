/*
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 *
 * behavior_magic_key.c
 *
 * Antecedent-morph ("magic key") behavior for ZMK.
 *
 * Emits a different binding based on the sequence of keys typed
 * immediately before the magic key is pressed. Longest match wins.
 * Falls back to a default binding when nothing matches.
 *
 * Devicetree binding: zmk,behavior-magic-key
 */

#define DT_DRV_COMPAT zmk_behavior_magic_key

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define MAGIC_KEY_HISTORY_SIZE 8

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

struct magic_key_antecedent {
    const uint32_t *keycodes;
    uint8_t         length;
};

struct magic_key_config {
    int32_t  max_delay_ms;
    uint8_t  antecedent_count;
    const struct magic_key_antecedent *antecedents;
    const struct zmk_behavior_binding *morphs;
    const struct zmk_behavior_binding  defaults;
};

struct magic_key_data {
    uint32_t history[MAGIC_KEY_HISTORY_SIZE];
    uint8_t  head;
    uint8_t  count;
    int64_t  last_press_time;
    bool     firing;
    int      resolved_index;
};

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ---------------------------------------------------------------------- */

static void history_push(struct magic_key_data *data, uint32_t encoded)
{
    data->history[data->head] = encoded;
    data->head = (data->head + 1) % MAGIC_KEY_HISTORY_SIZE;
    if (data->count < MAGIC_KEY_HISTORY_SIZE) {
        data->count++;
    }
    data->last_press_time = k_uptime_get();
}

static bool history_read(const struct magic_key_data *data,
                         uint8_t len, uint32_t *out)
{
    if (len == 0 || len > data->count) {
        return false;
    }
    int start = ((int)data->head - (int)len + MAGIC_KEY_HISTORY_SIZE)
                % MAGIC_KEY_HISTORY_SIZE;
    for (uint8_t i = 0; i < len; i++) {
        out[i] = data->history[(start + i) % MAGIC_KEY_HISTORY_SIZE];
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Encoded keycode helper
 *
 * Reconstruct the same uint32_t that keys.h macros produce, so that
 * the value stored in history matches the DT antecedent-keycodes array.
 * ---------------------------------------------------------------------- */

static inline uint32_t encoded_from_event(
    const struct zmk_keycode_state_changed *ksc)
{
    return ZMK_HID_USAGE(ksc->usage_page, ksc->keycode);
}

/* -------------------------------------------------------------------------
 * Matching
 * ---------------------------------------------------------------------- */

static int find_match(const struct magic_key_config *cfg,
                      struct magic_key_data *data,
                      int64_t now)
{
    if (cfg->max_delay_ms > 0 &&
        (now - data->last_press_time) > cfg->max_delay_ms) {
        LOG_DBG("magic_key: timeout, using default");
        return -1;
    }

    int     best_index = -1;
    uint8_t best_len   = 0;
    uint32_t window[MAGIC_KEY_HISTORY_SIZE];

    for (int i = 0; i < cfg->antecedent_count; i++) {
        const struct magic_key_antecedent *ant = &cfg->antecedents[i];

        if (ant->length <= best_len) {
            continue;
        }
        if (!history_read(data, ant->length, window)) {
            continue;
        }
        if (memcmp(window, ant->keycodes,
                   ant->length * sizeof(uint32_t)) == 0) {
            best_index = i;
            best_len   = ant->length;
            LOG_DBG("magic_key: matched antecedent[%d] len=%d", i, ant->length);
        }
    }

    return best_index;
}

/* -------------------------------------------------------------------------
 * Keycode event listener
 * ---------------------------------------------------------------------- */

static int magic_key_keycode_listener(const zmk_event_t *ev)
{
    const struct zmk_keycode_state_changed *ksc =
        as_zmk_keycode_state_changed(ev);

    /* Track key-up events only */
    if (!ksc || ksc->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t encoded = encoded_from_event(ksc);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(magic_key, magic_key_keycode_listener);
ZMK_SUBSCRIPTION(magic_key, zmk_keycode_state_changed);

/* -------------------------------------------------------------------------
 * Behavior callbacks
 * ---------------------------------------------------------------------- */

static int magic_key_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg  = dev->config;
    struct magic_key_data         *data = dev->data;

    data->resolved_index = find_match(cfg, data, k_uptime_get());

    const struct zmk_behavior_binding *chosen =
        (data->resolved_index >= 0)
            ? &cfg->morphs[data->resolved_index]
            : &cfg->defaults;

    LOG_DBG("magic_key: press resolved=%d", data->resolved_index);

    data->firing = true;
    int ret = zmk_behavior_invoke_binding(chosen, event, true);
    data->firing = false;

    return ret;
}

static int magic_key_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg  = dev->config;
    struct magic_key_data         *data = dev->data;

    const struct zmk_behavior_binding *chosen =
        (data->resolved_index >= 0)
            ? &cfg->morphs[data->resolved_index]
            : &cfg->defaults;

    LOG_DBG("magic_key: release resolved=%d", data->resolved_index);

    data->firing = true;
    int ret = zmk_behavior_invoke_binding(chosen, event, false);
    data->firing = false;

    return ret;
}

static const struct behavior_driver_api magic_key_driver_api = {
    .binding_pressed  = magic_key_binding_pressed,
    .binding_released = magic_key_binding_released,
};

/* -------------------------------------------------------------------------
 * Devicetree instantiation
 * ---------------------------------------------------------------------- */

static int magic_key_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

/*
 * STRUCT_SECTION_ITERABLE cannot be used directly inside a macro with ##n
 * token pasting because the ## is evaluated before macro expansion.
 * This helper indirection forces n to be substituted first.
 */

#define MAGIC_KEY_INST(n)                                                      \
                                                                               \
    /* Raw flat DT array: [len, k0..kN, len, k0..kN, ...] */                  \
    static const uint32_t mk_raw_##n[] =                                       \
        DT_INST_PROP(n, antecedent_keycodes);                                  \
                                                                               \
    /* Antecedent structs populated at init by walking mk_raw_##n */           \
    static struct magic_key_antecedent                                         \
        mk_antecedents_##n[DT_INST_PROP(n, antecedent_count)];                 \
                                                                               \
    /* Morph bindings — one per antecedent */                                  \
    static const struct zmk_behavior_binding mk_morphs_##n[] = {               \
        LISTIFY(DT_INST_PROP(n, antecedent_count),                             \
                ZMK_KEYMAP_EXTRACT_BINDING, (,), DT_DRV_INST(n))              \
    };                                                                         \
                                                                               \
    /* Default binding — last entry in bindings */                             \
    static const struct zmk_behavior_binding mk_default_##n =                 \
        ZMK_KEYMAP_EXTRACT_BINDING(                                            \
            DT_INST_PROP(n, antecedent_count), DT_DRV_INST(n));               \
                                                                               \
    static int magic_key_init_##n(const struct device *dev)                    \
    {                                                                          \
        ARG_UNUSED(dev);                                                       \
        const uint32_t count = DT_INST_PROP(n, antecedent_count);             \
        const uint32_t *p = mk_raw_##n;                                        \
        for (uint32_t i = 0; i < count; i++) {                                \
            uint32_t len = *p++;                                               \
            mk_antecedents_##n[i].length   = (uint8_t)len;                    \
            mk_antecedents_##n[i].keycodes = p;                               \
            p += len;                                                          \
        }                                                                      \
        return 0;                                                              \
    }                                                                          \
                                                                               \
    static const struct magic_key_config mk_cfg_##n = {                        \
        .max_delay_ms     = DT_INST_PROP(n, max_delay_ms),                    \
        .antecedent_count = DT_INST_PROP(n, antecedent_count),               \
        .antecedents      = mk_antecedents_##n,                               \
        .morphs           = mk_morphs_##n,                                    \
        .defaults         = mk_default_##n,                                   \
    };                                                                         \
                                                                               \
    MAGIC_KEY_DATA_DEFINE(n) = {                                               \
        .head            = 0,                                                  \
        .count           = 0,                                                  \
        .last_press_time = 0,                                                  \
        .firing          = false,                                              \
        .resolved_index  = -1,                                                 \
    };                                                                         \
                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, magic_key_init_##n, NULL,                      \
        &mk_data_##n, &mk_cfg_##n,                                             \
        APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
        &magic_key_driver_api);

#define MAGIC_KEY_PUSH(n)                          \
    do {                                           \
        if (!mk_data_##n.firing) {                 \
            history_push(&mk_data_##n, encoded);   \
        }                                          \
    } while (0);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_INST)

#define MAGIC_KEY_DATA_REF(n) &mk_data_##n,
