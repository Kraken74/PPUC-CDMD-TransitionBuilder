# Transition Builder

A plugin for the PPUC Colorizing DMD editor (the Serum editor). It builds transitions for you on
new frames at the end of your project. The transitions are:

- **Dissolve**: the picture fades to black pixel by pixel, in a regular pattern.
- **Split Vertical**: the picture is cut down the middle; the left half slides off to the left
  and the right half slides off to the right.
- **Split Horizontal**: the picture is cut across the middle; the top half slides up and the
  bottom half slides down.

The last frame of a transition is never completely black. A dissolve ends with one pixel in 256
still lit, and a split ends with thin strips of the picture at the two edges (1/32 of each half,
at least one pixel wide).

## How to use it

1. Open your project and select the frame you want to start the transition from.
2. Choose `Plugins -> Transition Builder...`.
3. Pick the transition from the list and type how many frames it should last (1 to 256).
4. Click `Apply` (or `Cancel`).

The plugin adds that many new frames **at the end of the project**. Each new frame is a copy of
the frame you started from, one step further along the transition than the one before. The new
frames are selected afterwards, so you can use them right away (for example to
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

The new frames have **no comparison mask** (the mask of the frame you started from is not
copied; its shape mode is). They keep the frame's display time.

To change the display time of the new frames, use the editor's `Duration` field in the Frame
section: the plugin leaves the new frames selected, and `Duration` applies to all selected
frames. (The editor's plugin API cannot set a frame's duration, so the plugin has no box for it.)

## What it does not do (yet)

- It starts from the **current frame** only, not from several selected frames.
- **Backgrounds:** where a frame shows its background, those pixels stay as they are (they
  neither dissolve nor move), because the plugin API cannot change a frame's background mask. The plugin tells you when the frame
  has a background.
- Black pixels are black in the frame's original shades, in the SD and the HD picture, and they
  are taken out of the dynamic zones (a dynamic zone would color them at run time). In a split,
  the dynamic zones move with the picture.
- The new frames are plain frames. Turning them into a scene (with a trigger) is done in the
  editor as usual; the plugin API cannot create scenes.

## Adding another transition

Everything about a transition is in `src/Transitions.h`. A transition is one function that
answers, for a pixel (x, y) of the new frame at step N of M: which pixel of the starting frame
does it show, or is it black? A dissolve shows the pixel in the same place or black; a split
shows a pixel moved sideways. Write one (a wipe from the left, for instance, is black where
`x < width * step / steps` and the same pixel elsewhere), add one line to the list
`kTransitions`, and it appears in the drop-down list. Nothing else changes: the same answers
are used for the original shades, the SD picture, the HD picture and the dynamic zones.

## Files

- `plugin.cfg`: what the editor reads to find the plugin.
- `src/TransitionBuilder.cpp`: the plugin (menu entry, window, creating the frames).
- `src/Transitions.h`: the transitions.
- `include/`: the editor's plugin API (`ColorizingDMDPlugin.h`, version 8, from
  PPUC-Colorizing-DMD) and the VPX plugin SDK header (`plugins/MsgPlugin.h`).
- `tests/`: tests that run the plugin against a fake editor.

## License

GPL-3.0-or-later, like the editor and the headers in `include/`.
