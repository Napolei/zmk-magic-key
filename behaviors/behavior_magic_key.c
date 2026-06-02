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

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/*
 * Maximum number of recent keycodes retained in the ring buffer.
 * Must be >= the longest antecedent sequence you intend to use.
 * Increasing this costs only (HISTORY_SIZE * 4) bytes of RAM per instance.
 */
#define MAGIC_KEY_HISTORY_SIZE 8

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

struct magic_key_antecedent {
    const uint32_t *keycodes; /* sequence to match, index 0 = oldest */
    uint8_t         length;
};

struct magic_key_config {
    int32_t  max_delay_ms;
    uint8_t  antecedent_count;
    const struct magic_key_antecedent    *antecedents;
    const struct zmk_behavior_binding    *morphs;   /* [antecedent_count] */
    const struct zmk_behavior_binding     defaults;
};

struct magic_key_data {
    /* Ring buffer of recent keycodes (key-up events, excluding magic key) */
    uint32_t history[MAGIC_KEY_HISTORY_SIZE];
    uint8_t  head;   /* next write position */
    uint8_t  count;  /* number of valid entries, capped at MAGIC_KEY_HISTORY_SIZE */

    /* Uptime of the last history push (used for max-delay-ms) */
    int64_t last_press_time;

    /*
     * Key position of the magic key itself, recorded on binding_pressed.
     * Used to exclude the magic key from the history ring buffer.
     */
    uint32_t magic_key_position;

    /*
     * Set to true while the resolved binding is being invoked, so that
     * synthetic key events from macros are not pushed into history.
     */
    bool firing;

    /*
     * Index of the binding resolved on press, reused on release so that
     * both events always use the same binding even if history changes.
     */
    int resolved_index; /* -1 = default */
};

/* -------------------------------------------------------------------------
 * Ring buffer helpers
 * ---------------------------------------------------------------------- */

static void history_push(struct magic_key_data *data, uint32_t keycode)
{
    data->history[data->head] = keycode;
    data->head = (data->head + 1) % MAGIC_KEY_HISTORY_SIZE;
    if (data->count < MAGIC_KEY_HISTORY_SIZE) {
        data->count++;
    }
    data->last_press_time = k_uptime_get();
}

/*
 * Copy the most-recent `len` keycodes into `out`, oldest first.
 * Returns false if fewer than `len` keycodes have been recorded.
 */
static bool history_read(const struct magic_key_data *data,
                         uint8_t len, uint32_t *out)
{
    if (len == 0 || len > data->count) {
        return false;
    }

    /* Index of the oldest entry in the window */
    int start = ((int)data->head - (int)len + MAGIC_KEY_HISTORY_SIZE)
                % MAGIC_KEY_HISTORY_SIZE;

    for (uint8_t i = 0; i < len; i++) {
        out[i] = data->history[(start + i) % MAGIC_KEY_HISTORY_SIZE];
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Matching
 * ---------------------------------------------------------------------- */

/*
 * Walk all antecedent sequences and return the index of the longest one
 * that matches the tail of the history buffer, or -1 if none match.
 *
 * Longest-match wins: a rule for [t, h, e] beats [t, h] when all three
 * are present, allowing prefix-free and non-prefix-free rules to coexist.
 */
static int find_match(const struct magic_key_config *cfg,
                      struct magic_key_data *data,
                      int64_t now)
{
    if (cfg->max_delay_ms > 0 &&
        (now - data->last_press_time) > cfg->max_delay_ms) {
        LOG_DBG("magic_key: antecedent timeout, using default");
        return -1;
    }

    int     best_index = -1;
    uint8_t best_len   = 0;
    uint32_t window[MAGIC_KEY_HISTORY_SIZE];

    for (int i = 0; i < cfg->antecedent_count; i++) {
        const struct magic_key_antecedent *ant = &cfg->antecedents[i];

        if (ant->length <= best_len) {
            /* Can't improve on current best, skip */
            continue;
        }

        if (!history_read(data, ant->length, window)) {
            /* Not enough history for this sequence */
            continue;
        }

        if (memcmp(window, ant->keycodes,
                   ant->length * sizeof(uint32_t)) == 0) {
            best_index = i;
            best_len   = ant->length;
            LOG_DBG("magic_key: matched antecedent[%d] (len %d)", i, ant->length);
        }
    }

    return best_index;
}

/* -------------------------------------------------------------------------
 * Keycode event listener
 *
 * Listens for key-up events and pushes them into the history ring buffer
 * of every magic-key instance, skipping:
 *   - the magic key's own position
 *   - synthetic events fired while a morph binding is executing
 * ---------------------------------------------------------------------- */

static int magic_key_keycode_listener(const zmk_event_t *ev)
{
    const struct zmk_keycode_state_changed *ksc =
        as_zmk_keycode_state_changed(ev);

    /* Only track key-up events */
    if (!ksc || ksc->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Update history for every active magic-key device instance */
    STRUCT_SECTION_FOREACH(magic_key_data, data) {
        if (data->firing) {
            LOG_DBG("magic_key: skipping synthetic keycode 0x%08x", ksc->keycode);
            continue;
        }
        if (ksc->position == data->magic_key_position) {
            LOG_DBG("magic_key: skipping own key position %d", ksc->position);
            continue;
        }
        history_push(data, ksc->keycode);
        LOG_DBG("magic_key: history push keycode 0x%08x (count=%d)",
                ksc->keycode, data->count);
    }

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
    const struct magic_key_config *cfg = dev->config;
    struct magic_key_data         *data = dev->data;

    /* Record our own position so the listener can filter us out */
    data->magic_key_position = event.position;

    /* Resolve and cache the binding index for use by binding_released */
    data->resolved_index = find_match(cfg, data, k_uptime_get());

    const struct zmk_behavior_binding *chosen =
        (data->resolved_index >= 0)
            ? &cfg->morphs[data->resolved_index]
            : &cfg->defaults;

    LOG_DBG("magic_key: press, resolved_index=%d", data->resolved_index);

    data->firing = true;
    int ret = zmk_behavior_invoke_binding(chosen, event, true);
    data->firing = false;

    return ret;
}

static int magic_key_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg = dev->config;
    struct magic_key_data         *data = dev->data;

    /* Reuse the index resolved on press — history must not change between */
    const struct zmk_behavior_binding *chosen =
        (data->resolved_index >= 0)
            ? &cfg->morphs[data->resolved_index]
            : &cfg->defaults;

    LOG_DBG("magic_key: release, resolved_index=%d", data->resolved_index);

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
 *
 * antecedent-keycodes is a flat array with a length prefix per sequence:
 *
 *   antecedent-keycodes = <
 *       3  T H E
 *       2  T H
 *       1  A
 *   >;
 *
 * The macro below walks this array at compile time (via Zephyr's DT
 * initialiser helpers) to build the per-instance antecedent structs.
 * ---------------------------------------------------------------------- */

static int magic_key_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

/*
 * For each DT instance, expand the flat antecedent-keycodes array into:
 *   - a single flat uint32_t array holding all keycode values
 *   - an array of magic_key_antecedent structs that point into it
 *
 * The flat array is walked with a running offset: the first element of
 * each entry is its length, followed by that many keycodes.
 */
#define MAGIC_KEY_ANTECEDENT_TOTAL_KEYCODES(n) \
    (DT_INST_PROP_LEN(n, antecedent_keycodes) - DT_INST_PROP(n, antecedent_count))

/*
 * Build the flat keycode storage and antecedent struct array.
 *
 * Because Zephyr's DT macros don't provide a clean way to walk a
 * length-prefixed flat array at macro-expansion time, we store the
 * entire raw array (lengths + keycodes) and parse it at init time via
 * a one-off helper instead of a heavy X-macro chain.
 */
#define MAGIC_KEY_INST(n)                                                      \
                                                                               \
    /* Raw flat array including length prefixes */                             \
    static const uint32_t mk_raw_##n[] =                                       \
        DT_INST_PROP(n, antecedent_keycodes);                                  \
                                                                               \
    /* Per-entry antecedent structs — populated by magic_key_init_##n */       \
    static struct magic_key_antecedent mk_antecedents_##n                      \
        [DT_INST_PROP(n, antecedent_count)];                                   \
                                                                               \
    /* Morph bindings (first antecedent_count entries of `bindings`) */        \
    static const struct zmk_behavior_binding mk_morphs_##n[]  = {              \
        LISTIFY(DT_INST_PROP(n, antecedent_count),                             \
                ZMK_KEYMAP_EXTRACT_BINDING, (,), n, bindings)                  \
    };                                                                         \
                                                                               \
    /* Parse the raw array into antecedent structs at device init */           \
    static int magic_key_init_##n(const struct device *dev)                    \
    {                                                                          \
        ARG_UNUSED(dev);                                                       \
        uint8_t count = DT_INST_PROP(n, antecedent_count);                     \
        const uint32_t *p = mk_raw_##n;                                        \
        for (uint8_t i = 0; i < count; i++) {                                  \
            uint32_t len = *p++;                                               \
            mk_antecedents_##n[i].length   = (uint8_t)len;                     \
            mk_antecedents_##n[i].keycodes = p;                                \
            p += len;                                                          \
        }                                                                      \
        return 0;                                                              \
    }                                                                          \
                                                                               \
    static const struct magic_key_config mk_cfg_##n = {                        \
        .max_delay_ms     = DT_INST_PROP(n, max_delay_ms),                     \
        .antecedent_count = DT_INST_PROP(n, antecedent_count),                 \
        .antecedents      = mk_antecedents_##n,                                \
        .morphs           = mk_morphs_##n,                                     \
        .defaults         = ZMK_KEYMAP_EXTRACT_BINDING(                        \
                                n, bindings,                                   \
                                DT_INST_PROP(n, antecedent_count)),            \
    };                                                                         \
                                                                               \
    static struct magic_key_data mk_data_##n = {                               \
        .head              = 0,                                                \
        .count             = 0,                                                \
        .last_press_time   = 0,                                                \
        .magic_key_position = UINT32_MAX,                                      \
        .firing            = false,                                            \
        .resolved_index    = -1,                                               \
    };                                                                         \
                                                                               \
    DEVICE_DT_INST_DEFINE(n, magic_key_init_##n, NULL,                         \
        &mk_data_##n, &mk_cfg_##n,                                             \
        APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
        &magic_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_INST)