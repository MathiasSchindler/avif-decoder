# Security Policy

## Project Status and Scope

This repository is a research and experimental AVIF/AV1 decoder. It processes
complex, potentially untrusted image and container data using a freestanding C
implementation of ISOBMFF, AV1 decoding, image reconstruction, color conversion,
and output encoding.

The vast majority of the code in this repository has been produced with help
from large language models. There is no explicit or implicit statement that the
code is valid, complete, robust, secure, production-ready, or suitable for any
particular purpose.

At this time, this software should not be used in or near production. In
particular, do not rely on it to safely process untrusted or attacker-controlled
input, and do not use it as a security boundary. Dependency-free and
freestanding operation do not imply security or correctness.

Malformed AVIF, ISOBMFF, AV1, metadata, or image data may expose parser errors,
resource-exhaustion issues, memory-safety bugs, incorrect validation, or other
unexpected behavior. The project has not received a production security audit.

Optional threading adds worker-stack requirements. Grid threading also
multiplies decoder workspace by the selected width; sample-transform and CDEF
and restoration row threading do not. Direct AV1 bitstream-tile threading
shares frame-sized storage and adds bounded per-worker decode scratch.
Applications should cap executor width, enforce their own memory budgets, and
retain a width-1 fallback for untrusted or unusually large inputs.

Reduced-still queries may return substantially smaller AV1 workspace because
inter-frame references are impossible and exact pass-through filter planes can
alias. Frames permitting intra block copy retain full motion-bearing block
cells. Applications must still allocate the complete queried requirement and
must not reuse a plan across different inputs or executor widths.

Adaptive compressed PNG output requires the separate
`avifdec_png_workspace_requirement()` allocation. It is bounded by a fixed
458,752-byte compressor core plus three scanlines, rejects filtered streams
larger than `UINT32_MAX`, and reports row-provider or output-callback failures
instead of continuing with a partial success result.

## Reporting Security Issues

Security reports are welcome via mathiasschindler@github.com. Reports may also
be raised through the repository's normal public issue and discussion channels.

Useful reports include:

- memory-safety bugs or undefined behavior
- parser bugs triggered by malformed or adversarial input
- excessive CPU, memory, stack, or output consumption
- validation bypasses or incorrect acceptance of invalid input
- unsafe behavior in AVIF, ISOBMFF, AV1, metadata, color, or output handling
- build or release issues that could misrepresent generated artifacts

Please include a reproducer, affected commit, observed behavior, and relevant
platform or build details when practical.

## Disclosure Expectations

Open discussion of security issues is allowed. Because this repository is an
experimental research project and not production software, the maintainer does
not require embargoed or confidential handling of vulnerability reports.

Reporters are kindly invited to notify the maintainer before or while discussing
an issue publicly so it can be understood, documented, reproduced, fixed, or
mitigated where appropriate. This invitation is not a request to restrict
publication or discussion.

Please do not use vulnerability information to attack systems, mislead users,
or cause harm to third parties.

## Versions, Releases, and Advisories

This project does not currently publish formal supported releases. The latest
commit on the main branch should be treated as the most recent research version.
Older commits are not supported.

The maintainer does not currently plan to request CVE entries or publish formal
security advisories. Security-relevant changes may instead be documented in
issues, pull requests, or commit messages.

## Response Expectations

This is an independent research project, so response times may vary. The
maintainer will make a best-effort attempt to review actionable reports,
reproduce issues where possible, and publish fixes or mitigations when
appropriate.