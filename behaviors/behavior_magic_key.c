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
 * Antecedent keycodes are specified using ZMK keycode macros from
 * dt-bindings/zmk/keys.h (e.g. T, H, E). Each macro encodes the HID
 * usage page and usage ID into a single 32-bit integer using the layout:
 *
 *   bits 15..0  : HID usage ID
 *   bits 23..16 : HID usage page
 *   bits 31..24 : flags (e.g. ZMK_HID_USAGE_PAGE_SHIFT)
 *
 * ZMK_HID_USAGE() and ZMK_HID_USAGE_PAGE() macros extract these fields.
 * The event listener stores the same encoded value from the keycode event
 * so that comparisons work correctly without decoding.
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
#include <zmk/keys.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/*
 * Maximum number of recent keycodes retained in the ring buffer.
 * Must be >= the longest antecedent sequence you intend to use.
 */
#define MAGIC_KEY_HISTORY_SIZE 8

/* -------------------------------------------------------------------------
 * Encoded keycode helpers
 *
 * ZMK keycode macros from keys.h encode page + usage into one uint32_t.
 * We store and compare them in this encoded form so we never need to
 * decode and re-encode. The same encoded value is reconstructed from the
 * keycode event fields using ZMK_HID_USAGE().
 * ---------------------------------------------------------------------- */

/*
 * Reconstruct the encoded keycode from a keycode_state_changed event.
 * This mirrors how keys.h macros are formed so the comparison is direct.
 */
static inline uint32_t encoded_from_event(
    const struct zmk_keycode_state_changed *ksc)
{
    return ZMK_HID_USAGE(ksc->usage_page, ksc->keycode);
}

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

struct magic_key_antecedent {
    /*
     * Sequence of encoded keycodes to match, index 0 = oldest keypress.
     * Values are ZMK keycode macros (e.g. T, H, E) stored as-is from DT.
     */
    const uint32_t *keycodes;
    uint8_t         length;
};

struct magic_key_config {
    int32_t  max_delay_ms;
    uint8_t  antecedent_count;
    const struct magic_key_antecedent *antecedents;
    const struct zmk_behavior_binding *morphs;  /* [antecedent_count] */
    const struct zmk_behavior_binding  defaults;
};

struct magic_key_data {
    /* Ring buffer of recent encoded keycodes (key-up, excluding magic key) */
    uint32_t history[MAGIC_KEY_HISTORY_SIZE];
    uint8_t  head;   /* next write position */
    uint8_t  count;  /* valid entries, capped at MAGIC_KEY_HISTORY_SIZE */

    /* Uptime of the last history push, for max-delay-ms */
    int64_t last_press_time;

    /*
     * Physical position of the magic key itself, set on binding_pressed.
     * Used to exclude the magic key's own release event from history.
     */
    uint32_t magic_key_position;

    /*
     * Set while the resolved binding is being invoked so that synthetic
     * key events produced by macros are not pushed into history.
     */
    bool firing;

    /*
     * Binding index resolved on press, reused on release so both events
     * always invoke the same binding even if history changes in between.
     * -1 means the default binding.
     */
    int resolved_index;
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

/*
 * Copy the most-recent `len` encoded keycodes into `out`, oldest first.
 * Returns false if fewer than `len` entries have been recorded.
 */
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
 * Matching
 * ---------------------------------------------------------------------- */

/*
 * Return the index of the longest antecedent that matches the tail of the
 * history buffer, or -1 if none match. Longest-match wins so that e.g. a
 * rule for [T, H, E] beats [T, H] when all three keys are present.
 */
static int find_match(const struct magic_key_config *cfg,
                      struct magic_key_data *data,
                      int64_t now)
{
    LOG_DBG("magic_key find_match: count=%d last_press=%lld now=%lld",
            data->count, data->last_press_time, now);
    for (int i = 0; i < data->count; i++) {
        int idx = ((int)data->head - (int)data->count + i
                   + MAGIC_KEY_HISTORY_SIZE) % MAGIC_KEY_HISTORY_SIZE;
        LOG_DBG("  history[%d] = 0x%08x", i, data->history[idx]);
    }
    for (int i = 0; i < cfg->antecedent_count; i++) {
        LOG_DBG("  antecedent[%d] len=%d keycodes[0]=0x%08x",
                i, cfg->antecedents[i].length,
                cfg->antecedents[i].length > 0
                    ? cfg->antecedents[i].keycodes[0] : 0);
    }

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

    // Log every event that reaches the listener at all
    if (ksc) {
        LOG_DBG("magic_key listener: state=%d page=0x%02x keycode=0x%04x",
                ksc->state, ksc->usage_page, ksc->keycode);
    } else {
        LOG_DBG("magic_key listener: not a keycode event");
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Track key-up events only */
    if (!ksc || ksc->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t encoded = encoded_from_event(ksc);

    STRUCT_SECTION_FOREACH(magic_key_data, data) {
        if (data->firing) {
            LOG_DBG("magic_key: skip synthetic 0x%08x", encoded);
            continue;
        }
        // position filter removed — not available on keycode event
        history_push(data, encoded);
        LOG_DBG("magic_key: push 0x%08x count=%d", encoded, data->count);
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
    LOG_DBG("magic_key binding_pressed: position=%d", event.position);
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg  = dev->config;
    struct magic_key_data         *data = dev->data;

    data->magic_key_position = event.position;
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
 *
 * antecedent-keycodes is a flat array with a length prefix per sequence:
 *
 *   antecedent-keycodes = <3 T H E  2 T H  1 A>;
 *
 * T, H, E etc. are ZMK keycode macros from keys.h. They expand to 32-bit
 * integers encoding HID usage page + usage ID, usable directly inside a
 * DT cell array.
 *
 * The raw array (including length prefixes) is stored in flash and parsed
 * into magic_key_antecedent structs at device init time. Each struct's
 * `keycodes` pointer points directly into the raw array, so no extra
 * keycode storage is needed.
 * ---------------------------------------------------------------------- */

static int magic_key_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    return 0;
}

#define MAGIC_KEY_DATA_DEFINE(n) \
    STRUCT_SECTION_ITERABLE(magic_key_data, mk_data_##n)

#define MAGIC_KEY_INST(n)                                                      \
                                                                               \
    /* Raw flat DT array: [len, k0..kN, len, k0..kN, ...] */                  \
    static const uint32_t mk_raw_##n[] =                                       \
        DT_INST_PROP(n, antecedent_keycodes);                                  \
                                                                               \
    /* Antecedent structs, populated at init by walking mk_raw_##n */          \
    static struct magic_key_antecedent                                         \
        mk_antecedents_##n[DT_INST_PROP(n, antecedent_count)];                 \
                                                                               \
    /* Morph bindings — one per antecedent */                                  \
    static const struct zmk_behavior_binding mk_morphs_##n[] = {               \
        LISTIFY(DT_INST_PROP(n, antecedent_count),                             \
                ZMK_KEYMAP_EXTRACT_BINDING, (,), n, bindings)                  \
    };                                                                         \
                                                                               \
    static int magic_key_init_##n(const struct device *dev)                    \
    {                                                                          \
        ARG_UNUSED(dev);                                                       \
        const uint32_t count = DT_INST_PROP(n, antecedent_count);             \
        const uint32_t *p = mk_raw_##n;                                        \
        for (uint32_t i = 0; i < count; i++) {                                \
            uint32_t len = *p++;   /* length prefix */                         \
            mk_antecedents_##n[i].length   = (uint8_t)len;                    \
            mk_antecedents_##n[i].keycodes = p;  /* pointer into flash array */\
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
        .defaults         = ZMK_KEYMAP_EXTRACT_BINDING(                        \
                                n, bindings,                                   \
                                DT_INST_PROP(n, antecedent_count)),            \
    };                                                                         \
                                                                               \
    MAGIC_KEY_DATA_DEFINE(n) = {                                               \
        .head               = 0,                                               \
        .count              = 0,                                               \
        .last_press_time    = 0,                                               \
        .magic_key_position = UINT32_MAX,                                      \
        .firing             = false,                                            \
        .resolved_index     = -1,                                              \
    };                                                                         \
                                                                               \
    STRUCT_SECTION_ITERABLE(magic_key_data, mk_data_##n) = {                   \
        .head               = 0,                                               \
        .count              = 0,                                               \
        .last_press_time    = 0,                                               \
        .magic_key_position = UINT32_MAX,                                      \
        .firing             = false,                                            \
        .resolved_index     = -1,                                              \
    };                                                                         \
                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, magic_key_init_##n, NULL,        \
        &mk_data_##n, &mk_cfg_##n,                               \
        APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,        \
        &magic_key_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_INST)