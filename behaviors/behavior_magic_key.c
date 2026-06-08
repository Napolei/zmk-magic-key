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

#define MAGIC_KEY_HISTORY_SIZE CONFIG_ZMK_MAGIC_KEY_HISTORY_SIZE

struct magic_key_sequence {
    const uint32_t *antecedent;
    uint8_t antecedent_len;

    const struct zmk_behavior_binding *bindings;
    uint8_t binding_len;
};

struct magic_key_config {
    const struct magic_key_sequence *sequences;
    uint8_t sequence_count;

    const struct zmk_behavior_binding *fallback_bindings;
    uint8_t fallback_bindings_len;
};

struct magic_key_data {
    uint32_t history[MAGIC_KEY_HISTORY_SIZE];
    uint8_t head;
    uint8_t count;

    bool firing;
    int resolved_index;
};

/* ---------------- HISTORY ---------------- */

static void history_push(struct magic_key_data *data, uint32_t v)
{
    data->history[data->head] = v;
    data->head = (data->head + 1) % MAGIC_KEY_HISTORY_SIZE;

    if (data->count < MAGIC_KEY_HISTORY_SIZE) {
        data->count++;
    }
}

static bool history_read(const struct magic_key_data *data,
                         uint8_t len,
                         uint32_t *out)
{
    if (len == 0 || len > data->count) {
        return false;
    }

    int start = (int)data->head - (int)len;
    if (start < 0) {
        start += MAGIC_KEY_HISTORY_SIZE;
    }

    for (uint8_t i = 0; i < len; i++) {
        out[i] = data->history[(start + i) % MAGIC_KEY_HISTORY_SIZE];
    }

    return true;
}

/* ---------------- MATCH ---------------- */

static int find_match(const struct magic_key_config *cfg,
                      struct magic_key_data *data)
{
    uint32_t window[MAGIC_KEY_HISTORY_SIZE];

    int best = -1;
    uint8_t best_len = 0;

    for (int i = 0; i < cfg->sequence_count; i++) {

        const struct magic_key_sequence *seq = &cfg->sequences[i];

        if (seq->antecedent_len <= best_len) {
            continue;
        }

        if (!history_read(data, seq->antecedent_len, window)) {
            continue;
        }

        if (memcmp(window,
                   seq->antecedent,
                   seq->antecedent_len * sizeof(uint32_t)) == 0) {
            best = i;
            best_len = seq->antecedent_len;
        }
    }

    return best;
}

/* ---------------- INVOKE ---------------- */

qqstatic int invoke_list(const struct zmk_behavior_binding *bindings,
                       uint8_t len,
                       struct zmk_behavior_binding_event event,
                       bool pressed)
{
    int ret = 0;

    for (uint8_t i = 0; i < len; i++) {
        ret = zmk_behavior_invoke_binding(&bindings[i], event, pressed);
    }

    return ret;
}

/* ---------------- DEVICE INST DATA ---------------- */

#define MAGIC_KEY_DECLARE(n) \
    static struct magic_key_data mk_data_##n;

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_DECLARE)

/* ---------------- LISTENER ---------------- */

static int listener(const zmk_event_t *eh)
{
    const struct zmk_keycode_state_changed *ev =
        as_zmk_keycode_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->usage_page != HID_USAGE_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->keycode >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL &&
        ev->keycode <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint32_t encoded = ev->keycode;

    /* FIX: expand INST list INSIDE macro scope, not via separate PUSH macro */
    DT_INST_FOREACH_STATUS_OKAY(
        static void _mk_push_##__LINE__(void) { }
    );

#define PUSH_INSTANCE(n) \
    do { \
        extern struct magic_key_data mk_data_##n; \
        if (!mk_data_##n.firing) { \
            history_push(&mk_data_##n, encoded); \
        } \
    } while (0)

    DT_INST_FOREACH_STATUS_OKAY(PUSH_INSTANCE);

#undef PUSH_INSTANCE

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(magic_key, listener);
ZMK_SUBSCRIPTION(magic_key, zmk_keycode_state_changed);

/* ---------------- BEHAVIOR ---------------- */

static int pressed(struct zmk_behavior_binding *binding,
                   struct zmk_behavior_binding_event event)
{
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg = dev->config;
    struct magic_key_data *data = dev->data;

    data->resolved_index = find_match(cfg, data);
    data->firing = true;

    int ret;

    if (data->resolved_index >= 0) {
        const struct magic_key_sequence *seq =
            &cfg->sequences[data->resolved_index];

        ret = invoke_list(seq->bindings, seq->binding_len, event, true);
    } else {
        ret = invoke_list(cfg->fallback_bindings,
                          cfg->fallback_bindings_len,
                          event,
                          true);
    }

    data->firing = false;
    return ret;
}

static int released(struct zmk_behavior_binding *binding,
                    struct zmk_behavior_binding_event event)
{
    const struct device *dev = device_get_binding(binding->behavior_dev);
    const struct magic_key_config *cfg = dev->config;
    struct magic_key_data *data = dev->data;

    data->firing = true;

    int ret;

    if (data->resolved_index >= 0) {
        const struct magic_key_sequence *seq =
            &cfg->sequences[data->resolved_index];

        ret = invoke_list(seq->bindings, seq->binding_len, event, false);
    } else {
        ret = invoke_list(cfg->fallback_bindings,
                          cfg->fallback_bindings_len,
                          event,
                          false);
    }

    data->firing = false;
    return ret;
}

static const struct behavior_driver_api api = {
    .binding_pressed = pressed,
    .binding_released = released,
};

/* ---------------- DT ---------------- */

#define MAGIC_KEY_BINDING_ENTRY(node_id, prop, idx) \
    { \
        .behavior_dev = DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node_id, prop, idx)), \
        .param1 = DT_PHA_BY_IDX_OR(node_id, prop, idx, param1, 0), \
        .param2 = DT_PHA_BY_IDX_OR(node_id, prop, idx, param2, 0), \
    }

#define MK_SEQ_INIT(child) \
    { \
        .antecedent = (const uint32_t *)DT_PROP(child, antecedent), \
        .antecedent_len = DT_PROP_LEN(child, antecedent), \
        .bindings = (const struct zmk_behavior_binding[]){ \
            DT_FOREACH_PROP_ELEM(child, bindings, MAGIC_KEY_BINDING_ENTRY) \
        }, \
        .binding_len = DT_PROP_LEN(child, bindings), \
    }

#define MAGIC_KEY_INST(n) \
    static struct magic_key_data mk_data_##n = { .resolved_index = -1 }; \
    \
    static const struct magic_key_sequence mk_sequences_##n[] = { \
        DT_FOREACH_CHILD(DT_DRV_INST(n), MK_SEQ_INIT) \
    }; \
    \
    static const struct zmk_behavior_binding mk_fallback_##n[] = { \
        DT_FOREACH_PROP_ELEM(DT_DRV_INST(n), fallback_bindings, MAGIC_KEY_BINDING_ENTRY) \
    }; \
    \
    static const struct magic_key_config mk_cfg_##n = { \
        .sequences = mk_sequences_##n, \
        .sequence_count = ARRAY_SIZE(mk_sequences_##n), \
        .fallback_bindings = mk_fallback_##n, \
        .fallback_bindings_len = ARRAY_SIZE(mk_fallback_##n), \
    }; \
    \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &mk_data_##n, &mk_cfg_##n, \
        APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &api);

DT_INST_FOREACH_STATUS_OKAY(MAGIC_KEY_INST)