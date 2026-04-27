# Background

This repository tracks an open-source effort for the FocalTech FT9366 fingerprint sensor (USB 2808:a658), commonly found in certain ASUS Vivobook systems.

Problem summary:
- Device has historically depended on an unavailable proprietary binary workflow.
- Existing shim approaches are unstable because encrypted command/session behavior is not fully replicated.
- The most critical blocker is chipid initialization returning zero in broken paths.

Primary goal:
- Implement a clean, auditable Linux driver path for reliable enroll/verify using libfprint-compatible architecture, with protocol findings documented in the `research/` directory.

See `research/PROTOCOL.md` for ongoing reverse-engineering evidence and reproducible steps.
