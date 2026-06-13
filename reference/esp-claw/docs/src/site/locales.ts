export type Lang = "en" | "zh-cn";

export const SITE_LOCALES: { id: Lang; label: string }[] = [
  { id: "en", label: "English" },
  { id: "zh-cn", label: "中文" },
];

export function localeHref(lang: Lang, pagePath: string): string {
  const base = (import.meta.env.BASE_URL ?? "/").replace(/\/+$/, "");
  const p = pagePath.replace(/^\/+|\/+$/g, "");
  if (!p) return `${base}/${lang}/`;
  return `${base}/${lang}/${p}/`;
}

export function htmlLangAttribute(lang: Lang): string {
  return lang === "zh-cn" ? "zh-CN" : "en";
}
