/**
 * remark-doc-links.ts
 *
 * A remark plugin that rewrites `@lang/...` and `@base/...` aliases in
 * MDX / Markdown content to absolute paths at compile time, respecting
 * Astro's `base` config and the locale inferred from the file path.
 *
 * Aliases
 * ───────
 *   @lang/<rest>  →  {base}/{locale}/<rest>
 *   @base/<rest>  →  {base}/<rest>
 *
 * `base` is read from the Astro config injected into `file.data.astro`,
 * or from the `ASTRO_BASE` environment variable, or defaults to `/`.
 * When base resolves to `/` it is treated as an empty prefix so that
 * the resulting URLs are `/en/foo` rather than `//en/foo`.
 *
 * `locale` is extracted from the file path segment immediately after
 * `content/docs/`, e.g. `…/content/docs/zh-cn/…` → `zh-cn`.
 *
 * Node types handled
 * ──────────────────
 * • `link`                   – Markdown link:  [text](@lang/foo)
 * • `image`                  – Markdown image: ![alt](@base/foo.png)
 * • `mdxJsxFlowElement`      – Block-level JSX component
 * • `mdxJsxTextElement`      – Inline JSX component
 *   For JSX nodes, every attribute whose value is a **string literal**
 *   matching the alias pattern is rewritten.  Expression attributes such
 *   as `src={img.src}` have a non-string `value` and are skipped.
 *
 * Usage in astro.config.mjs
 * ─────────────────────────
 *   import { remarkDocLinks } from './src/plugins/remark-doc-links.ts';
 *
 *   export default defineConfig({
 *     // base: '/my-docs',   ← optional
 *     markdown: {
 *       remarkPlugins: [remarkDocLinks],
 *     },
 *   });
 */

import { visit } from 'unist-util-visit';
import type { Plugin } from 'unified';
import type { Root, Link, Image } from 'mdast';
import type {
  MdxJsxFlowElement,
  MdxJsxTextElement,
  MdxJsxAttribute,
} from 'mdast-util-mdx-jsx';
import type { VFile } from 'vfile';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** Options accepted by the remarkDocLinks plugin. */
export interface RemarkDocLinksOptions {
  /**
   * The Astro `base` path (e.g. `"/esp-claw/"`).
   * Pass `config.base` from `astro.config.mjs` directly so the plugin is
   * always in sync with the Astro config.
   * Falls back to the `ASTRO_BASE` env variable, then `/`.
   */
  base?: string;
}

/**
 * @deprecated Astro does NOT inject `config` into `file.data.astro` during
 * the remark pipeline – only frontmatter lands there.  Kept for reference.
 */
interface AstroVFileData {
  astro?: Record<string, unknown>;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Matches @lang/<rest> and @base/<rest> (rest is optional). */
const ALIAS_RE = /^@(lang|base)(\/.*)?$/;

/** Names of JSX attributes whose *string* values should be rewritten. */
const REWRITTEN_ATTRS = new Set(['href', 'src']);

/**
 * Extract the locale from the absolute source-file path.
 * Starlight stores content under `src/content/docs/<locale>/…`.
 * Falls back to `'en'` if the pattern is not found.
 */
function localeFromPath(filePath: string): string {
  // Normalise to forward slashes for uniform matching on all OSes.
  const normalised = filePath.replace(/\\/g, '/');
  const match = normalised.match(/\/content\/docs\/([^/]+)\//);
  return match ? match[1] : 'en';
}

/**
 * Normalise the `base` string from Astro config:
 *  - Ensure it has a leading `/`
 *  - Strip any trailing `/`
 *  - When `base` is `/` (i.e. no sub-path), return `''` so that
 *    concatenation yields `/en/foo` not `//en/foo`
 */
function normaliseBase(raw: string): string {
  let b = raw.replace(/\/+$/, '');
  if (!b.startsWith('/')) b = '/' + b;
  return b === '/' ? '' : b;
}

/**
 * Resolve an `@lang/…` or `@base/…` alias to an absolute path.
 * Returns the original string unchanged if it does not match.
 */
function resolveAlias(url: string, locale: string, base: string): string {
  const m = url.match(ALIAS_RE);
  if (!m) return url;

  const kind = m[1]; // 'lang' | 'base'
  const rest = m[2] ?? '/'; // e.g. '/foo/bar' or '/'
  const suffix = rest.startsWith('/') ? rest : '/' + rest;

  return kind === 'lang'
    ? `${base}/${locale}${suffix}`
    : `${base}${suffix}`;
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

// `Plugin<[RemarkDocLinksOptions?], any>` — the `any` tree type is intentional:
// the standard `Root` type from @types/mdast does not include MDX extension
// node types (mdxJsxFlowElement, mdxJsxTextElement), so restricting to `Root`
// would cause spurious TS errors.  The runtime tree is still a valid mdast Root.
export const remarkDocLinks: Plugin<[RemarkDocLinksOptions?], any> = (
  options = {}
) => {
  return (tree: Root, file: VFile) => {
    // ── Derive base ────────────────────────────────────────────────────────
    // Priority: plugin options → ASTRO_BASE env var → "/"
    // NOTE: file.data.astro contains frontmatter, NOT the Astro config, so
    // reading config.base from there does not work.
    const rawBase = options?.base ?? process.env.ASTRO_BASE ?? '/';
    const base = normaliseBase(rawBase);

    // ── Derive locale ──────────────────────────────────────────────────────
    const locale = localeFromPath(file.path ?? '');

    // ── Walk the AST ───────────────────────────────────────────────────────
    visit(tree, (node) => {
      switch (node.type) {
        // ── Markdown link: [text](@lang/foo) ────────────────────────────
        case 'link': {
          const n = node as Link;
          if (ALIAS_RE.test(n.url)) {
            n.url = resolveAlias(n.url, locale, base);
          }
          break;
        }

        // ── Markdown image: ![alt](@base/img.png) ───────────────────────
        case 'image': {
          const n = node as Image;
          if (ALIAS_RE.test(n.url)) {
            n.url = resolveAlias(n.url, locale, base);
          }
          break;
        }

        // ── JSX components (block + inline) ─────────────────────────────
        // e.g. <LinkCard href="@lang/foo" />
        //      <img src="@base/img.png" />
        //      <video src="@base/clip.mp4" />
        //
        // src={xxx.src} is a MdxJsxAttributeValueExpression (not a string),
        // so `typeof attr.value === 'string'` naturally skips it.
        case 'mdxJsxFlowElement':
        case 'mdxJsxTextElement': {
          const n = node as MdxJsxFlowElement | MdxJsxTextElement;
          for (const attr of n.attributes) {
            if (attr.type !== 'mdxJsxAttribute') continue;
            const a = attr as MdxJsxAttribute;
            if (
              REWRITTEN_ATTRS.has(a.name) &&
              typeof a.value === 'string' &&
              ALIAS_RE.test(a.value)
            ) {
              a.value = resolveAlias(a.value, locale, base);
            }
          }
          break;
        }
      }
    });
  };
};
