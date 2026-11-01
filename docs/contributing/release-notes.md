# Release notes

Every OrbitInk release body states only the purpose of that version's changes,
in 1-4 human-authored bullets, at most 100 characters per line. No installation,
license, security boilerplate, links or placeholders. Keep that material in the
unchanged distribution README and license assets.

`docs/release-notes.md` starts with a `source-sha256` comment, followed by the
bullets. CI removes the comment from the body. Both immutable build releases and
the mutable nightly channel use the dedicated staged `RELEASE-NOTES.md`.
Configure, stage, package and publish reject missing/stale notes; signing and
publication also compare the staged body with the committed source.

For a firmware change:

1. Commit the source change.
2. Run `python3 scripts/orbitink_ci.py notes-hash`.
3. Replace the marker and write this version's change goals in
   `docs/release-notes.md`, then commit it before pushing the complete change.

If source was already pushed with stale notes, commit the correction and
explicitly dispatch at the corrected main HEAD; a notes-only push skips CI.

The marker hashes tracked file paths, modes and Git object IDs, including the
SDK gitlink. It is independent of commit IDs, so the notes commit does not
invalidate itself. New firmware source content requires a new marker and a
maintainer's fresh notes; CI does not generate summaries or prove prose quality.

The exact five `paths-ignore` entries in `.github/workflows/orbitink.yml` are
editorial exceptions: this policy, the notes source, the publication helper,
its tests and that workflow. They are excluded from the hash. A push changing
only these files skips the firmware workflow; a push also changing any other
file still triggers it and must supply matching notes. Publication tooling or
workflow changes can be checked locally with:

```sh
python3 -m unittest discover -s scripts -p 'test_orbitink*.py'
```

Use an explicit owner dispatch when intentionally validating changed release
tooling end to end. An editorial push alone does not authorize a firmware
release. README, licenses, firmware and the uploaded asset list are unaffected
by this notes policy. Historical GitHub body edits remain a separate owner task.
