# UI BIN viewer

A small [Vite](https://vitejs.dev/) + React + [Three.js](https://threejs.org/)
web app for inspecting the game's UI layout `.bin` files. It parses the
texture descriptor, section directory, and draw / helper records and
renders them as a navigable 3D scene, so you can see how a UI layout is
laid out and which textures it references.

This tool is independent of the PESMod build — it is a developer aid for
understanding the UI BIN format documented in
[`../../docs/INTERNALS.md`](../../docs/INTERNALS.md). The matching
[010 Editor](https://www.sweetscape.com/010editor/) template is at
[`../../docs/templates/ui_bin_generalized.bt`](../../docs/templates/ui_bin_generalized.bt).

## Running

Requires [Node.js](https://nodejs.org/) 18+.

```sh
npm install
npm run dev
```

Then open the printed local URL. In the page:

1. Load a UI layout `.bin` file with the file picker.
2. Optionally supply image files for the texture names the parser finds;
   un-supplied textures get a generated placeholder showing the name.
3. Move around with **WASD** and **mouse-look** (controls are listed in
   the side panel). Sections are separated along the Z axis so overlapping
   UI layers stay inspectable.

`npm run preview` serves a production preview on `127.0.0.1`.

## Layout

```
src/
├─ main.jsx        App entry
├─ UiBinScene.jsx  Three.js scene + camera controls
├─ parser.js       UI BIN parser (root, textures, sections, records)
└─ styles.css
```
