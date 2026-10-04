/* @ds-bundle: {"format":4,"namespace":"Dotty","components":[{"name":"LightField"},{"name":"LightOrb"},{"name":"Glass"},{"name":"Button"},{"name":"IconButton"},{"name":"ListRow"},{"name":"Toggle"},{"name":"Slider"},{"name":"ColorPicker"},{"name":"StatusPill"},{"name":"FireflyLoader"},{"name":"Logo"},{"name":"Icon"}]} */
(function () {
  var React = window.React;
  var h = React.createElement;

  // The full-colour firefly mark, injected by build.py from Design/Logo/dotty-mark.svg.
  var MARK_SRC = "data:image/svg+xml,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20viewBox%3D%220%200%201024%201024%22%20width%3D%221024%22%20height%3D%221024%22%3E%3Cdefs%3E%0A%3CradialGradient%20id%3D%22halo%22%20cx%3D%22512%22%20cy%3D%22700%22%20r%3D%22310%22%20gradientUnits%3D%22userSpaceOnUse%22%3E%0A%20%20%3Cstop%20offset%3D%220%22%20stop-color%3D%22%23F3F27A%22%20stop-opacity%3D%220.6%22%2F%3E%3Cstop%20offset%3D%220.4%22%20stop-color%3D%22%23B8E04E%22%20stop-opacity%3D%220.2%22%2F%3E%0A%20%20%3Cstop%20offset%3D%221%22%20stop-color%3D%22%237BC043%22%20stop-opacity%3D%220%22%2F%3E%3C%2FradialGradient%3E%0A%3ClinearGradient%20id%3D%22abdomen%22%20x1%3D%220%22%20y1%3D%22420%22%20x2%3D%220%22%20y2%3D%22880%22%20gradientUnits%3D%22userSpaceOnUse%22%3E%0A%20%20%3Cstop%20offset%3D%220%22%20stop-color%3D%22%234A3A1C%22%2F%3E%3Cstop%20offset%3D%220.38%22%20stop-color%3D%22%238D9A33%22%2F%3E%0A%20%20%3Cstop%20offset%3D%220.6%22%20stop-color%3D%22%23E8EE62%22%2F%3E%3Cstop%20offset%3D%221%22%20stop-color%3D%22%23FFFBD2%22%2F%3E%3C%2FlinearGradient%3E%0A%3ClinearGradient%20id%3D%22wing%22%20x1%3D%220%22%20y1%3D%22380%22%20x2%3D%220%22%20y2%3D%22840%22%20gradientUnits%3D%22userSpaceOnUse%22%3E%0A%20%20%3Cstop%20offset%3D%220%22%20stop-color%3D%22%237D5732%22%2F%3E%3Cstop%20offset%3D%220.6%22%20stop-color%3D%22%235A3B21%22%2F%3E%3Cstop%20offset%3D%221%22%20stop-color%3D%22%233A2413%22%2F%3E%3C%2FlinearGradient%3E%0A%3CradialGradient%20id%3D%22wingShade%22%20cx%3D%22420%22%20cy%3D%22470%22%20r%3D%22420%22%20gradientUnits%3D%22userSpaceOnUse%22%3E%0A%20%20%3Cstop%20offset%3D%220%22%20stop-color%3D%22%23000%22%20stop-opacity%3D%220%22%2F%3E%3Cstop%20offset%3D%220.75%22%20stop-color%3D%22%23000%22%20stop-opacity%3D%220.05%22%2F%3E%0A%20%20%3Cstop%20offset%3D%221%22%20stop-color%3D%22%23000%22%20stop-opacity%3D%220.35%22%2F%3E%3C%2FradialGradient%3E%0A%3CradialGradient%20id%3D%22shield%22%20cx%3D%22470%22%20cy%3D%22330%22%20r%3D%22200%22%20gradientUnits%3D%22userSpaceOnUse%22%3E%0A%20%20%3Cstop%20offset%3D%220%22%20stop-color%3D%22%23F0C576%22%2F%3E%3Cstop%20offset%3D%220.65%22%20stop-color%3D%22%23D0913F%22%2F%3E%3Cstop%20offset%3D%221%22%20stop-color%3D%22%23A06628%22%2F%3E%3C%2FradialGradient%3E%0A%3Cfilter%20id%3D%22rim%22%20x%3D%22-200%25%22%20y%3D%22-20%25%22%20width%3D%22500%25%22%20height%3D%22140%25%22%3E%3CfeGaussianBlur%20stdDeviation%3D%226%22%2F%3E%3C%2Ffilter%3E%0A%3Cfilter%20id%3D%22soft%22%20x%3D%22-20%25%22%20y%3D%22-20%25%22%20width%3D%22140%25%22%20height%3D%22140%25%22%3E%3CfeGaussianBlur%20stdDeviation%3D%2214%22%2F%3E%3C%2Ffilter%3E%0A%3Cfilter%20id%3D%22bloom%22%20x%3D%22-50%25%22%20y%3D%22-50%25%22%20width%3D%22200%25%22%20height%3D%22200%25%22%3E%3CfeGaussianBlur%20stdDeviation%3D%2228%22%2F%3E%3C%2Ffilter%3E%0A%3C%2Fdefs%3E%0A%3Ccircle%20cx%3D%22512%22%20cy%3D%22700%22%20r%3D%22310%22%20fill%3D%22url%28%23halo%29%22%2F%3E%0A%3Cpath%20d%3D%22M482%20286%20C%20462%20216%2C%20420%20178%2C%20366%20166%22%20fill%3D%22none%22%20stroke%3D%22%233A2413%22%20stroke-width%3D%2222%22%20stroke-linecap%3D%22round%22%2F%3E%0A%3Cpath%20d%3D%22M542%20286%20C%20562%20216%2C%20604%20178%2C%20658%20166%22%20fill%3D%22none%22%20stroke%3D%22%233A2413%22%20stroke-width%3D%2222%22%20stroke-linecap%3D%22round%22%2F%3E%0A%3Cellipse%20cx%3D%22512%22%20cy%3D%22298%22%20rx%3D%2262%22%20ry%3D%2246%22%20fill%3D%22%233A2413%22%2F%3E%0A%3Cellipse%20cx%3D%22512%22%20cy%3D%22790%22%20rx%3D%22120%22%20ry%3D%2290%22%20fill%3D%22%23F6EE6A%22%20opacity%3D%220.8%22%20filter%3D%22url%28%23bloom%29%22%2F%3E%0A%3Cpath%20d%3D%22M512%20400%20C%20610%20400%2C%20640%20560%2C%20632%20700%20C%20626%20800%2C%20580%20880%2C%20512%20880%20C%20444%20880%2C%20398%20800%2C%20392%20700%20C%20384%20560%2C%20414%20400%2C%20512%20400%20Z%22%20fill%3D%22url%28%23abdomen%29%22%2F%3E%0A%3Cg%20fill%3D%22none%22%20stroke%3D%22%235E6A1E%22%20stroke-opacity%3D%220.45%22%20stroke-width%3D%227%22%20stroke-linecap%3D%22round%22%3E%0A%20%20%3Cpath%20d%3D%22M404%20640%20Q%20512%20668%20620%20640%22%2F%3E%3Cpath%20d%3D%22M410%20718%20Q%20512%20746%20614%20718%22%2F%3E%3Cpath%20d%3D%22M428%20792%20Q%20512%20816%20596%20792%22%2F%3E%0A%3C%2Fg%3E%0A%3Cellipse%20cx%3D%22512%22%20cy%3D%22830%22%20rx%3D%2256%22%20ry%3D%2228%22%20fill%3D%22%23FFFFF0%22%20opacity%3D%220.7%22%20filter%3D%22url%28%23soft%29%22%2F%3E%0A%3Cg%20transform%3D%22rotate%2812%20508%20380%29%22%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22%231E140A%22%20opacity%3D%220.45%22%20filter%3D%22url%28%23soft%29%22%20transform%3D%22translate%2810%208%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22url%28%23wing%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22url%28%23wingShade%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M318%20462%20C%20350%20424%2C%20410%20404%2C%20462%20402%22%20fill%3D%22none%22%20stroke%3D%22%23FFF4DC%22%20stroke-opacity%3D%220.45%22%20stroke-width%3D%2224%22%20stroke-linecap%3D%22round%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M504%20480%20L%20504%20770%22%20fill%3D%22none%22%20stroke%3D%22%23F6EE6A%22%20stroke-opacity%3D%220.55%22%20stroke-width%3D%2222%22%20stroke-linecap%3D%22round%22%20filter%3D%22url%28%23rim%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M507%20500%20L%20507%20768%22%20fill%3D%22none%22%20stroke%3D%22%23FBF5A8%22%20stroke-opacity%3D%220.7%22%20stroke-width%3D%224%22%20stroke-linecap%3D%22round%22%2F%3E%0A%3C%2Fg%3E%0A%3Cg%20transform%3D%22translate%281024%200%29%20scale%28-1%201%29%20rotate%2812%20508%20380%29%22%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22%231E140A%22%20opacity%3D%220.45%22%20filter%3D%22url%28%23soft%29%22%20transform%3D%22translate%2810%208%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22url%28%23wing%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M508%20372%20C%20400%20360%2C%20236%20410%2C%20228%20580%20C%20222%20740%2C%20350%20840%2C%20462%20830%20C%20494%20827%2C%20508%20806%2C%20508%20770%20Z%22%20fill%3D%22url%28%23wingShade%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M318%20462%20C%20350%20424%2C%20410%20404%2C%20462%20402%22%20fill%3D%22none%22%20stroke%3D%22%23FFF4DC%22%20stroke-opacity%3D%220.2%22%20stroke-width%3D%2224%22%20stroke-linecap%3D%22round%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M504%20480%20L%20504%20770%22%20fill%3D%22none%22%20stroke%3D%22%23F6EE6A%22%20stroke-opacity%3D%220.55%22%20stroke-width%3D%2222%22%20stroke-linecap%3D%22round%22%20filter%3D%22url%28%23rim%29%22%2F%3E%0A%20%20%3Cpath%20d%3D%22M507%20500%20L%20507%20768%22%20fill%3D%22none%22%20stroke%3D%22%23FBF5A8%22%20stroke-opacity%3D%220.7%22%20stroke-width%3D%224%22%20stroke-linecap%3D%22round%22%2F%3E%0A%3C%2Fg%3E%0A%3Cpath%20d%3D%22M372%20432%20C%20336%20432%2C%20330%20398%2C%20346%20372%20C%20386%20286%2C%20638%20286%2C%20678%20372%20C%20694%20398%2C%20688%20432%2C%20652%20432%20C%20590%20450%2C%20434%20450%2C%20372%20432%20Z%22%20fill%3D%22%231E140A%22%20opacity%3D%220.5%22%20filter%3D%22url%28%23soft%29%22%20transform%3D%22translate%280%2018%29%22%2F%3E%0A%3Cpath%20d%3D%22M372%20432%20C%20336%20432%2C%20330%20398%2C%20346%20372%20C%20386%20286%2C%20638%20286%2C%20678%20372%20C%20694%20398%2C%20688%20432%2C%20652%20432%20C%20590%20450%2C%20434%20450%2C%20372%20432%20Z%22%20fill%3D%22url%28%23shield%29%22%2F%3E%0A%3Cpath%20d%3D%22M408%20364%20C%20426%20334%2C%20460%20318%2C%20494%20314%22%20fill%3D%22none%22%20stroke%3D%22%23FFF4DC%22%20stroke-opacity%3D%220.6%22%20stroke-width%3D%2216%22%20stroke-linecap%3D%22round%22%2F%3E%0A%3C%2Fsvg%3E%0A";

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
