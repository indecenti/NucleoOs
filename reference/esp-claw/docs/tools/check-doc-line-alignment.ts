#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
//
// SPDX-License-Identifier: Apache-2.0

import { promises as fs } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

type Severity = 'warning' | 'error';
type LineKind = 'empty' | 'dash' | 'header' | 'aside' | 'other';

type Finding = {
  severity: Severity;
  code: string;
  message: string;
  file?: string;
  line?: number;
};

type ParsedLine = {
  raw: string;
  kind: LineKind;
  dashCount?: number;
  headerLevel?: number;
  asideType?: string;
};

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const DOCS_ROOT = path.resolve(SCRIPT_DIR, '../src/content/docs');
const BASE_LANG = 'en';
const DOC_EXT_RE = /\.(md|mdx)$/i;

function addFinding(store: Finding[], finding: Finding): void {
  store.push(finding);
}

async function listImmediateDirs(root: string): Promise<string[]> {
  const entries = await fs.readdir(root, { withFileTypes: true });
  return entries.filter((e) => e.isDirectory()).map((e) => e.name).sort();
}

async function walkDocFiles(dir: string, rel = ''): Promise<string[]> {
  const abs = path.join(dir, rel);
  const entries = await fs.readdir(abs, { withFileTypes: true });
  const out: string[] = [];

  for (const entry of entries) {
    const nextRel = path.join(rel, entry.name);
    if (entry.isDirectory()) {
      out.push(...(await walkDocFiles(dir, nextRel)));
      continue;
    }
    if (!entry.isFile()) {
      continue;
    }
    if (DOC_EXT_RE.test(entry.name)) {
      out.push(nextRel.split(path.sep).join('/'));
    }
  }

  return out;
}

function parseLine(line: string): ParsedLine {
  if (line === '') {
    return { raw: line, kind: 'empty' };
  }

  if (/^-+$/.test(line)) {
    return { raw: line, kind: 'dash', dashCount: line.length };
  }

  const headerMatch = /^(#+)\s+/.exec(line);
  if (headerMatch) {
    return { raw: line, kind: 'header', headerLevel: headerMatch[1].length };
  }

  const asideMatch = /^<Aside\b([^>]*)>/.exec(line);
  if (asideMatch) {
    const attrs = asideMatch[1] ?? '';
    const typeMatch = /\btype\s*=\s*(?:"([^"]*)"|'([^']*)')/.exec(attrs);
    const asideType = typeMatch?.[1] ?? typeMatch?.[2];
    return { raw: line, kind: 'aside', asideType };
  }

  return { raw: line, kind: 'other' };
}

function formatFinding(f: Finding): string {
  const where = [f.file, f.line ? `:${f.line}` : ''].join('');
  const prefix = f.severity.toUpperCase();
  if (where) {
    return `${prefix} [${f.code}] ${where} ${f.message}`;
  }
  return `${prefix} [${f.code}] ${f.message}`;
}

async function main(): Promise<void> {
  const findings: Finding[] = [];

  const langDirs = await listImmediateDirs(DOCS_ROOT);
  if (langDirs.length === 0) {
    addFinding(findings, {
      severity: 'error',
      code: 'NO_LANG_DIR',
      message: `No language directories found under ${DOCS_ROOT}`,
    });
    flush(findings);
    process.exitCode = 1;
    return;
  }

  if (!langDirs.includes(BASE_LANG)) {
    addFinding(findings, {
      severity: 'error',
      code: 'MISSING_BASE_LANG',
      message: `Base language '${BASE_LANG}' directory not found under ${DOCS_ROOT}`,
    });
    flush(findings);
    process.exitCode = 1;
    return;
  }

  const fileMap = new Map<string, Set<string>>();
  for (const lang of langDirs) {
    const files = await walkDocFiles(path.join(DOCS_ROOT, lang));
    fileMap.set(lang, new Set(files));
  }

  const baseFiles = fileMap.get(BASE_LANG)!;

  for (const lang of langDirs) {
    if (lang === BASE_LANG) {
      continue;
    }

    const current = fileMap.get(lang)!;

    for (const rel of baseFiles) {
      if (!current.has(rel)) {
        addFinding(findings, {
          severity: 'error',
          code: 'MISSING_IN_LANG',
          file: rel,
          message: `Missing in '${lang}' but exists in '${BASE_LANG}'`,
        });
      }
    }

    for (const rel of current) {
      if (!baseFiles.has(rel)) {
        addFinding(findings, {
          severity: 'warning',
          code: 'NOT_IN_BASE_LANG',
          file: rel,
          message: `Exists in '${lang}' but not in '${BASE_LANG}'`,
        });
      }
    }
  }

  for (const rel of baseFiles) {
    const presentLangs = langDirs.filter((lang) => fileMap.get(lang)!.has(rel));
    if (presentLangs.length < 2) {
      continue;
    }

    const langLines = new Map<string, string[]>();
    for (const lang of presentLangs) {
      const abs = path.join(DOCS_ROOT, lang, rel);
      const raw = await fs.readFile(abs, 'utf8');
      const lines = raw.replace(/\r\n/g, '\n').split('\n');
      langLines.set(lang, lines);
    }

    const counts = [...langLines.entries()].map(([lang, lines]) => ({ lang, count: lines.length }));
    const expectedCount = counts[0].count;
    const countMismatch = counts.some((x) => x.count !== expectedCount);
    if (countMismatch) {
      addFinding(findings, {
        severity: 'error',
        code: 'LINE_COUNT_MISMATCH',
        file: rel,
        message: counts.map((x) => `${x.lang}:${x.count}`).join(', '),
      });
    }

    const maxLines = Math.max(...counts.map((x) => x.count));

    for (let i = 0; i < maxLines; i += 1) {
      const lineNo = i + 1;
      const parsed = new Map<string, ParsedLine>();

      for (const lang of presentLangs) {
        const lines = langLines.get(lang)!;
        const line = lines[i];
        parsed.set(lang, parseLine(line ?? ''));
      }

      for (const [lang, info] of parsed) {
        if (info.raw !== '' && /^\s+$/.test(info.raw)) {
          addFinding(findings, {
            severity: 'warning',
            code: 'WHITESPACE_BLANK_LINE',
            file: `${lang}/${rel}`,
            line: lineNo,
            message: 'Blank-looking line contains whitespace; use truly empty line',
          });
        }

        if (info.kind === 'dash' && (info.dashCount ?? 0) < 3) {
          addFinding(findings, {
            severity: 'warning',
            code: 'SHORT_DASH_LINE',
            file: `${lang}/${rel}`,
            line: lineNo,
            message: `Dash-only line has ${(info.dashCount ?? 0).toString()} '-' (expected >= 3)`,
          });
        }
      }

      const hasEmpty = [...parsed.values()].some((v) => v.kind === 'empty');
      if (hasEmpty) {
        const notEmpty = presentLangs.filter((lang) => parsed.get(lang)!.kind !== 'empty');
        for (const lang of notEmpty) {
          addFinding(findings, {
            severity: 'error',
            code: 'EMPTY_LINE_MISMATCH',
            file: `${lang}/${rel}`,
            line: lineNo,
            message: 'This line must be empty in all languages',
          });
        }
      }

      const hasDash = [...parsed.values()].some((v) => v.kind === 'dash');
      if (hasDash) {
        for (const lang of presentLangs) {
          const info = parsed.get(lang)!;
          if (info.kind !== 'dash' || (info.dashCount ?? 0) < 3) {
            addFinding(findings, {
              severity: 'error',
              code: 'DASH_LINE_MISMATCH',
              file: `${lang}/${rel}`,
              line: lineNo,
              message: "This line must be dash-only (at least '---') in all languages",
            });
          }
        }
      }

      const hasHeader = [...parsed.values()].some((v) => v.kind === 'header');
      if (hasHeader) {
        const levels = new Set<number>(
          [...parsed.values()].filter((v) => v.kind === 'header').map((v) => v.headerLevel ?? 0),
        );

        for (const lang of presentLangs) {
          const info = parsed.get(lang)!;
          if (info.kind !== 'header') {
            addFinding(findings, {
              severity: 'error',
              code: 'HEADER_LINE_MISMATCH',
              file: `${lang}/${rel}`,
              line: lineNo,
              message: 'This line must be a markdown heading in all languages',
            });
          }
        }

        if (levels.size > 1) {
          addFinding(findings, {
            severity: 'error',
            code: 'HEADER_LEVEL_MISMATCH',
            file: rel,
            line: lineNo,
            message: presentLangs
              .map((lang) => {
                const info = parsed.get(lang)!;
                return `${lang}:${info.kind === 'header' ? `h${info.headerLevel}` : info.kind}`;
              })
              .join(', '),
          });
        }
      }

      const hasAside = [...parsed.values()].some((v) => v.kind === 'aside');
      if (hasAside) {
        for (const lang of presentLangs) {
          const info = parsed.get(lang)!;
          if (info.kind !== 'aside') {
            addFinding(findings, {
              severity: 'error',
              code: 'ASIDE_LINE_MISMATCH',
              file: `${lang}/${rel}`,
              line: lineNo,
              message: 'This line must be an <Aside ...> tag in all languages',
            });
          }
        }

        const types = new Set<string>(
          presentLangs
            .map((lang) => parsed.get(lang)!)
            .filter((info) => info.kind === 'aside')
            .map((info) => info.asideType ?? '__MISSING__'),
        );

        if (types.size > 1) {
          addFinding(findings, {
            severity: 'error',
            code: 'ASIDE_TYPE_MISMATCH',
            file: rel,
            line: lineNo,
            message: presentLangs
              .map((lang) => {
                const info = parsed.get(lang)!;
                if (info.kind !== 'aside') return `${lang}:${info.kind}`;
                return `${lang}:${info.asideType ?? '<missing type>'}`;
              })
              .join(', '),
          });
        }
      }
    }
  }

  flush(findings);
  process.exitCode = findings.some((x) => x.severity === 'error') ? 1 : 0;
}

function flush(findings: Finding[]): void {
  const errors = findings.filter((f) => f.severity === 'error');
  const warnings = findings.filter((f) => f.severity === 'warning');

  for (const f of errors) {
    console.error(formatFinding(f));
  }
  for (const f of warnings) {
    console.warn(formatFinding(f));
  }

  const summary = `Summary: ${errors.length} error(s), ${warnings.length} warning(s)`;
  if (errors.length > 0) {
    console.error(summary);
  } else {
    console.log(summary);
  }
}

main().catch((err) => {
  console.error('FATAL', err instanceof Error ? err.message : String(err));
  process.exitCode = 1;
});
