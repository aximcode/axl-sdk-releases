#!/usr/bin/env python3
"""Download UEFI-related specifications from uefi.org.

HTML specs (UEFI, PI) open Chrome for Cloudflare bypass — solve the
captcha once, then the script downloads all chapters automatically.
PDF specs (Shell) download directly with no browser needed.

Supported specifications:
    uefi   — UEFI Specification 2.11         → deps/uefi-spec/  (HTML)
    pi     — PI Specification 1.8            → deps/pi-spec/    (HTML)
    acpi   — ACPI Specification 6.5          → deps/acpi-spec/  (HTML)
    shell  — UEFI Shell Specification 2.2    → deps/shell-spec/ (PDF)
    all    — All of the above (default)

Requirements:
    pip install playwright    (for HTML specs only)

Usage:
    python3 scripts/download-uefi-specs.py              # download all
    python3 scripts/download-uefi-specs.py uefi          # UEFI spec only
    python3 scripts/download-uefi-specs.py pi shell      # PI + Shell
    python3 scripts/download-uefi-specs.py --redownload  # force re-download
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

SPECS: dict[str, dict[str, str]] = {
    "uefi": {
        "name": "UEFI Specification 2.11",
        "base_url": "https://uefi.org/specs/UEFI/2.11/",
        "output_dir": "deps/uefi-spec",
        "format": "html",
    },
    "pi": {
        "name": "PI Specification 1.8",
        "base_url": "https://uefi.org/specs/PI/1.8/",
        "output_dir": "deps/pi-spec",
        "format": "html",
    },
    "acpi": {
        "name": "ACPI Specification 6.5",
        "base_url": "https://uefi.org/specs/ACPI/6.5/",
        "output_dir": "deps/acpi-spec",
        "format": "html",
    },
    "shell": {
        "name": "UEFI Shell Specification 2.2",
        "base_url": "https://uefi.org/sites/default/files/resources/UEFI_Shell_2_2.pdf",
        "output_dir": "deps/shell-spec",
        "format": "pdf",
    },
}


def download_pdf(spec_key: str, project_root: Path,
                 redownload: bool) -> bool:
    """Download a PDF spec.

    Tries urllib with a browser user-agent first, falls back to
    Playwright if Cloudflare blocks the request.
    """
    import urllib.request

    spec = SPECS[spec_key]
    url = spec["base_url"]
    output_dir = project_root / spec["output_dir"]
    output_dir.mkdir(parents=True, exist_ok=True)
    filename = url.rsplit("/", 1)[-1]
    out_file = output_dir / filename

    print(f"\n{'='*60}")
    print(f"Downloading: {spec['name']}")
    print(f"From:        {url}")
    print(f"To:          {out_file}")
    print(f"{'='*60}\n")

    if not redownload and out_file.exists():
        print(f"  {filename} (cached)")
        return True

    # Try direct download with a real browser user-agent
    print(f"  Downloading {filename}...", end="", flush=True)
    req = urllib.request.Request(url, headers={
        "User-Agent": ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                       "AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/131.0.0.0 Safari/537.36"),
    })
    try:
        with urllib.request.urlopen(req) as resp:
            out_file.write_bytes(resp.read())
        size_mb = out_file.stat().st_size / (1024 * 1024)
        print(f" ok ({size_mb:.1f} MB)")
        return True
    except urllib.error.HTTPError:
        print(" blocked by Cloudflare, trying browser...", flush=True)

    # Fall back to Playwright
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("  FAILED: install playwright to bypass Cloudflare")
        return False

    profile_dir = Path.home() / ".cache" / "axl-uefi-spec-browser"
    profile_dir.mkdir(parents=True, exist_ok=True)

    print("  Opening browser — solve captcha if prompted...", flush=True)
    try:
        with sync_playwright() as p:
            context = p.chromium.launch_persistent_context(
                str(profile_dir),
                headless=False,
                channel="chrome",
                args=["--disable-blink-features=AutomationControlled"],
            )
            page = context.pages[0] if context.pages else context.new_page()

            # Fetch the PDF as raw bytes via the browser's network stack
            # (bypasses Cloudflare since the browser session is authenticated)
            resp = page.request.get(url)
            if resp.ok:
                out_file.write_bytes(resp.body())
                context.close()
                size_mb = out_file.stat().st_size / (1024 * 1024)
                print(f"  ok ({size_mb:.1f} MB)")
                return True

            # If direct fetch fails, navigate to it and let the user
            # solve any captcha, then retry
            print(f"  Got {resp.status}, navigating to page...", flush=True)
            page.goto(url, timeout=120000)
            time.sleep(5)
            resp = page.request.get(url)
            if resp.ok:
                out_file.write_bytes(resp.body())
                context.close()
                size_mb = out_file.stat().st_size / (1024 * 1024)
                print(f"  ok ({size_mb:.1f} MB)")
                return True

            context.close()
            print(f"  FAILED: HTTP {resp.status}")
            return False
    except Exception as e:
        print(f"  FAILED: {e}")
        return False


def download_spec(context: object, spec_key: str, project_root: Path,
                  redownload: bool) -> bool:
    """Download a single spec using the given browser context.

    Returns True on success, False on failure.
    """
    spec = SPECS[spec_key]
    base_url = spec["base_url"]
    output_dir = project_root / spec["output_dir"]
    output_dir.mkdir(parents=True, exist_ok=True)

    page = context.pages[0] if context.pages else context.new_page()

    print(f"\n{'='*60}")
    print(f"Downloading: {spec['name']}")
    print(f"From:        {base_url}")
    print(f"To:          {output_dir}/")
    print(f"{'='*60}\n")

    page.goto(base_url, timeout=120000)

    # Wait for Sphinx content to render
    print("Waiting for spec page to load...", flush=True)
    for attempt in range(60):
        try:
            page.wait_for_selector("div.toctree-wrapper", timeout=3000)
            break
        except Exception:
            if attempt % 10 == 9:
                print("  Still waiting... solve the captcha if prompted.",
                      flush=True)
    else:
        print(f"Timeout waiting for {spec['name']}.", file=sys.stderr)
        return False

    print("Page loaded. Discovering chapters...")

    # Extract chapter links from the table of contents
    links: list[str] = page.eval_on_selector_all(
        "div.toctree-wrapper a.reference.internal",
        "els => els.map(e => e.getAttribute('href'))"
               ".filter(h => h && h.endsWith('.html'))"
    )

    # Deduplicate and sort
    seen: set[str] = set()
    chapters: list[str] = []
    for link in links:
        name = link.split("#")[0]
        if name and name not in seen:
            seen.add(name)
            chapters.append(name)

    print(f"Found {len(chapters)} chapters\n")

    # Save the index page
    index_file = output_dir / "index.html"
    if redownload or not index_file.exists():
        index_file.write_text(page.content(), encoding="utf-8")
        print(f"  Saved index.html")

    # Download each chapter
    failed = 0
    for i, chapter in enumerate(chapters, 1):
        out_file = output_dir / chapter
        if not redownload and out_file.exists():
            print(f"  [{i}/{len(chapters)}] {chapter} (cached)")
            continue

        url = f"{base_url}{chapter}"
        print(f"  [{i}/{len(chapters)}] {chapter}...",
              end="", flush=True)

        try:
            page.goto(url, timeout=30000)
            page.wait_for_selector("div.document", timeout=15000)
            time.sleep(0.5)

            out_file.parent.mkdir(parents=True, exist_ok=True)
            out_file.write_text(page.content(), encoding="utf-8")
            print(" ok")
        except Exception as e:
            print(f" FAILED: {e}")
            failed += 1

    total = sum(1 for _ in output_dir.glob("*.html"))
    print(f"\n{spec['name']}: {total} HTML files in {output_dir}/")
    if failed:
        print(f"  ({failed} chapters failed)")
    return failed == 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download UEFI-related HTML specifications")
    parser.add_argument(
        "specs",
        nargs="*",
        default=["all"],
        choices=list(SPECS.keys()) + ["all"],
        help="Which specs to download (default: all)")
    parser.add_argument(
        "--redownload",
        action="store_true",
        help="Re-download even if files already exist")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=Path("."),
        help="Project root directory (default: .)")
    args = parser.parse_args()

    # Resolve which specs to download
    if "all" in args.specs:
        spec_keys = list(SPECS.keys())
    else:
        spec_keys = args.specs

    # Split into PDF and HTML specs
    pdf_specs = [k for k in spec_keys if SPECS[k]["format"] == "pdf"]
    html_specs = [k for k in spec_keys if SPECS[k]["format"] == "html"]

    print("Specifications to download:")
    for key in spec_keys:
        fmt = SPECS[key]["format"].upper()
        print(f"  - {SPECS[key]['name']} ({fmt})")
    print()

    all_ok = True

    # Download PDFs first (no browser needed)
    for key in pdf_specs:
        ok = download_pdf(key, args.project_root, args.redownload)
        if not ok:
            all_ok = False

    # Download HTML specs via browser
    if html_specs:
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            print("Error: playwright not installed (needed for HTML specs)",
                  file=sys.stderr)
            print("  pip install playwright", file=sys.stderr)
            return 1

        profile_dir = Path.home() / ".cache" / "axl-uefi-spec-browser"
        profile_dir.mkdir(parents=True, exist_ok=True)

        print("If Cloudflare challenges you, solve it in the browser.")
        print("The script will continue automatically once each page loads.\n")

        with sync_playwright() as p:
            context = p.chromium.launch_persistent_context(
                str(profile_dir),
                headless=False,
                channel="chrome",
                args=["--disable-blink-features=AutomationControlled"],
            )

            for key in html_specs:
                ok = download_spec(context, key, args.project_root,
                                   args.redownload)
                if not ok:
                    all_ok = False

            context.close()

    if all_ok:
        print("\nAll specs downloaded successfully.")
    else:
        print("\nSome specs had errors.", file=sys.stderr)

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
