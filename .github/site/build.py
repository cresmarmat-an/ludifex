"""Builds the documentation site from the Markdown in docs/.

    python .github/site/build.py [--out _site] [--strict]

Each folder in docs/ is a category and each Markdown file in it is a page,
written to <category>/<page>/index.html. docs/nav.json sets the order of the
categories and pages and can give them titles; files it does not list are
added after the listed ones, sorted by name, with the title taken from their
first "# " heading. So a new page only needs a new .md file.

The changelog and third-party notices come from the repository's root. The
home page, a search index, a sitemap, and a 404 page are written beside the
pages.

Problems such as a link to a missing file are reported as warnings, so one
bad link does not stop the site from updating. --strict turns them into
errors.

Needs Python 3.9 or later with the `markdown` and `pygments` packages
(pip install -r .github/site/requirements.txt).
"""

import argparse
import hashlib
import html
import json
import os
import posixpath
import re
import shutil
import sys
from datetime import datetime, timezone

import markdown
from markdown.extensions.codehilite import CodeHiliteExtension
from markdown.extensions.toc import TocExtension

SITE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(SITE))
DOCS = os.path.join(REPO, "docs")
TEMPLATES = os.path.join(SITE, "templates")

# Files in docs/ that are not pages.
NOT_PAGES = {"readme.md"}


def slugify(value, separator="-"):
    """GitHub's heading anchors, so a link that works on GitHub works here."""
    value = re.sub(r"<[^>]+>", "", value)
    value = html.unescape(value).strip().lower()
    value = re.sub(r"[^\w\- ]", "", value, flags=re.UNICODE)
    return value.replace(" ", separator)


def read(path):
    with open(path, encoding="utf-8") as file:
        return file.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)


def fail(message):
    sys.exit(f"build.py: {message}")


def titleize(name):
    """A title from a file or folder name: "getting-started" is "Getting started"."""
    words = re.sub(r"[-_]+", " ", name).strip()
    return words[:1].upper() + words[1:]


def heading_title(path):
    """The text of a Markdown file's first "# " heading, outside code blocks."""
    fence = False
    for line in read(path).split("\n"):
        if re.match(r"^\s*```", line):
            fence = not fence
        elif not fence:
            match = re.match(r"^#\s+(.+?)\s*#*\s*$", line)
            if match:
                return re.sub(r"[`*_]", "", match.group(1)).strip()
    return None


def convert(source):
    """Markdown to HTML with GitHub-style anchors, highlighted code, and
    wrappers the stylesheet expects (tables scroll, code blocks name their
    language)."""
    converter = markdown.Markdown(extensions=[
        "fenced_code",
        "tables",
        "sane_lists",
        TocExtension(slugify=slugify, toc_depth="2-3"),
        CodeHiliteExtension(css_class="hl", guess_lang=False, use_pygments=True),
    ])
    body = converter.convert(source)
    # Repeated headings: GitHub numbers them -1, -2; Python-Markdown writes _1, _2.
    body = re.sub(r'id="([^"]+?)_(\d+)"', r'id="\1-\2"', body)
    body = re.sub(r"<table>", '<div class="table-wrap"><table>', body)
    body = re.sub(r"</table>", "</table></div>", body)

    languages = []
    fence = False
    for line in source.split("\n"):
        match = re.match(r"^\s*```\s*([\w+#-]*)", line)
        if match:
            if not fence:
                languages.append(match.group(1).lower())
            fence = not fence
    blocks = body.count('<div class="hl">')
    if blocks == len(languages):
        names = {"cpp": "C++", "c++": "C++", "cmake": "CMake", "bash": "Shell", "sh": "Shell", "hlsl": "HLSL",
                 "json": "JSON", "powershell": "PowerShell", "ps1": "PowerShell", "c": "C", "text": ""}
        iterator = iter(languages)

        def label(_):
            lang = next(iterator)
            name = names.get(lang, lang.upper() if lang else "")
            return f'<div class="hl" data-lang="{html.escape(name)}">' if name else '<div class="hl">'
        body = re.sub(r'<div class="hl">', label, body)
    return body, converter.toc_tokens


def flatten_toc(tokens):
    out = []
    for token in tokens:
        if token["level"] >= 2:
            out.append({"level": token["level"], "id": re.sub(r"_(\d+)$", r"-\1", token["id"]),
                        "text": html.unescape(re.sub(r"<[^>]+>", "", token["name"]))})
        out += flatten_toc(token.get("children", []))
    return out


def plain_text(body):
    text = re.sub(r"<pre.*?</pre>", " ", body, flags=re.S)
    text = re.sub(r"<[^>]+>", " ", text)
    return re.sub(r"\s+", " ", html.unescape(text)).strip()


def first_paragraph(body):
    match = re.search(r"<p>(.*?)</p>", body, flags=re.S)
    text = html.unescape(re.sub(r"<[^>]+>", "", match.group(1))) if match else ""
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= 158 else text[:155].rsplit(" ", 1)[0] + "…"


class Site:
    def __init__(self, out, strict):
        self.out = out
        self.strict = strict
        self.warnings = []
        self.files = {}  # repository files the pages use, by their path in the site
        self.config = json.loads(read(os.path.join(SITE, "config.json")))
        self.partials = {}
        for name in os.listdir(os.path.join(TEMPLATES, "partials")):
            self.partials[os.path.splitext(name)[0]] = read(os.path.join(TEMPLATES, "partials", name)).rstrip("\n")
        self.asset_version = self.hash_assets()

        self.nav = self.discover()
        self.pages = [page for category in self.nav for page in category["pages"]]
        if not self.pages:
            fail("docs/ has no pages")
        self.by_source = {os.path.normcase(os.path.abspath(p["source"])): p for p in self.pages}
        # The docs link to the repository's own files by their GitHub address,
        # so the Markdown reads correctly on GitHub too; those that are pages
        # here are sent to the page instead.
        self.by_repository_url = {}
        for p in self.pages:
            if p["origin"] == "repository":
                for kind in ("blob", "tree"):
                    self.by_repository_url[f"{self.config['repository']}/{kind}/main/{p['path']}"] = p

    def warn(self, message, path=None):
        self.warnings.append(message)
        if os.environ.get("GITHUB_ACTIONS") == "true":
            where = f" file={os.path.relpath(path, REPO).replace(os.sep, '/')}" if path else ""
            print(f"::warning{where}::{message}")
        else:
            print(f"warning: {message}")

    def hash_assets(self):
        digest = hashlib.sha256()
        for folder, _, files in sorted(os.walk(os.path.join(SITE, "assets"))):
            for name in sorted(files):
                with open(os.path.join(folder, name), "rb") as file:
                    digest.update(file.read())
        digest.update(json.dumps(self.config, sort_keys=True).encode())
        return digest.hexdigest()[:10]

    # --- finding the pages ---------------------------------------------------

    def page(self, folder, name, title, source, origin, path=None):
        slug = f"{folder}/{name}" if folder else name
        return {"slug": slug, "file": name, "title": title, "folder": folder, "source": source,
                "origin": origin, "path": path}

    def markdown_files(self, folder):
        directory = os.path.join(DOCS, folder)
        names = []
        for name in sorted(os.listdir(directory)):
            full = os.path.join(directory, name)
            if os.path.isfile(full) and name.lower().endswith(".md") and name.lower() not in NOT_PAGES:
                names.append(name[:-3])
        return names

    def discover(self):
        """The categories and their pages: those docs/nav.json lists, in its
        order, then any other Markdown files and folders, sorted by name."""
        listed = []
        nav_path = os.path.join(DOCS, "nav.json")
        if os.path.isfile(nav_path):
            try:
                listed = json.loads(read(nav_path))
            except json.JSONDecodeError as error:
                self.warn(f"nav.json is not valid JSON ({error}); pages are in alphabetical order", nav_path)

        folders = sorted(name for name in os.listdir(DOCS)
                         if os.path.isdir(os.path.join(DOCS, name)) and not name.startswith((".", "_")))
        nav = []
        seen = set()
        for entry in listed:
            folder = entry.get("folder", "")
            if folder not in folders:
                self.warn(f"nav.json lists the folder {folder!r}, which docs/ does not have", nav_path)
                continue
            seen.add(folder)
            pages, names = [], set()
            for item in entry.get("pages", []):
                name = item.get("file", "")
                names.add(name)
                if "library" in item:
                    source = os.path.join(REPO, item["library"])
                    if os.path.isfile(source):
                        pages.append(self.page(folder, name, item.get("title") or titleize(name), source,
                                               "repository", item["library"]))
                    else:
                        self.warn(f"nav.json lists {item['library']}, which the repository does not have", nav_path)
                    continue
                source = os.path.join(DOCS, folder, name + ".md")
                if not os.path.isfile(source):
                    self.warn(f"nav.json lists {folder}/{name}.md, which does not exist", nav_path)
                    continue
                pages.append(self.page(folder, name, item.get("title") or heading_title(source) or titleize(name),
                                       source, "docs"))
            for name in self.markdown_files(folder):
                if name not in names:
                    source = os.path.join(DOCS, folder, name + ".md")
                    pages.append(self.page(folder, name, heading_title(source) or titleize(name), source, "docs"))
            if pages:
                nav.append({"folder": folder, "title": entry.get("title") or titleize(folder), "pages": pages})

        for folder in folders:
            if folder in seen:
                continue
            pages = []
            for name in self.markdown_files(folder):
                source = os.path.join(DOCS, folder, name + ".md")
                pages.append(self.page(folder, name, heading_title(source) or titleize(name), source, "docs"))
            if pages:
                nav.append({"folder": folder, "title": titleize(folder), "pages": pages})

        # Pages directly in docs/, outside any folder.
        loose = [self.page("", name, heading_title(os.path.join(DOCS, name + ".md")) or titleize(name),
                           os.path.join(DOCS, name + ".md"), "docs")
                 for name in self.markdown_files("")]
        if loose:
            nav.append({"folder": "", "title": "More", "pages": loose})
        return nav

    # --- links ---------------------------------------------------------------

    @staticmethod
    def url_between(from_slug, to_slug, fragment=""):
        """A relative URL from one page's directory to another's ("" is the home page)."""
        rel = posixpath.relpath(to_slug or ".", from_slug or ".")
        rel = "./" if rel == "." else rel + "/"
        return rel + (f"#{fragment}" if fragment else "")

    def github(self, path, is_dir=False):
        return f"{self.config['repository']}/{'tree' if is_dir else 'blob'}/main/{path}"

    def resolve(self, page, href):
        """A relative link's target in the repository, or None if it leaves it."""
        target = os.path.normpath(os.path.join(os.path.dirname(page["source"]), href))
        inside = os.path.relpath(target, REPO).replace("\\", "/")
        return None if inside.startswith("..") else (target, inside)

    def rewrite_links(self, body, page):
        def swap(match):
            quote, href = match.group(1), html.unescape(match.group(2))
            if href.startswith("#"):
                return match.group(0)
            if re.match(r"^[a-z][a-z0-9+.-]*:", href):
                base, _, fragment = href.partition("#")
                target = self.by_repository_url.get(base)
                if not target:
                    return match.group(0)
                new = self.url_between(page["slug"], target["slug"], fragment)
                return f"href={quote}{html.escape(new, quote=True)}{quote}"
            path, _, fragment = href.partition("#")
            resolved = self.resolve(page, path)
            if resolved is None:
                self.warn(f"{page['slug']}: the link {href!r} leaves the repository", page["source"])
                return match.group(0)
            target, inside = resolved
            key = os.path.normcase(os.path.abspath(target))
            if key in self.by_source:
                new = self.url_between(page["slug"], self.by_source[key]["slug"], fragment)
            elif os.path.exists(target):
                new = self.github(inside, os.path.isdir(target)) + (f"#{fragment}" if fragment else "")
            else:
                self.warn(f"{page['slug']}: the link {href!r} points at nothing", page["source"])
                return match.group(0)
            return f"href={quote}{html.escape(new, quote=True)}{quote}"

        return re.sub(r'href=(["\'])(.*?)\1', swap, body)

    def rewrite_images(self, body, page):
        """Images next to the pages are copied into the site under files/."""
        def swap(match):
            quote, src = match.group(1), html.unescape(match.group(2))
            if re.match(r"^[a-z][a-z0-9+.-]*:", src) or src.startswith("/"):
                return match.group(0)
            resolved = self.resolve(page, src)
            if resolved is None or not os.path.isfile(resolved[0]):
                self.warn(f"{page['slug']}: the image {src!r} does not exist", page["source"])
                return match.group(0)
            target, inside = resolved
            site_path = "files/" + inside
            self.files[site_path] = target
            new = posixpath.relpath(site_path, page["slug"])
            return f"src={quote}{html.escape(new, quote=True)}{quote}"

        return re.sub(r'src=(["\'])(.*?)\1', swap, body)

    def check_fragments(self, rendered):
        """Every #fragment that points into this site names a heading that exists."""
        ids = {slug: set(re.findall(r'id="([^"]+)"', body)) for slug, body in rendered.items()}
        sources = {p["slug"]: p["source"] for p in self.pages}
        for slug, body in rendered.items():
            for href in re.findall(r'href="([^"]+)"', body):
                if re.match(r"^[a-z][a-z0-9+.-]*:", href):
                    continue
                path, _, fragment = html.unescape(href).partition("#")
                if not fragment:
                    continue
                target = posixpath.normpath(posixpath.join(slug, path)) if path else slug
                if target in ids and fragment not in ids[target]:
                    self.warn(f"{slug}: #{fragment} is not a heading of {target}", sources[slug])

    # --- pieces of a page ----------------------------------------------------

    def sidebar(self, current):
        chevron = ('<svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
                   'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
                   '<path d="m9 18 6-6-6-6"/></svg>')
        groups = []
        for category in self.nav:
            items = []
            is_open = False
            for page in category["pages"]:
                active = page["slug"] == current
                is_open = is_open or active
                attrs = ' class="side-link active" aria-current="page"' if active else ' class="side-link"'
                items.append(f'          <li><a{attrs} href="{self.url_between(current, page["slug"])}">'
                             f'{html.escape(page["title"])}</a></li>')
            groups.append(f'        <details class="side-group"{" open" if is_open else ""}>\n'
                          f'          <summary>{html.escape(category["title"])}{chevron}</summary>\n'
                          f'          <ul>\n' + "\n".join(items) + '\n          </ul>\n        </details>')
        return "\n".join(groups)

    @staticmethod
    def toc(entries):
        if len(entries) < 2:
            return ""
        items = "".join(f'<li class="toc-l{e["level"]}"><a href="#{e["id"]}">{html.escape(e["text"])}</a></li>'
                        for e in entries)
        return f'      <p class="toc-title">On this page</p>\n      <ul class="toc-list">{items}</ul>'

    def pager(self, index):
        arrows = {"prev": '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m12 19-7-7 7-7"/><path d="M19 12H5"/></svg>',
                  "next": '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 12h14"/><path d="m12 5 7 7-7 7"/></svg>'}
        cells = []
        for offset, cls in ((-1, "prev"), (1, "next")):
            position = index + offset
            if 0 <= position < len(self.pages):
                other = self.pages[position]
                label = f"{arrows['prev']} Previous" if cls == "prev" else f"Next {arrows['next']}"
                cells.append(f'<a class="pager-card {cls}" href="{self.url_between(self.pages[index]["slug"], other["slug"])}" '
                             f'rel="{cls}"><span class="pager-label">{label}</span>'
                             f'<span class="pager-title">{html.escape(other["title"])}</span></a>')
            else:
                cells.append('<span aria-hidden="true"></span>')
        return "".join(cells)

    def render(self, template_name, values):
        template = read(os.path.join(TEMPLATES, template_name))
        template = re.sub(r"\{\{> (\w+)\}\}", lambda m: self.partials[m.group(1)], template)

        def swap(match):
            key = match.group(1)
            if key not in values:
                fail(f"{template_name}: nothing fills {{{{{key}}}}}")
            return values[key]
        return re.sub(r"\{\{(\w+)\}\}", swap, template)

    def common(self, slug, depth=None):
        c = self.config
        if depth is None:
            depth = slug.count("/") + 1 if slug else 0
        root = "../" * depth or "./"
        return {
            "root": root,
            "assets": root + "assets/",
            "search_url": root + "assets/search.json",
            "docs_home": self.url_between(slug, self.pages[0]["slug"]),
            "name": html.escape(c["name"]),
            "version": html.escape(c["version"]),
            "repository": html.escape(c["repository"]),
            "examples": html.escape(c["examples"]),
            "author": html.escape(c["author"]),
            "author_url": html.escape(c["author_url"]),
            "license_url": html.escape(c["repository"] + "/blob/main/LICENSE"),
            "site_url": html.escape(c["site_url"]),
            "year": str(datetime.now(timezone.utc).year),
            "asset_version": self.asset_version,
            "container": "container-wide" if slug else "container",
            "menu_button": "",
            "docs_active": "",
            "docs_current": "",
        }

    # --- building --------------------------------------------------------------

    def build(self):
        if os.path.isdir(self.out):
            shutil.rmtree(self.out)
        shutil.copytree(os.path.join(SITE, "assets"), os.path.join(self.out, "assets"))
        self.write_accent()

        menu = ('<button class="icon-btn menu-btn" id="menu-btn" type="button" aria-label="Open the menu" '
                'aria-controls="sidebar" aria-expanded="false"><svg width="18" height="18" viewBox="0 0 24 24" '
                'fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" '
                'aria-hidden="true"><path d="M4 6h16"/><path d="M4 12h16"/><path d="M4 18h16"/></svg></button>')
        search, rendered, outputs = [], {}, {}
        category_of = {p["slug"]: c["title"] for c in self.nav for p in c["pages"]}
        for index, page in enumerate(self.pages):
            source = read(page["source"])
            body, tokens = convert(source)
            body = self.rewrite_links(body, page)
            body = self.rewrite_images(body, page)
            # The page's title sits in the template's header instead.
            body = re.sub(r"^\s*<h1[^>]*>.*?</h1>\s*", "", body, count=1, flags=re.S)
            rendered[page["slug"]] = body
            toc = flatten_toc(tokens)
            values = self.common(page["slug"])
            path = os.path.relpath(page["source"], REPO).replace("\\", "/")
            if page["origin"] == "docs":
                edit_url = self.github(path).replace("/blob/", "/edit/")
                edit_label = "Edit this page on GitHub"
            else:
                edit_url = self.github(path)
                edit_label = f"{path} on GitHub"
            values.update({
                "page_title": html.escape(f"{page['title']} · {self.config['name']} documentation"),
                "title": html.escape(page["title"]),
                "category": html.escape(category_of[page["slug"]]),
                "description": html.escape(first_paragraph(body) or self.config["description"]),
                "canonical": html.escape(self.config["site_url"] + page["slug"] + "/"),
                "content": body,
                "sidebar": self.sidebar(page["slug"]),
                "toc": self.toc(toc),
                "pager": self.pager(index),
                "edit_url": html.escape(edit_url),
                "edit_label": html.escape(edit_label),
                "menu_button": menu,
                "docs_active": " active",
                "docs_current": ' aria-current="true"',
            })
            outputs[page["slug"]] = self.render("page.html", values)
            search.append({"t": page["title"], "c": category_of[page["slug"]], "u": page["slug"] + "/",
                           "h": [[e["text"], e["id"]] for e in toc], "x": plain_text(body)[:8000]})

        self.check_fragments(rendered)
        for slug, text in outputs.items():
            write(os.path.join(self.out, slug, "index.html"), text)
        for site_path, source in self.files.items():
            destination = os.path.join(self.out, *site_path.split("/"))
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copyfile(source, destination)
        write(os.path.join(self.out, "assets", "search.json"),
              json.dumps(search, ensure_ascii=False, separators=(",", ":")))
        self.build_home()
        self.build_404()
        self.build_sitemap()
        write(os.path.join(self.out, ".nojekyll"), "")
        print(f"{self.config['name']}: {len(self.pages)} pages and the home page written to {self.out}")
        if self.warnings:
            print(f"{len(self.warnings)} warning(s)")
            if self.strict:
                sys.exit(1)

    def write_accent(self):
        def block(values):
            return ";".join(f"--{key}:{value}" for key, value in values.items())
        light, dark = block(self.config["accent"]["light"]), block(self.config["accent"]["dark"])
        write(os.path.join(self.out, "assets", "css", "accent.css"),
              f"/* {self.config['name']}'s accent, written by build.py from config.json */\n"
              f":root{{{light}}}\n"
              f":root[data-theme=\"dark\"]{{{dark}}}\n"
              f"@media(prefers-color-scheme:dark){{:root[data-theme=\"auto\"]{{{dark}}}}}\n")

    def build_home(self):
        c = self.config
        values = self.common("")
        page_url = {p["slug"]: p for p in self.pages}
        config_path = os.path.join(SITE, "config.json")

        def link(slug):
            if slug not in page_url:
                self.warn(f"config.json names the page {slug!r}, which docs/ does not have", config_path)
                slug = self.pages[0]["slug"]
            return slug + "/"

        cards = []
        for category in self.nav:
            links = "".join(f'<li><a href="{p["slug"]}/">{html.escape(p["title"])}</a></li>'
                            for p in category["pages"])
            first = category["pages"][0]
            cards.append(f'        <article class="cat-card reveal"><h3><a href="{first["slug"]}/">'
                         f'{html.escape(category["title"])}</a><span class="cat-count">{len(category["pages"])}</span>'
                         f'</h3><ul>{links}</ul></article>')
        features = "\n".join(
            f'        <article class="feature reveal"><span class="feature-icon" aria-hidden="true">'
            f'<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
            f'stroke-linecap="round" stroke-linejoin="round">{f["icon"]}</svg></span>'
            f'<h3><a href="{link(f["page"])}">{html.escape(f["title"])}</a></h3><p>{html.escape(f["text"])}</p></article>'
            for f in c["features"])

        # The home page's example is the introduction's, so the two never disagree.
        example = ""
        example_path = os.path.join(DOCS, c["example_page"] + ".md")
        match = re.search(r"^```cpp\n.*?^```$", read(example_path), flags=re.S | re.M) \
            if os.path.isfile(example_path) else None
        if match:
            example, _ = convert(match.group(0) + "\n")
        else:
            self.warn(f"{c['example_page']}.md has no C++ example for the home page", config_path)
        repos = "\n".join(
            f'            <li><a href="{html.escape(r["url"])}">{html.escape(r["name"])}'
            f'<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" '
            f'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 7h10v10"/><path d="M7 17 17 7"/></svg></a></li>'
            for r in c["example_repos"])
        jsonld = json.dumps({
            "@context": "https://schema.org", "@type": "SoftwareSourceCode", "name": c["name"],
            "description": c["description"], "url": c["site_url"], "codeRepository": c["repository"],
            "programmingLanguage": "C++", "license": "https://opensource.org/licenses/MIT", "version": c["version"],
            "author": {"@type": "Person", "name": c["author"], "url": c["author_url"]},
        }, ensure_ascii=False).replace("</", "<\\/")
        values.update({
            "page_title": html.escape(f"{c['name']} · {c['tagline'].rstrip('.')}"),
            "description": html.escape(c["description"]),
            "canonical": html.escape(c["site_url"]),
            "tagline": html.escape(c["tagline"]),
            "lead": html.escape(c["lead"]),
            "example": example,
            "example_title": html.escape(c["example_title"]),
            "example_text": html.escape(c["example_text"]),
            "install_page": link(c["install_page"]),
            "features_title": html.escape(c["features_title"]),
            "features": features,
            "categories": "\n".join(cards),
            "sibling_name": html.escape(c["sibling"]["name"]),
            "sibling_url": html.escape(c["sibling"]["url"]),
            "sibling_text": html.escape(c["sibling"]["text"]),
            "example_repos": repos,
            "jsonld": jsonld,
        })
        write(os.path.join(self.out, "index.html"), self.render("home.html", values))

    def build_404(self):
        # GitHub Pages serves this for a missing path at any depth, so its
        # links are absolute from the site's own address.
        c = self.config
        values = self.common("", depth=0)
        root = c["site_url"]
        values.update({
            "root": html.escape(root), "assets": html.escape(root + "assets/"),
            "search_url": html.escape(root + "assets/search.json"),
            "docs_home": html.escape(root + self.pages[0]["slug"] + "/"),
            "page_title": html.escape(f"Page not found · {c['name']} documentation"),
            "description": html.escape(c["description"]), "canonical": html.escape(root),
        })
        write(os.path.join(self.out, "404.html"), self.render("404.html", values))

    def build_sitemap(self):
        base = self.config["site_url"]
        urls = [base] + [base + p["slug"] + "/" for p in self.pages]
        entries = "".join(f"<url><loc>{html.escape(u)}</loc></url>" for u in urls)
        write(os.path.join(self.out, "sitemap.xml"),
              f'<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">{entries}</urlset>\n')
        write(os.path.join(self.out, "robots.txt"), f"User-agent: *\nAllow: /\nSitemap: {base}sitemap.xml\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", default=os.path.join(REPO, "_site"))
    parser.add_argument("--strict", action="store_true", help="fail on warnings, such as broken links")
    args = parser.parse_args()
    if not os.path.isdir(DOCS):
        fail(f"no docs folder at {DOCS}")
    Site(os.path.abspath(args.out), args.strict).build()


if __name__ == "__main__":
    main()
