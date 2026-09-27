# ludifex documentation

These pages are published at
**[cresmarmat-an.github.io/ludifex](https://cresmarmat-an.github.io/ludifex/)**.

Each folder is a category and each Markdown file in it is a page. Every push
to the `main` branch runs a GitHub Actions workflow
(`.github/workflows/docs.yml`) that rebuilds the site and publishes it,
usually within a couple of minutes. It can also be run by hand from the
repository's Actions tab.

## Adding a page

1. Add a Markdown file to one of the folders, or create a new folder for a new
   category.
2. Start the file with a `# Title` line. The title is used in the navigation.
3. Push it to `main`. The page is added at the end of its category.

`nav.json` sets the order of the categories and pages and can set their
titles. Anything it does not list is added after the listed pages, sorted by
file name, so editing it is optional.

Link to other pages by their relative `.md` path, for example
`../physics/joints.md#ragdolls`, so the links work both on GitHub and
on the site. Images can be kept next to the pages that use them.

## Building the site locally

```bash
pip install -r .github/site/requirements.txt
```

```bash
python .github/site/build.py --strict
```

The site is written to `_site/`. Broken links are reported as warnings, and
`--strict` turns them into errors. The site's templates, styles, and images
are in `.github/site/`.
