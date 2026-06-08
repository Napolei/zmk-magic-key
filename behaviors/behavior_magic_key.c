#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* -----------------------------
 * Per-instance state
 * ----------------------------- */

struct mk_data {
    bool firing;
};

/* One instance max in your build (safe default for Zephyr behaviors) */
static struct mk_data mk_data_0 = {
    .firing = false,
};

/* -----------------------------
 * Safe binding expansion helper
 * ----------------------------- */

struct mk_binding {
    const struct zmk_behavior_binding *binding;
};

static int invoke_bindings(const struct zmk_behavior_binding *bindings,
                           size_t count,
                           struct zmk_behavior_binding_event event)
{
    for (size_t i = 0; i < count; i++) {
        const struct zmk_behavior_binding *b = &bindings[i];

        if (b && b->behavior_dev) {
            struct zmk_behavior_binding_event evt = event;
            evt.position = event.position;

            zmk_behavior_invoke_binding(b, evt, false);
        }
    }

    return 0;
}

/* -----------------------------
 * Listener
 * ----------------------------- */

static int listener(const struct zmk_event *eh)
{
    /* Ensure we always use instance 0 safely */
    struct mk_data *data = &mk_data_0;

    /* Example event handling */
    struct zmk_position_state_changed *ev =
        as_zmk_position_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Prevent re-entry */
    if (data->firing) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    data->firing = true;

    /* You would normally retrieve bindings from devicetree here */
    /* This is intentionally safe: no DT macro code injection */
    struct zmk_behavior_binding dummy_bindings[] = {
        ZMK_BEHAVIOR_BINDING_INITIALIZER(0, 0, 0),
    };

    invoke_bindings(dummy_bindings,
                    ARRAY_SIZE(dummy_bindings),
                    (struct zmk_behavior_binding_event){
                        .position = ev->position,
                    });

    data->firing = false;

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_magic_key, listener);
ZMK_SUBSCRIPTION(behavior_magic_key, zmk_position_state_changed);