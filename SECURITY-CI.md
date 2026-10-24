# Security CI

Three workflows (`semgrep.yml`, `codeql.yml`, `secops.yml`) check every pull request, every push to `upgrade/vault` and the default branch on a
schedule. Results go to the run summary and to GitHub code scanning (Security → Code scanning, one
category per tool). Only Semgrep and Dependabot ever open pull requests, and each keeps a fixed small
number open, updated in place.

| Leg | Workflow / job | Gate on a PR | Results | Accept a finding |
|---|---|---|---|---|
| Semgrep (pinned community rule packs + `contrib/semgrep/rules`) | `semgrep.yml` | fails on a new **critical** finding the PR adds | summary, code scanning `semgrep`, at most four rolling PRs `semgrep/<bucket>` | `contrib/semgrep/accepted.json` (fingerprint or rule × paths, with reason), or `config.json` `exclude_rules` / `audit_rules` / `bucket_overrides` — see `contrib/semgrep/README.md` |
| Semgrep local rule tests | `secops.yml` `semgrep-rules` | fails when a rule in `contrib/semgrep/rules` misses or over-matches its annotated test file | log | fix the rule or its test (`<name>.yaml` beside `<name>.cpp`) |
| CodeQL (`c-cpp`, `python`, `rust`, `actions`; `security-extended`, build-mode none) | `codeql.yml` | never fails; new alerts show on the PR | code scanning `codeql-<language>` | an entry (tool + rule + path glob, reason, comment) in `contrib/secops/code-scanning-dismissals.json`, applied by `code-scanning.yml`; not by hand in the web UI |
| zizmor (workflow security) | `secops.yml` `zizmor` | fails on any finding in `.github/` | log, code scanning `zizmor` | fix the workflow; a justified exception is an inline `# zizmor: ignore[<audit>]` with the reason on the same line |
| gitleaks (secrets) | `secops.yml` `gitleaks` | fails on a leak in the PR's commits; nightly scans full history | code scanning `gitleaks` (values redacted) | test vectors, public constants and documentation examples only: add the fingerprint to `.gitleaksignore` under a reason comment. A real secret is rotated, never listed |
| OSV-Scanner (every lockfile) | `secops.yml` `osv-scanner` | fails on a known vulnerability | summary table, code scanning `osv-scanner` | fix it with a lockfile bump within the pin rules, or add `[[IgnoredVulns]]` to `osv-scanner.toml` with `reason` and `ignoreUntil` at most 90 days out |
| cargo-deny (root `Cargo.lock`: advisories, bans, sources) | `secops.yml` `cargo-deny` | fails on an advisory, a ban or an unknown source | log | `contrib/secops/deny-node.toml` `ignore` with reason (mirror it in `osv-scanner.toml`) |
| cargo-audit + cargo-deny (attestor agent) | `yellowback-tests.yml` `agent` | as before | log | `contrib/yellowback/attest/deny.toml` |
| Code-scanning triage | `code-scanning.yml` (push changing the ledger, nightly, by hand) | never | dismisses the listed alerts of every tool above; open alerts in the step summary and the `code-scanning-alerts` artifact | the ledger itself: `contrib/secops/code-scanning-dismissals.json` |
| Dependabot | `.github/dependabot.yml` | — | one grouped security PR per ecosystem (cargo, pip, github-actions, docker); version updates are off | close the PR with a reason |

Schedules: Semgrep 02:17 UTC nightly, secops 03:43 UTC nightly, code-scanning triage 03:53 UTC nightly, CodeQL Mondays 04:41 UTC.

Tools are pinned: actions by commit SHA, release binaries by version and sha256 (in `secops.yml`), pip
installs hash-locked (`contrib/semgrep/requirements.txt`, `contrib/secops/zizmor-requirements.txt`).
`contrib/semgrep/security-overrides.txt` moves Semgrep's own transitive dependencies to fixed versions
when an advisory lands before Semgrep releases a fix.

Consensus code (`src/consensus`, `src/script`, `src/primitives`, `src/pow`, `src/main.cpp`) keeps its
review gate whatever a scanner says: findings there are triaged and reported, never auto-fixed.

## Repository settings these need

- Code scanning results need GitHub code scanning (free on public repositories). Keep CodeQL **default
  setup off**: `codeql.yml` is an advanced configuration, and the two cannot run together.
- Settings → Code security: enable **Dependabot security updates** (Dependabot alerts on).
- Semgrep's rolling PRs: "Allow GitHub Actions to create and approve pull requests", and optionally the
  `SEMGREP_PR_TOKEN` secret (see the header of `semgrep.yml`).
