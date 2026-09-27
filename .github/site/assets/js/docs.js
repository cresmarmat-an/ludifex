/* Documentation site behaviour: the theme toggle shared with cresmarmat-an.github.io
   (same storage key, so a choice made on one site holds on the others), the
   sidebar drawer, the "On this page" scroll spy, copy buttons on code, the
   reading-progress bar, and search over an index the build writes. */
(() => {
  const root = document.documentElement;
  const calmMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const siteRoot = root.dataset.root || "./";

  /* ---------- theme: system -> light -> dark ---------- */

  const themeBtn = document.getElementById("theme-toggle");
  const darkMq = matchMedia("(prefers-color-scheme: dark)");
  const THEME_LABEL = { auto: "System", light: "Light", dark: "Dark" };
  const THEME_ORDER = ["auto", "light", "dark"];
  const themeMetas = [...document.querySelectorAll('meta[name="theme-color"]')].map((m) => ({ m, content: m.content, media: m.media }));
  function syncTheme() {
    const mode = THEME_ORDER.includes(root.dataset.theme) ? root.dataset.theme : "auto";
    const effective = mode === "auto" ? (darkMq.matches ? "dark" : "light") : mode;
    const next = THEME_ORDER[(THEME_ORDER.indexOf(mode) + 1) % THEME_ORDER.length];
    if (themeBtn) {
      themeBtn.setAttribute("aria-label", "Color theme: " + THEME_LABEL[mode] + (mode === "auto" ? " (" + effective + ")" : "") + ". Switch to " + THEME_LABEL[next].toLowerCase());
      themeBtn.title = "Theme: " + THEME_LABEL[mode] + (mode === "auto" ? " (" + effective + ")" : "");
    }
    themeMetas.forEach(({ m, content, media }) => {
      if (mode === "auto") { m.content = content; if (media) m.media = media; }
      else { m.content = effective === "dark" ? "#22262F" : "#E0E5EC"; m.removeAttribute("media"); }
    });
  }
  if (themeBtn) {
    themeBtn.addEventListener("click", () => {
      const mode = THEME_ORDER.includes(root.dataset.theme) ? root.dataset.theme : "auto";
      const next = THEME_ORDER[(THEME_ORDER.indexOf(mode) + 1) % THEME_ORDER.length];
      root.dataset.theme = next;
      try { localStorage.setItem("cm-theme", next); } catch { /* not persisted, still applied */ }
      syncTheme();
    });
  }
  darkMq.addEventListener("change", syncTheme);

  /* ---------- reveal ---------- */

  const pending = document.querySelectorAll(".reveal:not(.in)");
  if (!("IntersectionObserver" in window)) pending.forEach((e) => e.classList.add("in"));
  else {
    const revealObs = new IntersectionObserver((entries) => {
      entries.forEach((e) => {
        if (!e.isIntersecting) return;
        e.target.classList.add("in");
        revealObs.unobserve(e.target);
      });
    }, { threshold: 0.08, rootMargin: "0px 0px -4% 0px" });
    pending.forEach((e) => revealObs.observe(e));
  }

  /* ---------- sidebar drawer (narrow screens) ---------- */

  const sidebar = document.getElementById("sidebar");
  const scrim = document.getElementById("scrim");
  const menuBtn = document.getElementById("menu-btn");
  const closeBtn = document.getElementById("sidebar-close");
  function setDrawer(open) {
    if (!sidebar) return;
    sidebar.classList.toggle("open", open);
    if (scrim) scrim.classList.toggle("open", open);
    if (menuBtn) menuBtn.setAttribute("aria-expanded", String(open));
    document.body.style.overflow = open ? "hidden" : "";
    if (open) {
      const active = sidebar.querySelector(".side-link.active") || sidebar.querySelector("a");
      if (active) active.focus({ preventScroll: true });
    } else if (menuBtn && sidebar.contains(document.activeElement)) menuBtn.focus();
  }
  if (menuBtn) menuBtn.addEventListener("click", () => setDrawer(!sidebar.classList.contains("open")));
  if (closeBtn) closeBtn.addEventListener("click", () => setDrawer(false));
  if (scrim) scrim.addEventListener("click", () => setDrawer(false));
  matchMedia("(min-width: 961px)").addEventListener("change", (e) => { if (e.matches) setDrawer(false); });

  // bring the current page's link into view inside a long sidebar
  const activeLink = sidebar && sidebar.querySelector(".side-link.active");
  if (activeLink) {
    const box = activeLink.getBoundingClientRect(), frame = sidebar.getBoundingClientRect();
    const top = sidebar.scrollTop + box.top - frame.top - sidebar.clientHeight / 2 + box.height / 2;
    if (top > 0) sidebar.scrollTop = top;
  }

  /* ---------- headings: anchors ---------- */

  document.querySelectorAll(".prose h2[id], .prose h3[id], .prose h4[id]").forEach((h) => {
    const a = document.createElement("a");
    a.className = "heading-anchor";
    a.href = "#" + h.id;
    a.setAttribute("aria-label", "Link to " + h.textContent.trim());
    a.textContent = "#";
    h.prepend(a);
  });

  /* ---------- code: copy buttons ---------- */

  const COPY = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect width="14" height="14" x="8" y="8" rx="2" ry="2"/><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"/></svg>';
  const DONE = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 6 9 17l-5-5"/></svg>';
  document.querySelectorAll(".hl").forEach((block) => {
    const code = block.querySelector("pre");
    if (!code || !navigator.clipboard) return;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "copy-btn";
    btn.setAttribute("aria-label", "Copy code");
    btn.innerHTML = COPY;
    btn.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code.innerText.replace(/\n$/, ""));
        btn.classList.add("done");
        btn.innerHTML = DONE;
        btn.setAttribute("aria-label", "Copied");
        setTimeout(() => { btn.classList.remove("done"); btn.innerHTML = COPY; btn.setAttribute("aria-label", "Copy code"); }, 1600);
      } catch { /* clipboard refused; nothing to undo */ }
    });
    block.appendChild(btn);
  });

  /* ---------- "On this page" scroll spy + progress bar ---------- */

  const tocLinks = [...document.querySelectorAll(".toc-list a")];
  const targets = tocLinks.map((a) => document.getElementById(decodeURIComponent(a.hash.slice(1)))).filter(Boolean);
  function spy() {
    if (!targets.length) return;
    const line = innerHeight * 0.3;
    let current = targets[0];
    for (const t of targets) if (t.getBoundingClientRect().top <= line) current = t;
    if (innerHeight + scrollY >= document.documentElement.scrollHeight - 4) current = targets[targets.length - 1];
    tocLinks.forEach((a) => a.classList.toggle("active", a.hash === "#" + current.id));
  }
  const progress = document.getElementById("progress");
  let ticking = false;
  function onScroll() {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(() => {
      ticking = false;
      spy();
      if (progress && !calmMotion) {
        const max = document.documentElement.scrollHeight - innerHeight;
        progress.style.transform = "scaleX(" + (max > 0 ? Math.min(scrollY / max, 1).toFixed(4) : 0) + ")";
      }
    });
  }
  addEventListener("scroll", onScroll, { passive: true });
  addEventListener("resize", onScroll, { passive: true });

  /* ---------- search ---------- */

  const dialog = document.getElementById("search-dialog");
  const input = document.getElementById("search-input");
  const list = document.getElementById("search-results");
  const openers = document.querySelectorAll("[data-search-open]");
  let index = null, loading = null, hits = [], selected = 0;

  function load() {
    if (index) return Promise.resolve(index);
    if (!loading) {
      loading = fetch(root.dataset.search)
        .then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); })
        .then((data) => {
          index = data.map((p) => ({
            ...p,
            tl: p.t.toLowerCase(),
            hl: p.h.map(([text, id]) => [text, id, text.toLowerCase()]),
            xl: p.x.toLowerCase(),
          }));
          return index;
        })
        .catch(() => { loading = null; return null; });
    }
    return loading;
  }

  const escapeHtml = (s) => s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  function mark(text, terms) {
    let out = escapeHtml(text);
    for (const t of terms) {
      if (t.length < 2) continue;
      out = out.replace(new RegExp("(" + t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")", "ig"), "<mark>$1</mark>");
    }
    return out;
  }
  function snippet(page, terms) {
    const at = terms.reduce((best, t) => { const i = page.xl.indexOf(t); return i >= 0 && (best < 0 || i < best) ? i : best; }, -1);
    if (at < 0) return page.x.slice(0, 150);
    const start = Math.max(0, page.x.lastIndexOf(" ", Math.max(0, at - 50)) + 1);
    return (start > 0 ? "…" : "") + page.x.slice(start, start + 170) + "…";
  }

  function search(query) {
    const q = query.trim().toLowerCase();
    if (!q || !index) return [];
    const terms = q.split(/\s+/).filter(Boolean);
    const results = [];
    for (const page of index) {
      let score = 0, heading = null, all = true;
      for (const t of terms) {
        let s = 0;
        if (page.tl.includes(t)) s += page.tl.startsWith(t) ? 60 : 40;
        for (const h of page.hl) {
          if (h[2].includes(t)) { s += 18; if (!heading) heading = h; }
        }
        const count = page.xl.split(t).length - 1;
        if (count) s += Math.min(count, 12) * 2;
        if (!s) { all = false; break; }
        score += s;
      }
      if (!all) continue;
      if (page.tl === q) score += 100;
      results.push({ page, score, heading, terms });
    }
    return results.sort((a, b) => b.score - a.score).slice(0, 12);
  }

  function render(query) {
    if (!list) return;
    hits = search(query);
    selected = 0;
    if (!query.trim()) { list.innerHTML = '<li class="search-empty">Type to search the documentation.</li>'; return; }
    if (!index) { list.innerHTML = '<li class="search-empty">The search index could not be loaded.</li>'; return; }
    if (!hits.length) { list.innerHTML = '<li class="search-empty">Nothing matches <strong>' + escapeHtml(query) + "</strong>.</li>"; return; }
    list.innerHTML = hits.map((h, i) => {
      const href = siteRoot + h.page.u + (h.heading ? "#" + h.heading[1] : "");
      const title = h.heading && !h.page.tl.includes(h.terms[0]) ? h.page.t + " › " + h.heading[0] : h.page.t;
      return '<li><a class="search-hit' + (i === 0 ? " selected" : "") + '" href="' + href + '" role="option" aria-selected="' + (i === 0) + '">' +
        '<span class="hit-top"><span class="hit-title">' + mark(title, h.terms) + '</span><span class="hit-cat">' + escapeHtml(h.page.c) + "</span></span>" +
        '<span class="hit-text">' + mark(snippet(h.page, h.terms), h.terms) + "</span></a></li>";
    }).join("");
  }
  function select(i) {
    const items = list.querySelectorAll(".search-hit");
    if (!items.length) return;
    selected = (i + items.length) % items.length;
    items.forEach((el, n) => { el.classList.toggle("selected", n === selected); el.setAttribute("aria-selected", String(n === selected)); });
    items[selected].scrollIntoView({ block: "nearest" });
  }

  function openSearch() {
    if (!dialog) return;
    if (!dialog.open) dialog.showModal();
    input.select();
    load().then(() => render(input.value));
  }
  openers.forEach((b) => b.addEventListener("click", openSearch));
  if (dialog) {
    input.addEventListener("input", () => load().then(() => render(input.value)));
    input.addEventListener("keydown", (e) => {
      if (e.key === "ArrowDown") { e.preventDefault(); select(selected + 1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); select(selected - 1); }
      else if (e.key === "Enter") {
        const item = list.querySelectorAll(".search-hit")[selected];
        if (item) { e.preventDefault(); location.href = item.href; dialog.close(); }
      }
    });
    dialog.addEventListener("click", (e) => { if (e.target === dialog) dialog.close(); });
    list.addEventListener("click", (e) => { if (e.target.closest(".search-hit")) dialog.close(); });
  }
  addEventListener("keydown", (e) => {
    const typing = e.target.closest && e.target.closest("input, textarea, select, [contenteditable]");
    if ((e.key === "k" || e.key === "K") && (e.ctrlKey || e.metaKey)) { e.preventDefault(); openSearch(); }
    else if (e.key === "/" && !typing && !e.ctrlKey && !e.metaKey && !e.altKey) { e.preventDefault(); openSearch(); }
    else if (e.key === "Escape" && sidebar && sidebar.classList.contains("open")) setDrawer(false);
  });
  const isMac = /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
  document.querySelectorAll("[data-mod-key]").forEach((k) => { k.textContent = isMac ? "⌘K" : "Ctrl K"; });

  /* ---------- boot ---------- */

  const year = document.getElementById("year");
  if (year) year.textContent = new Date().getFullYear();
  syncTheme();
  spy();
  onScroll();
})();
