# Experimental tree

This checkout is the unpublished C layout, packaging identifier
`0.0.0_pre8-r0`. It has no install path that is ready for a router.

The 7.x getting-started guide, `auto_config` test, and image instructions
belong to branch `7.3.2beta` at
`4988d06ab817213c57b9cd39b4beb3d8a2731f80`. Do not apply that procedure to
`mesh11sd-v8`.

On a device the experimental package installs disabled. `enabled` is `0` in
`/etc/config/mesh11sd_v8_runtime`. Image builds and apk install/upgrade do not
enable or start the service. Turning it on would be a later, explicit change
on the two GL-MT3600BE boards, after flashed firmware compatibility is
checked. That check has not been done.

Host reading of the planner, with no network changes:

```sh
make host-checks
```

See `README.md` for the layout, the parent-box limits, and the behavior that
is still unfinished.
