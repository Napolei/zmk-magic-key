# ZMK Magic Key

A custom ZMK behavior that emits different key sequences depending on the keys typed immediately before activation.

Inspired by antecedent morphing / text expansion systems.

---

# Features

* Match previously typed key sequences
* Longest-match wins
* Supports arbitrary antecedent lengths
* Supports multiple output bindings per match
* Supports standard ZMK keycodes (`DE_A`, `DE_Z`, etc.)
* Supports fallback behavior
* Fully configurable from Devicetree
* No macro definitions required for outputs

---

# Example

Typing:

```text
w + magic_key
```

can emit:

```text
Shift+A
```

Typing:

```text
t h + magic_key
```

can emit:

```text
e
```

Typing:

```text
t h e + magic_key
```

can emit:

```text
 re
```

---

# Installation

Add the module to your `west.yml`:

```yaml
manifest:
  projects:
    - name: zmk-magic-key
      url: https://github.com/YOUR_USERNAME/zmk-magic-key
```

Then update dependencies:

```bash
west update
```

---

# Usage

Add the behavior to your keymap:

```dts
behaviors {
    magic_key: magic_key {
        compatible = "zmk,behavior-magic-key";
        #binding-cells = <0>;

        max-delay-ms = <1000>;

        fallback-bindings = <&kp SPACE>;

        w_seq {
            antecedent = <DE_W>;
            bindings = 
                  <&kp LSHIFT>
                , <&kp DE_A>
                ;
        };

        th_seq {
            antecedent = <DE_T DE_H>;
            bindings = <&kp DE_E>;
        };

        the_seq {
            antecedent = <DE_T DE_H DE_E>;
            bindings =
                  <&kp DE_R>
                , <&kp DE_E>
                ;
        };
    };
};
```

Then use it in your layout:

```dts
&magic_key
```

---

# Configuration

## `max-delay-ms`

Maximum time allowed between the antecedent sequence and activation.

```dts
max-delay-ms = <1000>;
```

If the timeout expires, fallback bindings are used.

---

## `fallback-bindings`

Bindings emitted when no antecedent matches.

Example:

```dts
fallback-bindings = <&kp SPACE>;
```

---

# Sequence Nodes

Each child node defines:

* an `antecedent`
* a list of `bindings`

Example:

```dts
th_seq {
    antecedent = <DE_T DE_H>;
    bindings = <&kp DE_B>;
};
```

---

# Matching Rules

* Matching is based on the most recent key releases
* Longest matching sequence wins
* Only key-up events are tracked
* History is circular-buffer based
* Matching is layout-aware through HID usages

---

# Supported Keycodes

Any standard ZMK keycode works:

```dts
DE_A
DE_B
DE_Z
LEFT_ARROW
SPACE
```

International layouts work correctly because matching internally converts keycodes to HID usages.

---

# Multiple Output Keys

Outputs can contain multiple bindings:

```dts
bindings =
      <&kp DE_R>
    , <&kp DE_E>
    ;
```

No separate macro definitions are required.

---

# Example Expansions

## German umlauts

```dts
ae_seq {
    antecedent = <DE_A DE_E>;
    bindings = 
        <&kp DELETE>
        , <&kp DELETE>
        , <&kp DE_A_UMLAUT>
        ;
};
```

## Coding helpers

```dts
py_list_comp_seq {
    antecedent = <DE_LBKT DE_X>;
    bindings = 
          < &kp SPACE >
        , < &kp DE_F >
        , < &kp DE_O >
        , < &kp DE_R >
        , < &kp SPACE >
        , < &kp DE_X >
        , < &kp SPACE >
        , < &kp DE_I >
        , < &kp DE_N >
        , < &kp SPACE >
        , < &kp DE_RBKT >
        , < &kp LEFT_ARROW >
        , < &kp LC(SPACE) >
        ;
};
```

---

# Internals

The behavior:

1. Listens for released key events
2. Stores recent HID usages in a circular buffer
3. Searches configured sequences
4. Selects the longest match
5. Emits the configured binding list

---

# Limitations

* Matches only released keys
* History size is fixed at compile time
* Does not currently support modifiers in antecedents
* Does not consume previously typed characters

---

# License

MIT
