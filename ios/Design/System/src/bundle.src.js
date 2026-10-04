/* @ds-bundle: {"format":4,"namespace":"Dotty","components":[{"name":"LightField"},{"name":"LightOrb"},{"name":"Glass"},{"name":"Button"},{"name":"IconButton"},{"name":"ListRow"},{"name":"Toggle"},{"name":"Slider"},{"name":"ColorPicker"},{"name":"StatusPill"},{"name":"FireflyLoader"},{"name":"Logo"},{"name":"Icon"}]} */
(function () {
  var React = window.React;
  var h = React.createElement;

  // The full-colour firefly mark, injected by build.py from Design/Logo/dotty-mark.svg.
  var MARK_SRC = "__MARK_SRC__";

  var LIGHTS = {
    firefly: "var(--light-firefly)",
    leaf: "var(--light-leaf)",
    lagoon: "var(--light-lagoon)",
    dusk: "var(--light-dusk)",
    bloom: "var(--light-bloom)",
    ember: "var(--light-ember)",
    amber: "var(--light-amber)"
  };

  /** A light colour: a light name ("lagoon"), or any CSS colour (the bracelet's own RGB). */
  function lightValue(light) {
    if (!light) return LIGHTS.firefly;
    return LIGHTS[light] || light;
  }

  function lightStyle(light, style) {
    return Object.assign({ "--lp-light": lightValue(light) }, style || {});
  }

  function cx() {
    return Array.prototype.filter.call(arguments, Boolean).join(" ");
  }

  // Deterministic pseudo-random numbers, so a field looks the same on every render.
  function rng(seed) {
    var s = seed >>> 0 || 1;
    return function () {
      s = (s * 1664525 + 1013904223) >>> 0;
      return s / 4294967296;
    };
  }

  // ---- Icon: 24px line icons, 2px stroke, round caps and joins ---------------------------
  var ICONS = {
    play: "M8 6.5v11a1 1 0 0 0 1.5.86l9-5.5a1 1 0 0 0 0-1.72l-9-5.5A1 1 0 0 0 8 6.5z",
    stop: "M8.5 6.5h7a2 2 0 0 1 2 2v7a2 2 0 0 1-2 2h-7a2 2 0 0 1-2-2v-7a2 2 0 0 1 2-2z",
    plus: "M12 5v14M5 12h14",
    close: "M6.5 6.5l11 11M17.5 6.5l-11 11",
    chevron: "M9.5 6l6 6-6 6",
    bluetooth: "M7 7.5l10 9-5 4.5V3l5 4.5-10 9",
    sliders: "M4 8h9M17 8h3M4 16h3M11 16h9M15 6v4M9 14v4",
    ripple: "M12 12m-2 0a2 2 0 1 0 4 0a2 2 0 1 0-4 0M7.8 7.8a6 6 0 0 0 0 8.4M16.2 7.8a6 6 0 0 1 0 8.4M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14",
    wave: "M3 12c2-4 4-4 6 0s4 4 6 0 4-4 6 0"
  };

  function Icon(props) {
    var size = props.size || 24;
    return h(
      "svg",
      {
        className: cx("lp-icon", props.className),
        width: size,
        height: size,
        viewBox: "0 0 24 24",
        fill: "none",
        stroke: "currentColor",
        strokeWidth: 2,
        strokeLinecap: "round",
        strokeLinejoin: "round",
        "aria-hidden": props.label ? undefined : true,
        "aria-label": props.label,
        role: props.label ? "img" : undefined
      },
      h("path", { d: ICONS[props.name] || "" })
    );
  }

  // ---- LightField: every screen's base -----------------------------------------------------
  // Where the light sources sit, spread over the field so they meet at their edges.
  var SPOTS = [[18, 22], [84, 40], [36, 86], [78, 92]];
  // Big soft light sources drifting in the void, blending where they meet, with motes drifting
  // nearer. Layers move by their depth when the pointer moves (the gyroscope, in the app).
  function LightField(props) {
    var lights = props.lights || ["lagoon", "bloom", "firefly"];
    var r = rng(props.seed || 5);
    var rootRef = React.useRef(null);

    function onMove(e) {
      if (props.parallax === false || !rootRef.current) return;
      var box = rootRef.current.getBoundingClientRect();
      var x = ((e.clientX - box.left) / box.width - 0.5) * 2;
      var y = ((e.clientY - box.top) / box.height - 0.5) * 2;
      rootRef.current.style.setProperty("--lp-tilt-x", x.toFixed(3));
      rootRef.current.style.setProperty("--lp-tilt-y", y.toFixed(3));
    }

    var sources = lights.map(function (light, i) {
      return h("span", {
        key: "l" + i,
        className: "lp-source lp-source-" + (i % 4),
        style: lightStyle(light, {
          left: (SPOTS[i % 4][0] + (r() - 0.5) * 16).toFixed(1) + "%",
          top: (SPOTS[i % 4][1] + (r() - 0.5) * 16).toFixed(1) + "%",
          width: (50 + r() * 30).toFixed(0) + "%",
          animationDelay: (-r() * 14).toFixed(1) + "s"
        })
      });
    });
    var motes = [];
    var count = props.motes == null ? 14 : props.motes;
    for (var j = 0; j < count; j++) {
      motes.push(
        h("span", {
          key: "m" + j,
          className: "lp-mote",
          style: lightStyle(lights[j % lights.length], {
            left: (r() * 100).toFixed(1) + "%",
            top: (r() * 100).toFixed(1) + "%",
            animationDelay: (-r() * 9).toFixed(1) + "s",
            opacity: (0.35 + r() * 0.6).toFixed(2)
          })
        })
      );
    }
    return h(
      "div",
      { ref: rootRef, className: cx("lp-field", props.className), style: props.style, onPointerMove: onMove },
      h("div", { className: "lp-layer lp-layer-far", "aria-hidden": true }, sources),
      h("div", { className: "lp-layer lp-layer-back", "aria-hidden": true }, motes),
      h("div", { className: "lp-layer-ui" }, props.children)
    );
  }

  // ---- LightOrb: a single light source ---------------------------------------------------------
  function LightOrb(props) {
    var size = props.size || 64;
    return h("span", {
      className: cx("lp-orb", props.pulse && "lp-orb-pulse", props.className),
      style: lightStyle(props.light, { width: size, height: size }),
      role: props.label ? "img" : undefined,
      "aria-label": props.label,
      "aria-hidden": props.label ? undefined : true
    });
  }

  // ---- Glass ---------------------------------------------------------------------------------------
  function Glass(props) {
    return h(
      "section",
      {
        className: cx("lp-glass", props.light && "lp-glass-lit", props.className),
        style: props.light ? lightStyle(props.light) : undefined,
        "aria-label": props.label
      },
      props.title ? h("h3", { className: "lp-glass-title" }, props.title) : null,
      props.children
    );
  }

  // ---- Button / IconButton -------------------------------------------------------------------------
  function Button(props) {
    var variant = props.variant || "glass";
    var rest = Object.assign({}, props);
    ["variant", "icon", "size", "block", "light"].forEach(function (k) { delete rest[k]; });
    rest.className = cx("lp-btn", "lp-btn-" + variant, props.size === "s" && "lp-btn-s", props.block && "lp-btn-block", props.className);
    rest.style = lightStyle(props.light, props.style);
    rest.type = props.type || "button";
    return h("button", rest, props.icon ? h(Icon, { name: props.icon, size: 20 }) : null, h("span", null, props.children));
  }

  function IconButton(props) {
    var rest = Object.assign({}, props);
    ["variant", "icon", "label", "light"].forEach(function (k) { delete rest[k]; });
    rest.className = cx("lp-iconbtn", "lp-btn-" + (props.variant || "glass"), props.className);
    rest.style = lightStyle(props.light, props.style);
    rest.type = props.type || "button";
    rest["aria-label"] = props.label;
    return h("button", rest, h(Icon, { name: props.icon, size: 22 }));
  }

  // ---- ListRow ---------------------------------------------------------------------------------------
  function ListRow(props) {
    var body = [
      props.light
        ? h("span", { key: "o", className: "lp-row-light", style: lightStyle(props.light) })
        : props.leading
          ? h("span", { key: "l", className: "lp-row-leading" }, props.leading)
          : null,
      h(
        "span",
        { key: "t", className: "lp-row-text" },
        h("span", { className: "lp-row-title" }, props.title),
        props.subtitle ? h("span", { className: "lp-row-subtitle" }, props.subtitle) : null
      ),
      props.trailing != null ? h("span", { key: "v", className: "lp-row-trailing" }, props.trailing) : null,
      props.onClick ? h(Icon, { key: "c", name: "chevron", size: 18, className: "lp-row-chevron" }) : null
    ];
    return props.onClick
      ? h("button", { type: "button", className: "lp-row lp-row-button", onClick: props.onClick }, body)
      : h("div", { className: "lp-row" }, body);
  }

  // ---- Toggle ------------------------------------------------------------------------------------------
  function Toggle(props) {
    var on = !!props.checked;
    var sw = h(
      "button",
      {
        type: "button",
        role: "switch",
        "aria-checked": on,
        "aria-label": props.showLabel ? undefined : props.label,
        disabled: props.disabled,
        className: cx("lp-toggle", on && "lp-toggle-on"),
        style: lightStyle(props.light),
        onClick: function () { if (props.onChange) props.onChange(!on); }
      },
      h("span", { className: "lp-toggle-knob" })
    );
    if (!props.showLabel) return sw;
    return h("label", { className: "lp-toggle-field" }, h("span", null, props.label), sw);
  }

  // ---- Slider ------------------------------------------------------------------------------------------
  function Slider(props) {
    var min = props.min == null ? 0 : props.min;
    var max = props.max == null ? 100 : props.max;
    var value = props.value == null ? min : props.value;
    var pct = ((value - min) / (max - min)) * 100;
    var id = props.id || "lp-slider-" + (props.label || "").replace(/\W+/g, "-").toLowerCase();
    var style = lightStyle(props.light);
    if (props.emptyLight) style["--lp-empty"] = lightValue(props.emptyLight);
    return h(
      "div",
      { className: "lp-slider", style: style },
      props.label
        ? h("div", { className: "lp-slider-head" },
            h("label", { htmlFor: id }, props.label),
            h("output", { htmlFor: id }, props.format ? props.format(value) : value))
        : null,
      h("input", {
        id: id, type: "range", min: min, max: max, step: props.step || 1, value: value,
        style: { "--lp-pct": pct + "%" },
        onChange: function (e) { if (props.onChange) props.onChange(Number(e.target.value)); }
      })
    );
  }

  // ---- ColorPicker ---------------------------------------------------------------------------------------
  var BRACELET_COLORS = [
    { name: "Firefly", value: "firefly" }, { name: "Leaf", value: "leaf" }, { name: "Lagoon", value: "lagoon" },
    { name: "Dusk", value: "dusk" }, { name: "Bloom", value: "bloom" }, { name: "Ember", value: "ember" }, { name: "Amber", value: "amber" }
  ];

  function ColorPicker(props) {
    var colors = props.colors || BRACELET_COLORS;
    return h(
      "div",
      { className: "lp-colors", role: "radiogroup", "aria-label": props.label || "Colour" },
      colors.map(function (c) {
        var selected = c.value === props.value;
        return h("button", {
          key: c.value,
          type: "button",
          role: "radio",
          "aria-checked": selected,
          "aria-label": c.name,
          className: cx("lp-color", selected && "lp-color-on"),
          style: lightStyle(c.value),
          onClick: function () { if (props.onChange) props.onChange(c.value); }
        });
      })
    );
  }

  // ---- StatusPill ------------------------------------------------------------------------------------------
  var STATUS = {
    connected: { text: "Connected", light: "firefly" },
    searching: { text: "Searching", light: "amber" },
    connecting: { text: "Connecting", light: "lagoon" },
    off: { text: "Not connected", light: null },
    error: { text: "Can't connect", light: "ember" }
  };
  function StatusPill(props) {
    var state = STATUS[props.state] ? props.state : "off";
    var info = STATUS[state];
    return h(
      "span",
      { className: cx("lp-status", "lp-status-" + state), role: "status", style: info.light ? lightStyle(props.light || info.light) : undefined },
      h("span", { className: "lp-status-dot", "aria-hidden": true }),
      props.children || info.text
    );
  }

  // ---- FireflyLoader and Logo --------------------------------------------------------------------------------
  function FireflyLoader(props) {
    var size = props.size || 120;
    return h(
      "div",
      { className: cx("lp-loader", props.className), role: "status", style: lightStyle(props.light, { width: size, height: size }) },
      h("span", { className: "lp-loader-halo", "aria-hidden": true }),
      h("img", { className: "lp-loader-mark", src: MARK_SRC, alt: "", width: size, height: size }),
      h("span", { className: "lp-sr" }, props.label || "Loading")
    );
  }

  function Logo(props) {
    var size = props.size || 96;
    var row = props.layout === "row";
    return h(
      "div",
      { className: cx("lp-logo", row && "lp-logo-row", props.className) },
      h("img", { src: MARK_SRC, width: size, height: size, alt: props.wordmark === false ? "Dotty" : "" }),
      props.wordmark === false ? null : h("span", { className: "lp-wordmark", style: { fontSize: Math.round(size * (row ? 0.5 : 0.36)) } }, "dotty")
    );
  }

  window.Dotty = Object.assign(window.Dotty || {}, {
    LightField: LightField, LightOrb: LightOrb, Glass: Glass, Button: Button, IconButton: IconButton,
    ListRow: ListRow, Toggle: Toggle, Slider: Slider, ColorPicker: ColorPicker, StatusPill: StatusPill,
    FireflyLoader: FireflyLoader, Logo: Logo, Icon: Icon, markSrc: MARK_SRC, lights: Object.keys(LIGHTS)
  });
})();
