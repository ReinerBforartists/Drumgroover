// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// =========================================
// HOW TO — content of the How-To tab's accordion sections (markup: index.html, #howto-*)
// =========================================

function howtoSet(id, html) {
  const el = document.getElementById(id);
  if (el) el.innerHTML = html;
}

howtoSet('howto-overview', `
  DrumGroover is a drum pattern generator and a drum synthesizer in one VST3 instrument. It writes drum patterns for you, plays them with its own built-in drum sounds, and hands the result to your DAW as MIDI.<br>
  <b>Generate.</b> Six instrument groups — Kick, Snare, Hi-Hat, Ride, Toms, Cymbals — plus extra percussion on custom tracks. Every group has a few knobs (how busy, how varied, which grid) and a reroll button; the pattern regenerates as you turn them.<br>
  <b>Arrange.</b> Patterns live in blocks (Verse, Chorus, Bridge, Breaks, Fills). The Song Generator strings blocks together into a song.<br>
  <b>Play.</b> The sound is made inside the plugin. The <b>808</b> engine models the circuits of a classic analog drum machine; the <b>Lo-Fi</b> engine is a simpler alternative. Behind it sits a mixer with effects and sends. The plugin has a stereo output and also plays whatever MIDI notes your DAW or keyboard sends it — every instrument has a fixed MIDI note (Options → Show GM Notes in the Note Editor shows them).<br>
  <b>Take it with you.</b> Export the song or single blocks as .mid files, or drag them out of the plugin into your DAW. The whole state — patterns, song, sound settings and mixer — is also stored inside your DAW project.<br>
  <b>The tabs:</b> <b>Note Editor</b> (patterns), <b>Song Generator</b> (arrangement), <b>Mixer</b>, <b>Drum Engine</b> (sound), and this <b>How To</b>.
`);

howtoSet('howto-quickstart', `
  <b>1. Set the frame.</b> On the right of the Note Editor, the sidebar holds <b>Genre</b>, <b>Time Signature</b>, <b>Snap</b> and the block's <b>Length</b> and <b>Feel</b>. Genre is only a starting point: it sets a time signature and a baseline Feel and applies matching preset values to Verse, Chorus and Bridge — everything it sets can be changed afterwards. The tempo comes from your DAW.<br>
  <b>2. Shape the groove.</b> The knob clusters above the grid (Kick, Snare, Hi-Hat, Ride, Toms, Cymbals) are the actual instrument. Turn <b>Subdiv</b>, <b>Dens</b> and <b>Var</b> — the pattern of that group regenerates immediately. Details: <i>Note Editor — generating patterns</i>.<br>
  <b>3. Not the right hit?</b> The 🎲 next to a cluster title rerolls that group with a new random seed and the same knob values. <b>Edit ▾ → Reroll</b> rerolls the whole open block or all blocks.<br>
  <b>4. Use blocks.</b> The row above the grid holds your blocks. Click one to open it, <b>+ Add Block ▾</b> to create more. Each block has its own Feel and its own knob values.<br>
  <b>5. Arrange them.</b> Open the <b>Song Generator</b> tab. <b>Generate Song ▾</b> lays out a complete structure from the blocks you have, or drag blocks onto the timeline yourself.<br>
  <b>6. Listen.</b> Press <b>▶ Play</b> in the plugin, or press play in your DAW — the plugin follows the DAW (see <i>Playing &amp; DAW sync</i>).<br>
  <b>7. Shape the sound.</b> The <b>Mixer</b> tab has faders, pan, effects and sends; the <b>Drum Engine</b> tab changes how each instrument sounds.<br>
  <b>8. Get it out.</b> <b>File ▾ → Export Song</b>, or drag a clip or block out of the plugin into your DAW (see <i>Export, drag to DAW &amp; projects</i>).
`);

howtoSet('howto-playback', `
  <b>Play button.</b> <b>▶ Play</b> / <b>■ Stop</b> play inside the plugin at the tempo of your DAW. In the <b>Note Editor</b> it loops the open block; in the <b>Song Generator</b> and the <b>Mixer</b> it plays the whole song. The Drum Engine and How To tabs have no playback — switching to a tab of the other playback mode stops it. Edits you make while it plays are picked up live.<br>
  <b>DAW transport.</b> While your DAW plays, the plugin plays the song from the Song Generator at the DAW's position: the DAW's position zero is the start of the song. Jumping around in the DAW is followed. Without a loop range the song plays once and then stays silent; with one, it loops once the DAW position reaches the range.<br>
  <b>Loop range.</b> In the Song Generator drag on the thin loop ruler above the clips to mark a range (at least one bar); click the ruler to clear it. The Note Editor has the same kind of ruler for looping part of the open block.<br>
  <b>MIDI notes.</b> MIDI notes sent to the plugin play the matching instrument right away, so you can play the kit from a keyboard or from another MIDI track.<br>
  <b>Mute.</b> The <b>Mute</b> button (top right) silences DrumGroover's output completely, independently of the DAW's own mute.<br>
  <b>Where is the tempo?</b> There is no BPM setting in the plugin: tempo always comes from the DAW. The time signature is set per project in the Note Editor sidebar.
`);

howtoSet('howto-generator', `
  The Note Editor builds patterns from groups. A <b>group</b> is one instrument family with its articulations, e.g. Kick, Snare (center hit, rim shot, side stick, ...), Hi-Hat (closed, tight, open, ...), Ride, Toms, Cymbals. Every group has its own knobs:<br>
  <b>Subdiv</b> — the grid the group's pattern is generated on. Coarser (8th) keeps it simple, finer (up to 64th) allows busier, more detailed patterns. This is independent of the Snap grid, which only affects notes you place by hand.<br>
  <b>Dens</b> (Density) — how many of the grid's steps actually get a hit.<br>
  <b>Var</b> (Variation) — the chance of an alternate articulation instead of the plain hit (for example rim shot or edge on the snare, open hi-hat, bell or mute on the ride, other tom voices, other crash voices). It is weighted, not a hard split.<br>
  <b>Ghost</b> (Snare only) — the density of extra quiet notes that fill the gaps between the main hits.<br>
  <b>Phrase length</b> — how many bars are generated independently before the pattern repeats. It is set per instrument: expand a group in the grid (click its header) and use the <b>Phrase Length</b> row at the bottom — drag the handle at the end of the first segment; it snaps to whole bars and regenerates when you let go.<br>
  <b style="display:block;margin-top:12px">Reroll and locking</b>
  The 🎲 next to a group's title keeps the knob values and tries a new random seed. <b>Edit ▾ → Reroll → Active Block</b> rerolls everything unlocked in the open block (for Breaks, Intros and Outros: their own dials and their Kick); <b>All Blocks</b> does that for all regular blocks (Break and Intro/Outro blocks are skipped — reroll those individually). The 🎲 next to <b>Main</b> does the same as Active Block.<br>
  If you edit a note of a group by hand, that whole group is <b>locked</b>: a 🔒 appears next to its title and its knobs and reroll stop changing it. Click the 🔒 to discard your edits and hand the group back to the knobs, or use <b>Edit ▾ → Discard → All Edits (Block)</b> / <b>(Global)</b>.<br>
  <b>Shift+drag</b> one of the six Density knobs to scale all six proportionally (Energy macro, one undo step).<br>
  <b style="display:block;margin-top:12px">The Main cluster</b>
  <b>Randomize</b> jitters the timing and velocity of the <i>selected</i> notes by the chosen amount (applied when you let go). <b>Nudge</b> shifts the selected notes by an eighth of a Snap step per turn — fine timing feel, not quantized. Both do nothing without a selection.<br>
  <b style="display:block;margin-top:12px">Custom tracks</b>
  <b>+ Add Track</b> adds an extra percussion instrument (High Conga, Claves, Hand Clap, Maracas, Cowbell, PC-2). Pick the instrument and its MIDI note number in the dialog; each instrument can be used once. Its knobs are in the Custom cluster, its sound in the Drum Engine tab (Custom).
`);

howtoSet('howto-ne-edit', `
  <b>Click</b> empty grid space to add a note (snapped to the Snap grid); clicking where a note already sits on that row replaces it.<br>
  <b>Drag</b> from empty grid space to rectangle-select every note the box touches. Hold <b>Shift</b> while dragging to delete everything it touches instead.<br>
  <b>Ctrl+drag</b> along one instrument's row is the pencil: starting on empty grid paints a note at every snap point the drag crosses; starting on an existing note erases every note it crosses.<br>
  <b>Right-drag</b> along a row is a dedicated eraser. <b>Right-click</b> a note deletes it; right-click empty row space just deselects.<br>
  <b>Drag</b> a note to move it (snaps to the grid); drop it on another row to reassign it to that instrument. A note that is part of a multi-selection moves the whole selection.<br>
  <b>Velocity lane</b> (under the grid) shows the selected notes, or if nothing is selected the notes of the row you last clicked. With a selection it also offers <b>Vel</b> (shift all selected velocities), <b>Dyn</b> (more or less contrast around their average) and <b>Rnd</b> (random spread). Drag a note's handle to change its velocity (with several notes selected, the whole selection moves together). Drag elsewhere in the lane to paint velocity freehand across the notes the pointer crosses; <b>Shift+drag</b> draws a straight ramp.<br>
  <b>Triplet / Straight zone.</b> Expand a group and drag on its zone row to mark a range that plays the opposite subdivision; drag the edges of a marked range to resize it, right-click it to remove it.<br>
  <b>Arrow keys</b> nudge the selection (Left/Right in time by one Snap step, Shift = 4×; Up/Down switch each selected note to the previous/next articulation of its group). <b>Ctrl+C / X / V</b> copy, cut and paste at the playhead; <b>Ctrl+A</b> selects all, <b>Ctrl+I</b> inverts the selection.<br>
  <b>Zoom and view.</b> Ctrl+mouse wheel zooms at the cursor, middle-drag pans, <b>F</b> fits the view, <b>Tab</b> hides or shows the sidebar.<br>
  <b>Mute / Solo (M / S on a group's header)</b> silence or isolate a whole group while you work: they change the Note Editor's own playback (<b>▶ Play</b> on this tab) and the sounds you trigger by clicking. Solo is exclusive and overrides mute. They do not affect the Song Generator, the Mixer's playback, your DAW or exports — use the Mixer to mute channels there.<br>
  <b>GM notes.</b> <b>Options → Show GM Notes</b> adds an editable MIDI note number to every row, to remap an instrument to another note without touching your DAW's mapping.<br>
  <b>Snap to Grid</b> (Edit ▾) moves the selected notes back onto the current grid.
`);

howtoSet('howto-blocks', `
  <b>Blocks</b> are patterns. The row above the grid is the block palette: <b>click</b> a block to open it in the Note Editor, <b>double-click</b> to rename it, <b>right-click</b> to delete it (right-drag deletes several). Regular blocks are Verse, Chorus and Bridge; you can add more.<br>
  <b>+ Add Block ▾</b> (Note Editor) creates an <b>Empty Block</b>, an <b>Intro-style Fill</b> (resolves into what follows), an <b>Outro-style Fill</b> (sits at bar 1 and trails silent), or a <b>Duplicate</b> of the open block. <b>Clear All</b> deletes every block and everything they placed on the timeline.<br>
  Each block has its own <b>Length (Bars)</b>, its own <b>Feel</b> (Straight, Swing or Half-Time — it applies only to this block) and its own copy of all group knobs. <b>Fill Every</b> (the sidebar group is called <b>Turnaround (Fill)</b>) lets a block end with a small fill, a turnaround, every N bars; the <b>Fill</b> and <b>Fill Kick</b> dials in the toolbar (Subdiv and Amnt each) shape that fill.<br>
  <b style="display:block;margin-top:12px">Break blocks</b>
  The small <b>+</b> that appears when you hover a regular block adds a <b>Break</b> for that block (it does not place anything on the timeline). A Break has two zones: a <b>copied groove zone</b> — a locked, live mirror of its parent block, shown grayed out — followed by the <b>break-tail</b>, the Break's own generated fill and kick, which you can edit freely. The <b>Break</b> and <b>Break Kick</b> dials in the toolbar (Subdiv and Amnt each) shape them, and <b>Length (Beats)</b> in the Break panel sets how much of the block is break-tail.<br>
  <b>Edit whole block</b> (checkbox in the Break panel) lets you hand-edit the copied groove zone too, which then stops following the parent. Turning it off again discards those edits (and any locked Break/Break Kick edits) and re-syncs the block with its parent.<br>
  Hand-editing a note in the break-tail locks that group (Break or Break Kick): a 🔒 appears and its knobs and reroll stop affecting it until you discard the edit (click the 🔒 or use <b>Edit ▾ → Discard → All Edits (Block)</b>). The 🎲 on <b>Break</b> and <b>Break Kick</b> skips whichever half is locked.<br>
  <b>Intro/Outro-style fills</b> are separate blocks. An Intro block has <b>Intro</b> and <b>Intro Kick</b> dials in the toolbar and an <b>Intro instruments</b> list in the sidebar; an Outro block has the same with <b>Outro</b> in place of Intro.<br>
  Turning group knobs only regenerates parts that are still algorithmically generated; hand-edited groups stay as they are.
`);

howtoSet('howto-song', `
  The Song Generator is the playback order: a timeline in bars with one clip per block.<br>
  <b>Generate Song ▾</b> lays out a complete structure from the blocks you already have — for example 12 Bar Basic, AABA, ABAB, ABCABC, Chorus First, Standard Pop or Extended Pop. Every structure sits between Intro and Outro, and each letter is a section: <b>A</b> = three Verse clips plus the Verse Break, <b>B</b> = the same with Chorus, <b>C</b> = the same with Bridge (hover an entry to see its order). It only arranges existing blocks; it creates no new content, so the patterns are still shaped in the Note Editor.<br>
  <b>Drag</b> a block from the palette onto the timeline to insert a copy where you drop it. <b>Drag</b> a clip to move it; hold <b>Shift</b> while dragging to duplicate it. Whatever is in the way is pushed aside just enough to make room — clips never overlap, and a vacated spot stays an empty gap instead of closing up. <b>Consolidate</b> (in the Generate Song menu) closes all gaps while keeping the order.<br>
  <b>Double-click</b> a clip to open its block in the Note Editor. <b>Right-click</b> a clip to delete it (right-drag deletes several); nothing else shifts. <b>Clear All</b> (also in the Generate Song menu) removes every clip but leaves the blocks.<br>
  <b>Selecting.</b> Click selects one clip, Shift+click selects a range, Ctrl/Cmd+click toggles. Drag on empty space to lasso-select (Shift adds, Ctrl/Cmd toggles). <b>Delete</b> removes the selection.<br>
  <b>View.</b> The Zoom slider or Ctrl+mouse wheel zooms, <b>F</b> fits the selected clips (or resets to 100%), and the handle under the timeline changes the clip height (double-click resets).<br>
  <b>Loop.</b> Drag on the thin loop ruler above the clips for a loop range of at least one bar; click it to clear.<br>
  <b>Drag out.</b> Dragging a clip out of the plugin window onto your DAW inserts the whole song as MIDI (see <i>Export, drag to DAW &amp; projects</i>).
`);

howtoSet('howto-mixer', `
  The Mixer has one strip for every instrument channel — Kick, Snare, Hi-Hat, five toms (Ra Tom 1–3, Fl Tom 1–2), five cymbals, Ride and Custom (all custom tracks) — plus a <b>Kit Gain</b> strip, the <b>Master</b>, and a <b>Reverb</b> and a <b>Delay</b> bus. The mixer settings are saved with the project.<br>
  <b>Signal path.</b> Each instrument's sound goes through Kit Gain, then the channel's effect slots, its fader, pan and mute/solo, and is summed into the Master; the Rev and Del knobs send part of the channel (after the fader) to the Reverb and Delay buses.<br>
  <b>On every channel strip:</b> four <b>effect slots</b> (choose None, EQ, Compressor, Saturation, Transient Shaper or Limiter; the gear next to a slot opens that effect's panel), <b>Pan</b>, <b>Rev</b> and <b>Del</b> send knobs, a <b>fader</b> with its dB value (double-click the value to type one), <b>M</b> and <b>S</b> buttons, an <b>Auto</b> button with a target peak, and a level meter with a peak readout.<br>
  <b>Effect panels</b> float above the UI, can be dragged by their header and several can be open at once. Close them with their X or <b>Options → Close all effect panels</b>. The EQ panel shows the response curve; the Limiter panel shows the input, the output and the gain reduction.<br>
  <b>Mute and Solo.</b> Solo is exclusive: soloing a channel un-solos all others. Mute and Solo of one channel exclude each other. (Unlike the Note Editor's M/S, these change what you really hear.)<br>
  <b>Kit Gain</b> is one gain on the input of all instrument channels, before their effects and faders (−24 to +24 dB).<br>
  <b>Master.</b> The master strip has its own effect slots and fader and a dB scale. By default the master carries an EQ and a Limiter; instrument channels start with empty effect slots.<br>
  <b>Auto gain.</b> The <b>Auto</b> button of a strip plays the song, measures the loudest peak of that strip for at least the time set in <b>Options → Auto Gain Time</b>, and then sets the fader once so that the peak lands on the strip's target value (the number next to the button). On the Kit Gain strip it sets Kit Gain from the loudest instrument channel. <b>Auto All</b> (transport row) runs Kit Gain first, then every instrument channel, then the Master; channels without signal are skipped. Both start playback if it is not running; click again to cancel.<br>
  <b>Mixer presets</b> (menu in the top row) load or save the whole mixer: faders, pan, sends, effects and Kit Gain. The <b>Default</b> entry is the mixer that the selected sound engine starts with.<br>
  <b>Song timeline.</b> In the transport row the Mixer shows a read-only overview of the song; click or drag on it to set where playback starts.
`);

howtoSet('howto-engine', `
  The Drum Engine tab is where the sound is shaped. The instrument tabs in the top row (Kick, Snare, Hi-Hat, Tom, Cymbal, Ride, Custom) switch between instrument families.<br>
  <b>Cards.</b> Every instrument (every articulation, e.g. Kick High Stack or Snare Rimshot) has a card with its <b>knobs</b> — the controls of the real circuit it models, such as Tone, Decay or Level, plus <b>Pitch</b>, which transposes the sound digitally and is not a circuit control — a <b>Reset</b> button and a volume. Some instruments also have a <b>Morph Pad</b>: drag it to blend the sound between four corner presets; it retriggers the sound while you hold the mouse button (a hollow dot means a knob was changed by hand since — drag again to resync). <b>Options → Retrigger</b> sets the repeat rate for pads and for the knob preview.<br>
  <b>Polyphony.</b> The row at the top sets how many hits of a group can sound at the same time before the oldest is cut off (Kick, Snare, all Toms together, Ride).<br>
  <b>Keyboard.</b> The keys at the bottom play the instruments with their current settings; keys are colored by instrument family. <b>Options → Preview sound on knob turn</b> plays the instrument while you turn one of its knobs. The meter on the right shows the peak level of the engine output.<br>
  <b>Engine.</b> The <b>Engine</b> selector (top right) switches the whole kit between <b>808</b> and <b>Lo-Fi</b>. The Lo-Fi engine is simpler: its cards only keep the name, Reset and volume, because it has no adjustable circuit parameters. Each engine starts with its own default mixer, so switching loads that engine's default mixer settings (unsaved mixer changes are lost; the plugin asks first).<br>
  <b>Custom.</b> The Custom tab holds the extra percussion: High Conga, Claves, Hand Clap, Maracas, Cowbell and PC-2. An instrument plays in a pattern once you add it as a track with <b>+ Add Track</b> in the Note Editor; all custom tracks use the Mixer's Custom channel.
`);

howtoSet('howto-cache', `
  Hi-hat, cymbal and ride sounds are expensive to compute, so the <b>808 engine</b> renders them in advance and keeps several recorded variants per sound setting; the variants are played in rotation so that repeated hits do not sound identical. All other instruments are computed live on every hit and need no cache. The Lo-Fi engine uses no cache.<br>
  <b>Where.</b> The variants are stored on disk, shared by all projects of the computer (on Windows in %APPDATA%\\DrumGroover\\Cache).<br>
  <b>First start.</b> The first time DrumGroover runs it renders its factory defaults; a dialog <i>Preparing the sound cache</i> shows the progress. On a fast computer this takes a few seconds, on slower ones longer. Later, when you release a knob of a cached instrument, the new setting is baked in the background and the status line shows the progress.<br>
  <b>Cache menu</b> (Drum Engine tab, 808 engine only):<br>
  <b>Variants per sound</b> — how many variants are baked per setting (1–20). More variants give more variance but need longer baking and more disk space. Type a new count and press <b>Apply</b>; an estimate is shown first, and the new count takes effect when all its samples are baked.<br>
  <b>Cache Size Limit</b> — the maximum disk space (all projects together). When it is exceeded the least recently used sets are deleted; the factory defaults are never deleted.<br>
  <b>Rebake at value change</b> — bakes a completely new set whenever a knob is released, even if that setting was baked before.<br>
  <b>Hi-Hat cache off / Cymbal / Ride cache off</b> — nothing is played from or baked into the cache for these instruments; every hit is synthesised live. This costs more processor load (the cymbal is the most expensive instrument) and every hit gets its own oscillator phase.<br>
  <b>Clear Cache</b> deletes all variants except the factory defaults; they are baked again as needed. <b>Reset Defaults</b> deletes and rebakes the factory defaults — only needed if you suspect they are damaged.
`);

howtoSet('howto-export', `
  <b>File ▾</b> holds <b>New</b> (empty project, with a confirmation if there are unsaved changes), <b>Save</b> and <b>Load</b> (the whole project as a .json file), <b>Export Song</b> (the whole arranged song as one .mid file) and <b>Export All Blocks</b> (one .mid file per block; the name you enter is used as a prefix). The Save button shows an asterisk while there are unsaved changes.<br>
  <b>Drag to your DAW.</b> Drag a <b>clip</b> from the Song Generator out of the plugin window onto your DAW's playlist or channel rack (in FL Studio: Playlist or Channel Rack) to insert the <b>whole song</b> as MIDI. To insert a <b>single block</b>, drag its chip from the block palette. Keep the mouse button held while crossing the plugin's border; dropping inside the plugin does nothing.<br>
  Exports contain every instrument, regardless of Mute/Solo.<br>
  <b>Project state.</b> Your DAW saves the whole plugin state — patterns, song, sound settings and mixer — inside its own project file automatically. <b>Save</b> / <b>Load</b> in the File menu are for moving a project between sessions or sharing it as a separate file.<br>
  <b>Exported MIDI.</b> A multi-track .mid file that uses the plugin's MIDI notes (Options → Show GM Notes shows them).
`);

howtoSet('howto-options', `
  Every tab has an <b>Options ⚙</b> menu with the settings that matter there:<br>
  <b>Everywhere:</b> <b>Show tooltips</b> turns the hover help on or off. <b>Accessible Zoom</b> enlarges the knob cluster under the cursor on hover (<b>Zoom Factor</b> sets how much). <b>UI Scale</b> (75%–200%) resizes the plugin window if your DAW allows it. <b>Developer tools</b> enables right-click → Inspect for debugging; leave it off unless you need it.<br>
  <b>Note Editor:</b> <b>Show GM Notes</b> (a MIDI note field on every row), <b>Show Info String</b> (the small strip above the grid), <b>Show confirmation dialogs</b> (ask before discarding hand edits, rerolling all blocks, clearing all blocks or the timeline).<br>
  <b>Drum Engine:</b> <b>Retrigger</b> (repeat rate of the Morph Pads and the knob preview while held), <b>Meter Peak Hold</b>, <b>Preview sound on knob turn</b> (plays the instrument while you turn one of its knobs).<br>
  <b>Mixer:</b> <b>Meter Peak Hold</b>, <b>Auto Gain Time</b>, <b>Close all effect panels</b>.<br>
  Tooltips show the name, what the control does and how to use it; hold the pointer still over any control.
`);

howtoSet('howto-hotkeys', `
  Shortcuts work on every tab unless noted, and nothing fires while you are typing in a text field or a dropdown.<br>
  <b style="display:block;margin-top:12px">Global</b>
  <b>Space</b> — play / stop (not on the Drum Engine tab, where the keyboard at the bottom is for testing sounds).<br>
  <b>F</b> — fit the view: the current block in the Note Editor, or the whole song (or the selected clips) in the Song Generator.<br>
  <b>Ctrl+Z</b> / <b>Ctrl+Shift+Z</b> (or <b>Ctrl+Y</b>) — undo / redo.<br>
  <b>Ctrl+A</b> / <b>Alt+A</b> — select all eligible notes or clips / clear the selection.<br>
  <b>Ctrl+I</b> — invert the selection (notes in the Note Editor, clips in the Song Generator).<br>
  <b>Del</b> / <b>Backspace</b> — delete the selected notes or clips.<br>
  <b style="display:block;margin-top:12px">Note Editor</b>
  <b>Tab</b> — show / hide the sidebar.<br>
  <b>Arrow Left/Right</b> — nudge the selected notes by one Snap step (<b>Shift</b> = 4×).<br>
  <b>Arrow Up/Down</b> — switch each selected note to the previous / next articulation of its group.<br>
  <b>Ctrl+C</b> / <b>Ctrl+X</b> / <b>Ctrl+V</b> — copy / cut / paste.<br>
  <b>Ctrl + mouse wheel</b> — zoom at the cursor; <b>middle-drag</b> pans.<br>
  Mouse gestures are described in <i>Note Editor — editing by hand</i>.<br>
  <b style="display:block;margin-top:12px">Song Generator</b>
  <b>Shift+click</b> a clip — select the range from the anchor clip; <b>Ctrl/Cmd+click</b> toggles it.<br>
  <b>Drag</b> on empty space — lasso-select; <b>Shift+drag</b> adds, <b>Ctrl/Cmd+drag</b> toggles.<br>
  <b>Shift+drag</b> a clip — duplicate instead of move.<br>
  <b>Ctrl + mouse wheel</b> — zoom at the cursor.<br>
  <b style="display:block;margin-top:12px">Mixer</b>
  <b>Ctrl+click</b> a fader or knob — reset it to its default.<br>
  <b>Double-click</b> the dB value under a fader — type an exact value; <b>Enter</b> commits, <b>Escape</b> cancels.<br>
  <b style="display:block;margin-top:12px">Drum Engine</b>
  <b>Ctrl+click</b> any slider or knob — reset it to its default.<br>
  <b style="display:block;margin-top:12px">Knobs and value fields (everywhere)</b>
  <b>Mouse wheel</b> over a knob — step it by one increment.<br>
  <b>Double-click</b> a knob's value — type an exact number; <b>Enter</b> commits, <b>Escape</b> cancels.<br>
  <b>Ctrl+click</b> a knob — reset it to its default.<br>
  <b>Shift+drag</b> one of the six Density knobs — Energy macro: all six scale proportionally.<br>
  <b style="display:block;margin-top:12px">Dialogs and dropdowns</b>
  Confirmation dialogs: <b>Enter</b> confirms, <b>Escape</b> cancels, <b>Arrow Left/Right</b> switches between the two buttons (when no text field is focused).<br>
  Dropdowns: with the control focused, <b>Enter</b>, <b>Space</b>, <b>F4</b> or <b>Alt+Arrow Up/Down</b> opens the list; <b>Escape</b> closes it.
`);

howtoSet('howto-troubleshooting', `
  <b>No sound.</b> (1) Is the <b>Mute</b> button (top right) lit? (2) In the Mixer, check the channel's fader, M/S buttons and Kit Gain, and the effects on the master. (3) In your DAW the plugin must be loaded as an instrument with its output routed to a mixer track. (4) Press the plugin's own <b>▶ Play</b>, or start the DAW transport and check that the Song Generator contains clips. (5) On the very first start wait until the <i>Preparing the sound cache</i> dialog is gone.<br>
  <b>Blank window.</b> The interface needs the Microsoft Edge WebView2 Runtime. It comes with current Windows 11; older systems may need to install it.<br>
  <b>Turning a knob changes nothing.</b> The group is probably locked by a hand edit — look for the 🔒 next to its title and click it, or use Edit ▾ → Discard.<br>
  <b>Muting a group in the Note Editor does not silence the song.</b> Its M/S buttons only affect the Note Editor's own playback and the sounds you click. Mute channels in the Mixer for the Song Generator and your DAW.<br>
  <b>Space does not play on the Drum Engine tab.</b> It is not a playback tab; use the keyboard at the bottom to test sounds.<br>
  <b>A hi-hat, ride or cymbal sounds different for a moment after changing a knob.</b> The new setting is being baked into the sound cache; the status line shows the progress.<br>
  <b>Where is the tempo / BPM?</b> It comes from the DAW; the plugin has no tempo control.<br>
  <b>Shortcuts do nothing.</b> They are disabled while a text field or dropdown has the focus — click on an empty area first.
`);

// =========================================
// Export: the whole How-To as one standalone HTML file with every section open
// =========================================

async function howtoBuildHtml() {
  let logo = '';
  try {
    const blob = await (await fetch('drumgrooverlogo.png')).blob();
    const base64 = await new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result).split(',')[1]);
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
    logo = `<img class="logo" alt="DrumGroover" src="data:image/png;base64,${base64}">`;
  } catch (e) { /* the page works without the logo */ }

  const credits = [...document.querySelectorAll('#howtoCredits > div')]
    .map(d => `<div${d.className ? ' class="' + d.className + '"' : ''}>${d.innerHTML}</div>`)
    .join('\n');

  const sections = [...document.querySelectorAll('.howto-section')]
    .map(d => `<h2>${d.querySelector('summary').innerHTML}</h2>\n<div class="body">${d.querySelector('.howto-body').innerHTML}</div>`)
    .join('\n');

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DrumGroover — How To</title>
<style>
  body { background:#0f172a; color:#e2e8f0; font:14px/1.6 Inter,'SF Pro Text',-apple-system,system-ui,sans-serif; margin:0; padding:32px 20px; }
  main { max-width:860px; margin:0 auto; }
  img.logo { display:block; height:56px; width:auto; margin:0 auto 28px; }
  .credits { text-align:center; font-size:12px; line-height:1.5; color:#94a3b8; margin:-14px 0 24px; }
  .credits a { color:#fdd04d; }
  .credits .howto-warn { color:#f59e0b; font-weight:600; margin-top:4px; }
  h1 { display:none; font-size:22px; margin:0 0 16px; }
  h2 { font-size:15px; letter-spacing:.06em; text-transform:uppercase; color:#fdd04d; border-bottom:1px solid #475569; padding:24px 0 8px; margin:0 0 10px; }
  .body { color:#cbd5e1; }
  .body b { color:#f8fafc; }
  @media print {
    body { background:#fff; color:#111; padding:0; }
    img.logo { display:none; }
    h1 { display:block; }
    h2 { color:#111; border-color:#999; break-after:avoid; }
    .body { color:#222; }
    .body b { color:#000; }
  }
</style>
</head>
<body>
<main>
${logo}
<div class="credits">
${credits}
</div>
<h1>DrumGroover — How To</h1>
${sections}
</main>
</body>
</html>
`;
}

async function howtoExport() {
  const html = await howtoBuildHtml();
  const filename = 'DrumGroover-HowTo.html';
  if (typeof hostBridge !== 'undefined' && hostBridge.isEmbedded) {
    hostBridge.send('exportHowTo', { html, suggestedFilename: filename });
    return;
  }
  const url = URL.createObjectURL(new Blob([html], { type: 'text/html' }));
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

document.getElementById('howtoExportBtn')?.addEventListener('click', () => { howtoExport().catch(console.error); });
