# Security Policy

## Supported Versions

Only the latest release receives security updates during the pre-1.0 phase.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | ✅        |
| < 0.1   | ❌        |

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Report privately via GitHub's **Private Vulnerability Reporting**:

1. Go to the **Security** tab of this repository.
2. Click **Report a vulnerability**.

Alternatively, email: <hha0x617@users.noreply.github.com>

Please include:

- Affected version / commit hash / plugin name
- Steps to reproduce
- Impact assessment
- Proof-of-concept if available

## Response

- Initial response: within 7 days
- Fix timeline depends on severity and complexity
- A GitHub Security Advisory will be published after the fix is released
- Reporters will be credited unless they request anonymity

## Out of Scope

- Vulnerabilities in guest operating systems (NetBSD, Linux) running inside
  a host that loads these plugins — report those to the upstream project
- Issues requiring physical access to the host machine
- Denial of service caused by crafted ROM, disk, or configuration files —
  the plugins are not sandboxes and are not designed to execute untrusted
  guest software safely
- Vulnerabilities in third-party dependencies (Rust crates, Windows SDK,
  etc.) unless directly exploitable through this project's code
- Vulnerabilities in the upstream source trees vendored as submodules
  (`external/em6809`, `external/em68030_WinUI3Cpp`) — report those to the
  respective upstream repositories
