# main: Engine composition entry

The Engine's `examples_common` component provides `app_main()` and startup order.
`game_entry.c` only selects `app_game_runtime_module()` through the unchanged
`raylib_lite_game_module_v1()` ABI. This getter has no arguments, never fails,
returns a borrowed immutable descriptor, and is thread-safe/nonblocking.

Private dependencies: the application runtime, Engine launcher and selected Board.
There are no exported application headers or business state here. The component
also embeds declared assets through the Engine's build helper; the empty template
has none. Dependency versions/overrides are centralized in `idf_component.yml`.
Build/run via the workspace commands in the root README. Host uses the same entry
and components, selected explicitly by `game.sim.json`.
