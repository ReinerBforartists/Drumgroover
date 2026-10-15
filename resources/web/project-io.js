// =========================================
// PROJECT SAVE / LOAD
// =========================================
// Two layers: serializeProject/deserializeProject for pure data (JSON-safe),
// and saveProjectToFile/loadProjectFromFile for file transport.
// In embedded plugin mode, file operations are delegated to C++ (native dialogs + disk I/O).
// In standalone browser mode, fall back to Blob and <input type=file>.
// This file handles user-initiated Save/Load, not DAW host session recall (GetState/SetState).
//
// block.data is the source of truth; per-group knobs are stored on each block.
const PROJECT_FORMAT_VERSION = 1;

function serializeProject() {
  return {
    formatVersion: PROJECT_FORMAT_VERSION,
    savedAt: new Date().toISOString(),
    global: {
      numerator: getParamNum('numerator'),
      denominator: getParamNum('denominator', 4) || 4,
      subdivision: getParamNum('subdivision'),
      selectedGenre,
      sessionSeed,
      activeBlockId,
      selectedSegmentIdx,
      mutedInstruments: [...mutedInstruments],
      soloedInstruments: [...soloedInstruments],
      songTypeLabel: document.getElementById('tlSongTypeLabel')?.textContent || '',
    },
    trackList: JSON.parse(JSON.stringify(trackList)),
    blocksList: JSON.parse(JSON.stringify(blocksList)),
    songSequence: JSON.parse(JSON.stringify(songEngine.songSequence)),
  };
}

// Replaces live state with `proj` and re-renders. Throws on invalid input.
function deserializeProject(proj) {
  if (!proj || typeof proj !== 'object') throw new Error('Not a project file (not an object).');
  if (!Array.isArray(proj.blocksList) || !Array.isArray(proj.trackList) || !Array.isArray(proj.songSequence)) {
    throw new Error('Not a project file (missing blocksList / trackList / songSequence).');
  }
  if ((proj.formatVersion || 0) > PROJECT_FORMAT_VERSION) {
    throw new Error(`This project was saved by a newer version of the app (format v${proj.formatVersion}) — please update DGG first.`);
  }

  stopPlayback();

  // Clear undo/redo history — snapshots would be invalid for the new state.
  undoHistory = []; 
  redoHistory = [];

  // Invalidate any pending async default block generation that could overwrite loaded state.
  projectGeneration++;

  trackList = JSON.parse(JSON.stringify(proj.trackList));
  blocksList = JSON.parse(JSON.stringify(proj.blocksList));
  songEngine.songSequence = JSON.parse(JSON.stringify(proj.songSequence));

  const g = proj.global || {};
  if (g.numerator != null) setParamValue('numerator', g.numerator);
  // Use HTML default (4) if denominator not present.
  if (g.denominator != null) setParamValue('denominator', g.denominator);
  if (g.subdivision != null) setParamValue('subdivision', g.subdivision);
  if (g.selectedGenre) {
    selectedGenre = g.selectedGenre;
    const sel = document.getElementById('genreSelect');
    if (sel) sel.value = g.selectedGenre;
  }
  if (g.sessionSeed != null) sessionSeed = g.sessionSeed;
  mutedInstruments = new Set(g.mutedInstruments || []);
  soloedInstruments = new Set(g.soloedInstruments || []);
  // Fall back to first block if saved activeBlockId was removed.
  activeBlockId = (g.activeBlockId && findBlock(g.activeBlockId)) ? g.activeBlockId : (blocksList[0] ? blocksList[0].id : null);
  selectedSegmentIdx = null; // Timeline indices may be invalid after load.
  const label = document.getElementById('tlSongTypeLabel');
  if (label) label.textContent = g.songTypeLabel || '';

  rebuildFullFromEditedBlocks();
  renderBlockTabs();
  renderNoteEditor();
  renderTimeline();
  renderStats(readParams(), readGrid());
  updateGroupLockUI();
  refreshUndoRedoButtons();
  syncKnobsToBlock(findBlock(activeBlockId));
}

// =========================================
// DIRTY TRACKING — powers confirmDiscardCurrentProject and the asterisk on Save button.
// =========================================
// Dirty means the current project differs from the last saved/loaded state.
// savedAt is stripped from the comparison to avoid false dirtiness due to timestamp changes.
let savedStateJson = null;

function projectSnapshotForCompare() {
  const p = serializeProject();
  delete p.savedAt;
  return JSON.stringify(p);
}

function isProjectDirty() {
  if (savedStateJson === null) return false;
  return projectSnapshotForCompare() !== savedStateJson;
}

// Updates Save button label with trailing " *" when dirty. Caches original label.
function updateDirtyIndicator(dirty) {
  const btn = document.getElementById('saveProjectBtn');
  if (!btn) return;
  if (btn.dataset.baseLabel === undefined) btn.dataset.baseLabel = btn.textContent;
  btn.textContent = dirty ? btn.dataset.baseLabel + ' *' : btn.dataset.baseLabel;
}

// Call after a successful save or load to mark the current state as clean. The startup baseline is set by app.js
// once the async default generation has settled.
function markProjectClean() {
  savedStateJson = projectSnapshotForCompare();
  updateDirtyIndicator(false);
}

// =========================================
// FILE TRANSPORT — bridge-aware (embedded) vs Blob/<input> (standalone)
// =========================================
function defaultProjectFilename() {
  return `dgg-project-${timestampSlug()}.json`;
}

// Embedded: delegate to C++ (native Save dialog + disk write).
// Standalone: Blob download. Status text updated accordingly.
function saveProjectToFile() {
  const projectData = serializeProject();

  if (hostBridge.isEmbedded) {
    hostBridge.send('saveProject', { json: JSON.stringify(projectData, null, 2), suggestedFilename: defaultProjectFilename() });
    return;
  }

  const json = JSON.stringify(projectData, null, 2);
  const blob = new Blob([json], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = defaultProjectFilename();
  a.click();
  URL.revokeObjectURL(url);
  const status = document.getElementById('statusText');
  if (status) status.textContent = 'Project saved';
  markProjectClean();
}

// Generic confirmation dialog for discarding unsaved changes; wording parameterizable.
async function confirmDiscardCurrentProject(verb = 'Load', verbing = 'Loading a project') {
  if (!isProjectDirty()) return true;
  return showModal({
    title: `${verb} Project`,
    message: `${verbing} replaces everything currently open.<br>Unsaved changes will be lost — continue?`,
    confirmLabel: verb,
    cancelLabel: 'Cancel',
    danger: true,
  });
}

// File > New: discard current project and rebuild default scaffold via generateSequence(true).
async function newProject() {
  const ok = await confirmDiscardCurrentProject('New', 'Starting a new project');
  if (!ok) return;

  stopPlayback();

  // Invalidate any pending async generation.
  projectGeneration++;
  undoHistory = [];
  redoHistory = [];

  blocksList = [];
  songEngine.songSequence = [];
  activeBlockId = null;
  selectedSegmentIdx = null;
  tlSetSelection([]);

  // Reset toolbar controls to their HTML defaults.
  setParamValue('numerator', 4);
  setParamValue('denominator', 4);
  setParamValue('subdivision', 4);
  selectedGenre = 'straight';
  const genreSel = document.getElementById('genreSelect');
  if (genreSel) genreSel.value = 'straight';
  sessionSeed = Math.floor(Math.random() * 999999);
  mutedInstruments = new Set();
  soloedInstruments = new Set();
  const label = document.getElementById('tlSongTypeLabel');
  if (label) label.textContent = '';

  // trackList is static; left untouched.
  await generateSequence(true);
  updateGroupLockUI();
  refreshUndoRedoButtons();
  syncKnobsToBlock(findBlock(activeBlockId));

  markProjectClean();
  const status = document.getElementById('statusText');
  if (status) status.textContent = 'New project started';
}

document.getElementById('newProjectBtn')?.addEventListener('click', newProject);

// Shared apply logic for loaded project text (embedded reply or standalone file read).
async function applyLoadedProjectText(text) {
  let proj;
  try {
    proj = JSON.parse(text);
  } catch (err) {
    await showModal({ title: 'Load Project', message: 'That file isn\u2019t valid JSON — is it really a DGG project file?', confirmLabel: 'OK' });
    return;
  }

  const ok = await confirmDiscardCurrentProject();
  if (!ok) return;

  try {
    deserializeProject(proj);
    markProjectClean();
    rebuildFullFromEditedBlocks();
    const status = document.getElementById('statusText');
    if (status) status.textContent = 'Project loaded';
  } catch (err) {
    await showModal({ title: 'Load Project', message: `Could not load this project: ${err.message}`, confirmLabel: 'OK' });
  }
}

// Standalone-only: read file from browser and apply.
async function loadProjectFromFile(file) {
  const text = await file.text().catch(() => null);
  if (text === null) {
    await showModal({ title: 'Load Project', message: 'Could not read that file.', confirmLabel: 'OK' });
    return;
  }
  await applyLoadedProjectText(text);
}

// =========================================
// SILENT STATE SYNC TO HOST — survives editor WebView teardown
// =========================================
// Periodically push current project state to C++ (memory cache) so that
// if the WebView is destroyed and re-created, the state can be restored.
// This is separate from user-initiated Save/Load and DAW host session recall.
let lastSyncedCompareJson = null;

function periodicSyncTick() {
  const compareJson = projectSnapshotForCompare();

  if (hostBridge.isEmbedded && compareJson !== lastSyncedCompareJson) {
    lastSyncedCompareJson = compareJson;
    hostBridge.send('cacheState', { json: JSON.stringify(serializeProject()) });
  }

  updateDirtyIndicator(savedStateJson !== null && compareJson !== savedStateJson);
}

// Periodic safety net; deduped by lastSyncedCompareJson.
setInterval(periodicSyncTick, 1000);

// Additional triggers when the WebView becomes hidden or before unload.
document.addEventListener('visibilitychange', () => {
  if (document.hidden) syncStateToHost();
});
window.addEventListener('beforeunload', syncStateToHost);

// On-demand sync, used by visibilitychange and beforeunload.
function syncStateToHost() {
  if (!hostBridge.isEmbedded) return;
  const compareJson = projectSnapshotForCompare();
  if (compareJson === lastSyncedCompareJson) return;
  lastSyncedCompareJson = compareJson;
  hostBridge.send('cacheState', { json: JSON.stringify(serializeProject()) });
}

// Embedded-only: restore cached state when editor loads.
hostBridge.on('restoreState', (payload) => {
  if (!payload || typeof payload.text !== 'string') return;
  let proj;
  try {
    proj = JSON.parse(payload.text);
  } catch (err) {
    console.error('restoreState: cached state was not valid JSON', err);
    return;
  }

  try {
    deserializeProject(proj);
    markProjectClean();
    lastSyncedCompareJson = projectSnapshotForCompare();
    rebuildFullFromEditedBlocks();
  } catch (err) {
    console.error('restoreState: could not apply cached state', err);
  }
});

// Event wiring for Save/Load buttons.
document.getElementById('saveProjectBtn')?.addEventListener('click', saveProjectToFile);

document.getElementById('loadProjectBtn')?.addEventListener('click', () => {
  if (hostBridge.isEmbedded) {
    hostBridge.send('requestLoadProject');
    return;
  }
  document.getElementById('loadProjectInput')?.click();
});

document.getElementById('loadProjectInput')?.addEventListener('change', (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (file) loadProjectFromFile(file);
});

// Embedded replies from C++.
hostBridge.on('projectLoaded', (payload) => {
  if (payload && typeof payload.text === 'string') applyLoadedProjectText(payload.text);
});
hostBridge.on('projectSaved', () => {
  const status = document.getElementById('statusText');
  if (status) status.textContent = 'Project saved';
  markProjectClean();
});
