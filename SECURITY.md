# Security policy

## Supported versions

NucleoOS is developed as a rolling release. Security fixes land on `main` and ship in the next
tagged release; only the **latest release** is supported. Please reproduce any issue against the
current `main` or the newest tag before reporting.

## Reporting a vulnerability

Please report security vulnerabilities **privately** — do not open a public issue, and do not
disclose the details until a fix is available.

- Preferred: GitHub's private vulnerability reporting — open the repository's **Security** tab and
  choose **“Report a vulnerability.”** This creates a private advisory visible only to you and the
  maintainers.
- If that is unavailable, contact the maintainer through their GitHub profile
  ([@indecenti](https://github.com/indecenti)) and ask for a private channel before sending details.

When you report, please include: the affected component (firmware, web shell, a specific app, or a
tool under `tools/`), the version or commit, clear reproduction steps, and the impact you observed.

We aim to acknowledge a report within a few days, agree on a disclosure timeline with you, and
credit you in the release notes if you wish. NucleoOS is a noncommercial project with no bug-bounty
program, so reports are handled on a best-effort basis.

## Scope

**In scope** — defects that let an attacker compromise a NucleoOS device or its operator beyond the
tool's documented behavior, for example:

- the firmware HTTP/WebSocket API, OTA/update path, the SD/registry layer, or the pairing/PIN flow;
- the web shell or bundled apps (e.g. sandbox escape, cross-app data access, unsafe handling of
  attacker-controlled manifests, files, or network responses);
- the developer tooling under `tools/` (e.g. a host gate or deploy script that executes or exfiltrates
  attacker-controlled input).

**Out of scope** — the bundled security-testing tools (Wi-Fi/BLE/Ethernet/IR/USB-HID, Evil Portal,
BadUSB, the FIDO stack, etc.) doing exactly what they are designed to do. Those are offensive tools
by nature; their capabilities are not vulnerabilities. Their intended, lawful use is governed by the
**[Legal & responsible use](README.md#legal--responsible-use)** section of the README. Misuse of
NucleoOS against systems you do not own or are not authorized to test is your responsibility, not a
security defect in the project.
