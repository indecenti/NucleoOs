# Contributing to NucleoOS

Contributions are welcome — bug reports, fixes, apps, docs.

## How to contribute

1. Open an issue first for anything non-trivial, so we can agree on the approach.
2. Fork, branch, keep changes focused, and match the surrounding code style
   (the codebase is **English-only**).
3. Verify with the host harness before flashing — `npm run validate`, `npm run anima:gate`,
   `npm run test:all` (see the README "Develop without flashing" section).
4. Open a pull request describing **what** changed and **why**.

## Contributor License Agreement (important)

NucleoOS is offered under a **dual model**: free under the
[PolyForm Noncommercial License](LICENSE), and under separate **paid commercial licenses**
(see [COMMERCIAL.md](COMMERCIAL.md)). For that to work, the project maintainer must be able to
license *all* of the code — including your contributions — commercially.

**By submitting a contribution (pull request, patch, or any code/content) you agree that:**

- You are the original author of the contribution, or you have the right to submit it.
- You grant **indecenti** (the maintainer) a perpetual, worldwide, non-exclusive,
  royalty-free, irrevocable license to use, reproduce, modify, distribute, sublicense, and
  **relicense** your contribution, **including under commercial terms**, as part of NucleoOS.
- Your contribution is also made available to everyone else under the project's
  PolyForm Noncommercial License.
- You provide it "as is", without warranty.

You keep the copyright to your contribution — this just lets the project be dual-licensed.
If you cannot agree to this, please don't submit code; open an issue instead.

## Third-party code

Do not paste code from projects whose license is incompatible with the above. New third-party
dependencies must be noted in [THIRD_PARTY.md](THIRD_PARTY.md) with their license — take special
care with copyleft (GPL/LGPL) components.
