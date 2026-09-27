/* Reactive node field behind the page: drifting dots, webbed links, cursor push,
   press-and-hold pull, click shockwaves.

   Built to be exact rather than approximate:
   - every node is moved first and drawn second, so links and cursor lines always
     end precisely on their dots (no one-frame lag between a dot and its lines)
   - neighbours come from a uniform grid: true distances, every pair checked once
   - physics is delta-timed to 60fps, so 60Hz, 120Hz and 144Hz screens behave the same
   - pointer speed is measured against event timestamps, so a 1000Hz mouse and a
     60Hz trackpad push the field equally hard, and it decays to zero when you stop
   - the canvas matches its real on-screen box at the device's pixel ratio (up to 2x),
     and resizing keeps every node where it was instead of reseeding the field
   - a lifted finger or a cursor leaving the window releases the field right away,
     so there are no leftover links to a stale position
   - colours follow the site's theme toggle as well as the OS setting */
(() => {
  const canvas = document.getElementById("bg-canvas");
  if (!canvas || !canvas.getContext) return;
  const ctx = canvas.getContext("2d");
  const root = document.documentElement;
  const reduceMq = matchMedia("(prefers-reduced-motion: reduce)");
  const darkMq = matchMedia("(prefers-color-scheme: dark)");

  const THEMES = {
    light: { dots: ["#2563EB", "#7C3AED", "#0891B2", "#3B82F6", "#8B5CF6", "#0EA5E9"], link: "59,91,160", linkA: 0.36, grab: "37,99,235", grabA: 0.5, ring: "37,99,235", ring2: "124,58,237" },
    dark: { dots: ["#60A5FA", "#A78BFA", "#22D3EE", "#93C5FD", "#C4B5FD", "#38BDF8"], link: "148,180,255", linkA: 0.3, grab: "147,197,253", grabA: 0.45, ring: "147,197,253", ring2: "167,139,250" },
  };
  const isDark = () => {
    const t = root.dataset.theme;
    return t === "dark" || (t !== "light" && darkMq.matches);
  };
  let theme = THEMES[isDark() ? "dark" : "light"];

  const MARGIN = 24;     // nodes wrap this far outside the edges so they never pop in
  const CRUISE = 0.36;   // natural drift, px per 60fps frame
  const MAX_SPEED = 4;
  const LEVELS = 12;     // link opacities are batched into this many paths per frame

  let W = 0, H = 0, dpr = 1, small = false;
  let linkDist = 130, grabDist = 190;
  let nodes = [], sparks = [], waves = [];
  let simT = 0;
  const pointer = { x: 0, y: 0, vx: 0, vy: 0, t: 0, active: false, down: false, type: "mouse" };

  /* ---------- nodes ---------- */

  function makeNode(x, y, fadeIn) {
    const ang = Math.random() * Math.PI * 2;
    const sp = CRUISE * (0.35 + Math.random() * 0.9);
    const bvx = Math.cos(ang) * sp, bvy = Math.sin(ang) * sp;
    return {
      x, y, vx: bvx, vy: bvy, bvx, bvy,
      r: 0.9 + Math.random() * 1.6,
      c: (Math.random() * 6) | 0,
      tw: Math.random() * Math.PI * 2,
      ts: 0.02 + Math.random() * 0.035,
      a: fadeIn ? 0 : 1,
    };
  }

  function targetCount() {
    const per = small ? 8500 : 7000;
    return Math.max(24, Math.min(small ? 80 : 190, Math.round((W * H) / per)));
  }

  // add or remove nodes to match the viewport's area; existing nodes stay put
  function fitCount(fadeIn) {
    const want = targetCount();
    while (nodes.length < want) nodes.push(makeNode(Math.random() * W, Math.random() * H, fadeIn));
    if (nodes.length > want) nodes.length = want;
  }

  function resize() {
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return;
    const nextDpr = Math.min(window.devicePixelRatio || 1, 2);
    if (w === W && h === H && nextDpr === dpr) return;

    // width changed (window resize, rotation): scale positions so the field keeps its shape.
    // height-only changes (mobile toolbars sliding) leave nodes exactly where they are.
    if (W && H && w !== W) {
      const sx = w / W, sy = h / H;
      for (const n of nodes) { n.x *= sx; n.y *= sy; }
    }
    W = w; H = h; dpr = nextDpr;
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    small = Math.min(W, H) < 600;
    linkDist = small ? 108 : 132;
    grabDist = small ? 150 : 200;
    fitCount(!reduceMq.matches);
    if (!running()) draw(false);
  }

  /* ---------- neighbour grid ---------- */

  let head = new Int32Array(0), next = new Int32Array(0), cols = 0, rows = 0;
  const linkSegs = Array.from({ length: LEVELS }, () => []);
  const grabSegs = Array.from({ length: LEVELS }, () => []);

  function buildGrid() {
    const cell = linkDist;
    cols = Math.max(1, Math.ceil((W + MARGIN * 2) / cell));
    rows = Math.max(1, Math.ceil((H + MARGIN * 2) / cell));
    const size = cols * rows;
    if (head.length < size) head = new Int32Array(size);
    head.fill(-1, 0, size);
    if (next.length < nodes.length) next = new Int32Array(nodes.length + 64);
    for (let i = 0; i < nodes.length; i++) {
      const n = nodes[i];
      const cx = Math.min(cols - 1, Math.max(0, ((n.x + MARGIN) / cell) | 0));
      const cy = Math.min(rows - 1, Math.max(0, ((n.y + MARGIN) / cell) | 0));
      const k = cy * cols + cx;
      next[i] = head[k];
      head[k] = i;
    }
  }

  function pair(a, b, L2) {
    const dx = a.x - b.x, dy = a.y - b.y;
    const d2 = dx * dx + dy * dy;
    if (d2 >= L2) return;
    const strength = (1 - Math.sqrt(d2) / linkDist) * Math.min(a.a, b.a);
    if (strength <= 0.01) return;
    linkSegs[Math.min(LEVELS - 1, (strength * LEVELS) | 0)].push(a.x, a.y, b.x, b.y);
  }

  // each pair once: same cell (later entries only) plus the four "forward" cells
  function collectLinks() {
    for (const s of linkSegs) s.length = 0;
    const L2 = linkDist * linkDist;
    for (let cy = 0; cy < rows; cy++) {
      for (let cx = 0; cx < cols; cx++) {
        for (let i = head[cy * cols + cx]; i !== -1; i = next[i]) {
          const a = nodes[i];
          for (let j = next[i]; j !== -1; j = next[j]) pair(a, nodes[j], L2);
          if (cx + 1 < cols) for (let j = head[cy * cols + cx + 1]; j !== -1; j = next[j]) pair(a, nodes[j], L2);
          if (cy + 1 < rows) {
            const r = (cy + 1) * cols;
            if (cx > 0) for (let j = head[r + cx - 1]; j !== -1; j = next[j]) pair(a, nodes[j], L2);
            for (let j = head[r + cx]; j !== -1; j = next[j]) pair(a, nodes[j], L2);
            if (cx + 1 < cols) for (let j = head[r + cx + 1]; j !== -1; j = next[j]) pair(a, nodes[j], L2);
          }
        }
      }
    }
  }

  function strokeLevels(segs, rgb, peak, width) {
    ctx.lineWidth = width;
    for (let k = 0; k < LEVELS; k++) {
      const s = segs[k];
      if (!s.length) continue;
      ctx.strokeStyle = "rgba(" + rgb + "," + (((k + 0.5) / LEVELS) * peak).toFixed(3) + ")";
      ctx.beginPath();
      for (let i = 0; i < s.length; i += 4) { ctx.moveTo(s[i], s[i + 1]); ctx.lineTo(s[i + 2], s[i + 3]); }
      ctx.stroke();
    }
  }

  /* ---------- simulation ---------- */

  function blast(x, y, big) {
    waves.push({ x, y, r: 6, a: 0.55, max: big ? 480 : 320, big });
    const count = big ? 40 : 24;
    for (let i = 0; i < count; i++) {
      const ang = Math.random() * Math.PI * 2;
      const sp = 1.4 + Math.random() * (big ? 6.5 : 4.5);
      sparks.push({ x, y, vx: Math.cos(ang) * sp, vy: Math.sin(ang) * sp, r: 0.9 + Math.random() * 2.2, c: (Math.random() * 6) | 0, life: 0.85 + Math.random() * 0.6, decay: 0.018 });
    }
  }

  function step(dt) {
    simT += dt / 60;
    const relax = 1 - Math.pow(0.975, dt); // pull back toward each node's own drift
    const G = grabDist, G2 = G * G;
    const pOn = pointer.active;
    const pSpeed = Math.min(Math.hypot(pointer.vx, pointer.vy), 30);

    for (const n of nodes) {
      if (n.a < 1) n.a = Math.min(1, n.a + 0.025 * dt);
      n.tw += n.ts * dt;

      // slow ambient current so the field never looks static
      n.vx += Math.sin(n.y * 0.008 + simT * 0.35) * 0.0035 * dt;
      n.vy += Math.cos(n.x * 0.008 + simT * 0.35) * 0.0035 * dt;

      if (pOn) {
        const dx = n.x - pointer.x, dy = n.y - pointer.y;
        const d2 = dx * dx + dy * dy;
        if (d2 < G2 && d2 > 0.25) {
          const d = Math.sqrt(d2), ux = dx / d, uy = dy / d;
          const fall = 1 - d / G; // 1 at the pointer, 0 at the edge of its reach
          if (pointer.down) {
            // press and hold: pull in, softened near the centre so nodes orbit instead of collapsing
            const pull = fall * 0.15 * Math.min(1, d / 28);
            n.vx -= ux * pull * dt;
            n.vy -= uy * pull * dt;
          } else {
            // hover: a soft push that grows with pointer speed, plus a little drag along its path
            const push = fall * fall * (0.08 + pSpeed * 0.012);
            n.vx += (ux * push + pointer.vx * fall * 0.018 - uy * fall * 0.01) * dt;
            n.vy += (uy * push + pointer.vy * fall * 0.018 + ux * fall * 0.01) * dt;
          }
        }
      }

      for (const w of waves) {
        const dx = n.x - w.x, dy = n.y - w.y;
        const d = Math.hypot(dx, dy);
        const off = Math.abs(d - w.r);
        if (off < 56 && d > 1) {
          const f = (1 - off / 56) * 1.9 * (w.a / 0.55) * dt;
          n.vx += (dx / d) * f;
          n.vy += (dy / d) * f;
        }
      }

      n.vx += (n.bvx - n.vx) * relax;
      n.vy += (n.bvy - n.vy) * relax;
      const sp = Math.hypot(n.vx, n.vy);
      if (sp > MAX_SPEED) { n.vx *= MAX_SPEED / sp; n.vy *= MAX_SPEED / sp; }
      n.x += n.vx * dt;
      n.y += n.vy * dt;

      if (n.x < -MARGIN) n.x += W + MARGIN * 2;
      else if (n.x > W + MARGIN) n.x -= W + MARGIN * 2;
      if (n.y < -MARGIN) n.y += H + MARGIN * 2;
      else if (n.y > H + MARGIN) n.y -= H + MARGIN * 2;
    }

    // a still pointer has no speed, even though no event says so
    const pd = Math.pow(0.82, dt);
    pointer.vx *= pd;
    pointer.vy *= pd;

    const sd = Math.pow(0.965, dt);
    for (const s of sparks) {
      s.vx *= sd; s.vy *= sd; s.vy += 0.02 * dt;
      s.x += s.vx * dt; s.y += s.vy * dt;
      s.life -= s.decay * dt;
    }
    sparks = sparks.filter((s) => s.life > 0);

    for (const w of waves) { w.r += 9.5 * dt; w.a *= Math.pow(0.945, dt); }
    waves = waves.filter((w) => w.a > 0.02 && w.r < w.max);
  }

  /* ---------- drawing ---------- */

  function draw(animated) {
    ctx.clearRect(0, 0, W, H);
    if (!nodes.length) return;

    buildGrid();
    collectLinks();
    strokeLevels(linkSegs, theme.link, theme.linkA, 1);

    if (animated && pointer.active) {
      for (const s of grabSegs) s.length = 0;
      const G = grabDist, G2 = G * G;
      for (const n of nodes) {
        const dx = n.x - pointer.x, dy = n.y - pointer.y;
        const d2 = dx * dx + dy * dy;
        if (d2 >= G2) continue;
        const strength = (1 - Math.sqrt(d2) / G) * n.a;
        if (strength <= 0.01) continue;
        grabSegs[Math.min(LEVELS - 1, (strength * LEVELS) | 0)].push(pointer.x, pointer.y, n.x, n.y);
      }
      strokeLevels(grabSegs, theme.grab, theme.grabA, 1);
    }

    for (const n of nodes) {
      ctx.globalAlpha = n.a * (animated ? 0.6 + Math.sin(n.tw) * 0.35 : 0.75);
      ctx.fillStyle = theme.dots[n.c];
      ctx.beginPath();
      ctx.arc(n.x, n.y, n.r, 0, Math.PI * 2);
      ctx.fill();
    }

    for (const s of sparks) {
      ctx.globalAlpha = Math.min(1, s.life);
      ctx.fillStyle = theme.dots[s.c];
      ctx.beginPath();
      ctx.arc(s.x, s.y, s.r, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.globalAlpha = 1;

    for (const w of waves) {
      ctx.lineWidth = w.big ? 2.5 : 2;
      ctx.strokeStyle = "rgba(" + theme.ring + "," + w.a.toFixed(3) + ")";
      ctx.beginPath(); ctx.arc(w.x, w.y, w.r, 0, Math.PI * 2); ctx.stroke();
      ctx.lineWidth = 1.5;
      ctx.strokeStyle = "rgba(" + theme.ring2 + "," + (w.a * 0.5).toFixed(3) + ")";
      ctx.beginPath(); ctx.arc(w.x, w.y, w.r * 0.7, 0, Math.PI * 2); ctx.stroke();
    }
  }

  /* ---------- loop: one guarded rAF, paused when hidden or when motion is reduced ---------- */

  let raf = 0, last = 0;
  const running = () => !reduceMq.matches && !document.hidden;
  function frame(now) {
    raf = 0;
    if (!running()) return;
    const dt = Math.min(Math.max((now - last) / 16.667, 0.25), 3);
    last = now;
    step(dt);
    draw(true);
    raf = requestAnimationFrame(frame);
  }
  function play() {
    if (raf || !running()) return;
    last = performance.now();
    raf = requestAnimationFrame(frame);
  }
  function pause() {
    if (raf) cancelAnimationFrame(raf);
    raf = 0;
  }

  /* ---------- input ---------- */

  function local(e) {
    const r = canvas.getBoundingClientRect();
    return [e.clientX - r.left, e.clientY - r.top];
  }
  function release() {
    pointer.active = false;
    pointer.down = false;
    pointer.vx = pointer.vy = 0;
  }

  addEventListener("pointermove", (e) => {
    const [x, y] = local(e);
    const t = e.timeStamp || performance.now();
    if (pointer.active && pointer.type === e.pointerType) {
      const gap = t - pointer.t;
      if (gap > 0 && gap < 120) {
        // px per 60fps frame, lightly smoothed so noisy high-rate mice read steadily
        const k = 16.667 / Math.max(gap, 4);
        pointer.vx = pointer.vx * 0.35 + (x - pointer.x) * k * 0.65;
        pointer.vy = pointer.vy * 0.35 + (y - pointer.y) * k * 0.65;
        const sp = Math.hypot(pointer.vx, pointer.vy);
        if (sp > 60) { pointer.vx *= 60 / sp; pointer.vy *= 60 / sp; }
      }
    } else {
      pointer.vx = pointer.vy = 0;
    }
    pointer.x = x; pointer.y = y; pointer.t = t;
    pointer.type = e.pointerType;
    pointer.active = true;

    const sp = Math.hypot(pointer.vx, pointer.vy);
    if (running() && sp > 18 && sparks.length < 220) {
      sparks.push({ x, y, vx: -pointer.vx * 0.04 + (Math.random() - 0.5), vy: -pointer.vy * 0.04 + (Math.random() - 0.5), r: 0.8 + Math.random() * 1.4, c: (Math.random() * 6) | 0, life: 0.55, decay: 0.022 });
    }
  }, { passive: true });

  addEventListener("pointerdown", (e) => {
    if (e.pointerType === "mouse" && e.button !== 0) return;
    const [x, y] = local(e);
    pointer.x = x; pointer.y = y; pointer.t = e.timeStamp || performance.now();
    pointer.type = e.pointerType;
    pointer.active = true;
    pointer.down = true;
  }, { passive: true });

  addEventListener("pointerup", (e) => {
    pointer.down = false;
    if (e.pointerType !== "mouse") release(); // a lifted finger or pen is gone
  }, { passive: true });
  addEventListener("pointercancel", release, { passive: true }); // e.g. a touch that became a scroll
  root.addEventListener("mouseleave", release);
  document.addEventListener("pointerout", (e) => { if (!e.relatedTarget && e.pointerType === "mouse") release(); });
  addEventListener("blur", release);

  addEventListener("click", (e) => {
    if (!running() || e.detail === 0) return; // detail 0 = keyboard-activated, no real position
    if (e.target.closest && e.target.closest("input, textarea, select, label, [contenteditable]")) return;
    const [x, y] = local(e);
    blast(x, y, e.detail >= 2);
  });

  /* ---------- environment ---------- */

  function retheme() {
    theme = THEMES[isDark() ? "dark" : "light"];
    if (!running()) draw(false);
  }
  new MutationObserver(retheme).observe(root, { attributes: true, attributeFilter: ["data-theme"] });
  darkMq.addEventListener("change", retheme);

  reduceMq.addEventListener("change", () => {
    if (reduceMq.matches) { pause(); sparks = []; waves = []; nodes.forEach((n) => { n.a = 1; }); draw(false); }
    else play();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) pause();
    else play();
  });

  if ("ResizeObserver" in window) {
    new ResizeObserver(() => resize()).observe(canvas);
  } else {
    addEventListener("resize", resize);
  }
  // pixel ratio changes (browser zoom, moving to another monitor) don't resize the box
  addEventListener("resize", resize);

  resize();
  play();
})();
