# Transition Builder

A plugin for the PPUC Colorizing DMD editor (the Serum editor). It builds transitions for you on
new frames at the end of your project. The first transition is **Dissolve**: the picture fades
to black pixel by pixel, in a regular pattern.

## How to use it

1. Open your project and select the frame you want to start the transition from.
2. Choose `Plugins -> Transition Builder...`.
3. Pick the transition from the list and type how many frames it should last (1 to 256).
4. Click `Apply` (or `Cancel`).

The plugin adds that many new frames **at the end of the project**. Each new frame is a copy of
the frame you started from with more black pixels than the one before; the last one is fully
black. The new frames are selected afterwards, so you can use them right away (for example to
make them a scene). It is all one step of the editor's history: **one `Undo` removes all the
new frames.** The frame you started from is never changed.

## Installing it

1. Get the plugin folder `TransitionBuilder` (see "Getting the plugin" below). It contains
   `plugin.cfg` and `plugin-transitionbuilder64.dll`.
2. In the editor, open `Settings -> Plugins` and click `Open Folder`. Put the whole
   `TransitionBuilder` folder into that folder (usually
   `%APPDATA%\ppuc.org\PPUC Colorizing DMD\plugins`), so that `plugin.cfg` is at
   `...\plugins\TransitionBuilder\plugin.cfg`.
3. Restart the editor, open `Settings -> Plugins`, and check `Transition Builder`.
4. A `Plugins` menu appears with the entry `Transition Builder...`.

If the plugin does not load, `Settings -> Plugins` names the reason, and `Help -> Plugin Log`
has more.

## Getting the plugin (Windows, no programming tools needed)

GitHub builds it for you:

1. Make a free account at github.com and create a new repository (private is fine).
2. Upload everything in this folder to it, including the hidden `.github` folder
   (`Add file -> Upload files`; drag the whole folder in).
3. Open the repository's `Actions` tab. The `Build Windows plugin` run starts by itself after
   the upload (or choose it and click `Run workflow`).
4. When the run is green, open it and download `TransitionBuilder-windows-x64` under
   `Artifacts`. Unpack it: it holds the `TransitionBuilder` folder from the steps above.

## What it does not do (yet)

- It starts from the **current frame** only, not from several selected frames.
- **Backgrounds:** where a frame shows its background, those pixels do not dissolve, because
  the plugin API cannot change a frame's background mask. The plugin tells you when the frame
  has a background.
- Pixels that dissolve are black in the frame's original shades, in the SD and the HD picture,
  and they are taken out of the dynamic zones (a dynamic zone would color them at run time).
- The new frames are plain frames. Turning them into a scene (with a trigger) is done in the
  editor as usual; the plugin API cannot create scenes.
- The last frame is completely black, so its original is an all-black frame. Keep that in mind
  if your ROM also shows a blank screen.

## Adding another transition

Everything about a transition is in `src/Transitions.h`. A transition is one function that says
whether the pixel at position (x, y) is black at step N of M. Write one (a wipe from the left,
for instance, is `x < width * step / steps`), add one line to the list `kTransitions`, and it
appears in the drop-down list. Nothing else changes. The same pattern is used for the original
shades, the SD picture, the HD picture and the dynamic zones.

## Files

- `plugin.cfg`: what the editor reads to find the plugin.
- `src/TransitionBuilder.cpp`: the plugin (menu entry, window, creating the frames).
- `src/Transitions.h`: the transitions.
- `include/`: the editor's plugin API (`ColorizingDMDPlugin.h`, version 8, from
  PPUC-Colorizing-DMD) and the VPX plugin SDK header (`plugins/MsgPlugin.h`).
- `tests/`: tests that run the plugin against a fake editor.

## License

GPL-3.0-or-later, like the editor and the headers in `include/`.
