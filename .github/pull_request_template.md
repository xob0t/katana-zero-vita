## What and why

<!-- What changes in the game, where, and the measured gain (ms/frame, fps) with the harness or room used. -->

## Checklist

- [ ] Exact or visible? A visible change has a row in `docs/changes.md`; any new `yyl.cfg` key has a row in
      `docs/settings.md` (default, range, effect).
- [ ] Off-switch: the `exp_*` flag (or setting) that turns the change off is named here and in the file's header
      comment.
- [ ] The module owns its report: an `xxx_report(int frames)` in the feature's own file, not a new block in
      `katana_prof.c`.
- [ ] Verifier: the command or `exp_*verify` flag that checks the change, and its result on this branch.
- [ ] Install order: any dependency on another install (runs after X, wraps Y, hooks the same function as Z) is stated
      here and in a comment at the install site. No second `entry_hook` on a function another module already hooks
      (the `[HOOKS]` log line names conflicts).
- [ ] Built with `uv run build.py --no-deps` ("Built target" present, no errors); tested on a Vita (rooms, fps).
