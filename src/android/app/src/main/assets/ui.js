let navStack = ['gamesPage'];
let gameListGen = 0;
let libraryDirty = true;
let selectedGameIndex = -1;
let settingsSelection = -1;
const settingsSelectionByPage = {};
let themeReady = false;

function applyTheme() {
  let dark = false;
  try {
    dark = !!NxEmu.isDarkTheme();
  } catch (e) {
    dark = window.matchMedia('(prefers-color-scheme: dark)').matches;
  }
  document.documentElement.classList.toggle('dark', dark);
  document.documentElement.classList.toggle('light', !dark);
  if (!themeReady) {
    themeReady = true;
    requestAnimationFrame(function () {
      requestAnimationFrame(function () {
        document.documentElement.classList.add('theme-ready');
      });
    });
  }
}

function onSettingChanged(setting) {
  if (setting === 'nxui:GameDirectories') {
    libraryDirty = true;
    const top = navStack[navStack.length - 1];
    if (top === 'gameFolders' || top === 'settingsPage') renderCurrentSettings();
    if (top === 'gamesPage') refreshGameList();
  }
  if (setting === 'nxui:ThemeMode') {
    applyTheme();
    const top = navStack[navStack.length - 1];
    if (top === 'themeSettings' || top === 'settingsPage') renderCurrentSettings();
  }
}

function handleAndroidBack() {
  if (navStack.length > 1) {
    goBack();
    return true;
  }
  return false;
}

function escapeHtml(text) {
  if (!text) return '';
  return String(text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;');
}

function iconMimeFromBase64(b64) {
  if (!b64 || b64.length < 4) return 'image/jpeg';
  try {
    const head = atob(b64.slice(0, 8));
    const c0 = head.charCodeAt(0);
    const c1 = head.charCodeAt(1);
    if (c0 === 0xff && c1 === 0xd8) return 'image/jpeg';
    if (head.startsWith('GIF')) return 'image/gif';
    if (head.charCodeAt(0) === 0x89 && head.slice(1, 4) === 'PNG') return 'image/png';
  } catch (e) {}
  return 'image/jpeg';
}

function getConfiguredFolders() {
  try {
    const dirs = JSON.parse(NxEmu.getSettingString('nxui:GameDirectories') || '[]');
    return Array.isArray(dirs) ? dirs : [];
  } catch (e) {
    return [];
  }
}

function setLibraryStatus(text) {
  const status = document.getElementById('libraryStatus');
  if (status) status.textContent = text || '';
}

function renderEmptyGames(kind) {
  const container = document.getElementById('gameList');
  syncGameSelection(-1);
  container.className = '';
  if (kind === 'no-folders') {
    container.innerHTML =
      '<div class="empty-state">' +
      '<p class="empty-title">Add a game folder</p>' +
      '<p class="empty-copy">NxEmu lists games from folders you choose. Your files stay where they are.</p>' +
      '<button class="primary-btn" onclick="NxEmu.addGameDirectory()">Add game folder</button>' +
      '</div>';
    return;
  }
  if (kind === 'no-roms') {
    container.innerHTML =
      '<div class="empty-state">' +
      '<p class="empty-title">No games found</p>' +
      '<p class="empty-copy">No .nro, .dxci, or .dnsp files were found in your folders.</p>' +
      '<button class="primary-btn" onclick="navigate(\'gameFolders\')">Manage folders</button>' +
      '</div>';
    return;
  }
  container.innerHTML =
    '<div class="empty-state">' +
    '<p class="empty-title">Couldn\'t read these files</p>' +
    '<p class="empty-copy">Files were found, but none could be opened as games. Check that they are valid dumps.</p>' +
    '<button class="link-btn" onclick="refreshGameList()">Try again</button>' +
    '</div>';
}

function refreshGameList() {
  libraryDirty = false;
  const gen = ++gameListGen;
  const container = document.getElementById('gameList');
  if (!container) return;

  syncGameSelection(-1);
  const folders = getConfiguredFolders();
  if (!folders.length) {
    setLibraryStatus('');
    renderEmptyGames('no-folders');
    return;
  }

  container.className = '';
  container.innerHTML = '<div class="empty-hint">Scanning folders...</div>';
  setLibraryStatus('Scanning...');
  NxEmu.requestGameLibraryScan(gen);
}

function onGameLibraryPaths(gen, jsonStr) {
  if (gen !== gameListGen) return;
  const container = document.getElementById('gameList');
  if (!container) return;

  let paths = [];
  try {
    paths = JSON.parse(jsonStr);
  } catch (e) {
    paths = [];
  }
  if (!Array.isArray(paths)) paths = [];

  if (!paths.length) {
    setLibraryStatus('');
    renderEmptyGames('no-roms');
    return;
  }

  container.className = 'game-list';
  container.innerHTML = '';
  const cards = [];
  for (let i = 0; i < paths.length; i++) {
    const path = paths[i];
    const card = document.createElement('div');
    card.className = 'game-card is-loading';
    card.innerHTML =
      '<div class="game-icon-ph"></div>' +
      '<div class="game-title">' + escapeHtml(shortPath(path)) + '</div>';
    container.appendChild(card);
    cards.push({ path: path, el: card });
  }
  setLibraryStatus('Loading 0 of ' + paths.length);
  loadGameMetadata(gen, cards, 0, 0, 0);
}

function fillGameCard(card, meta, path) {
  const title = meta.title && meta.title.length ? meta.title : shortPath(path);
  const iconSrc = meta.icon && meta.icon.length
    ? ('data:' + iconMimeFromBase64(meta.icon) + ';base64,' + meta.icon)
    : 'file:///android_asset/nxlogo-32x32.png';
  card.className = 'game-card';
  card.innerHTML =
    '<img class="game-icon" src="' + iconSrc + '" alt="">' +
    '<div class="game-title">' + escapeHtml(title) + '</div>';
  card.dataset.title = title.toLowerCase();
  card.onclick = function () {
    try {
      NxEmu.launchGame(path);
    } catch (e) {}
  };
}

async function loadGameMetadata(gen, cards, index, loaded, failed) {
  if (gen !== gameListGen) return;
  if (index >= cards.length) {
    finishGameList(gen, cards, loaded, failed);
    return;
  }

  setLibraryStatus('Loading ' + (index + 1) + ' of ' + cards.length);
  const item = cards[index];
  let meta = { title: '', icon: '', error: 'failed', path: item.path };
  try {
    meta = JSON.parse(NxEmu.queryRomMetadata(item.path));
  } catch (e) {
    meta.error = 'parse';
  }

  if (meta && !meta.error) {
    fillGameCard(item.el, meta, item.path);
    loaded += 1;
  } else {
    item.el.className = 'game-card is-failed';
    failed += 1;
  }

  await new Promise(function (resolve) { setTimeout(resolve, 0); });
  loadGameMetadata(gen, cards, index + 1, loaded, failed);
}

function finishGameList(gen, cards, loaded, failed) {
  if (gen !== gameListGen) return;
  const container = document.getElementById('gameList');
  if (!container) return;

  if (!loaded) {
    setLibraryStatus('');
    renderEmptyGames('unreadable');
    return;
  }

  const visible = Array.prototype.slice.call(container.querySelectorAll('.game-card:not(.is-failed)'));
  visible.sort(function (a, b) {
    return (a.dataset.title || '').localeCompare(b.dataset.title || '');
  });
  visible.forEach(function (el) { container.appendChild(el); });
  setLibraryStatus('');
  syncGameSelection(0);
}

function selectableGameCards() {
  const container = document.getElementById('gameList');
  if (!container) return [];
  return Array.prototype.filter.call(container.children, function (el) {
    return el.classList.contains('game-card') &&
      !el.classList.contains('is-failed') &&
      !el.classList.contains('is-loading');
  });
}

function controllerAttached() {
  try {
    const names = JSON.parse(NxEmu.connectedControllerNames());
    return Array.isArray(names) && names.length > 0;
  } catch (e) {
    return false;
  }
}

function updatePlayHint() {
  const hint = document.getElementById('playHint');
  if (hint) hint.hidden = !controllerAttached() || selectedGameIndex < 0;
}

function applyControllerBrowser() {
  const attached = controllerAttached();
  const settingsHint = document.getElementById('settingsHint');
  const touchHint = document.getElementById('touchHint');
  if (settingsHint) settingsHint.hidden = !attached;
  if (touchHint) touchHint.hidden = attached;
  const backHint = document.getElementById('backHint');
  if (backHint) backHint.hidden = !attached;
  if (!attached) {
    syncGameSelection(-1);
    syncSettingsSelection(-1);
    return;
  }
  if (currentSettingsId()) {
    const remembered = settingsSelectionByPage[currentSettingsId()];
    const index = settingsSelection >= 0 ? settingsSelection : (remembered == null ? 0 : remembered);
    syncSettingsSelection(index);
  }
  if (selectedGameIndex < 0 && selectableGameCards().length) {
    syncGameSelection(0);
  } else {
    updatePlayHint();
  }
}

function syncGameSelection(index) {
  const cards = selectableGameCards();
  if (!controllerAttached() || !cards.length || index < 0) {
    selectedGameIndex = -1;
  } else {
    selectedGameIndex = Math.min(index, cards.length - 1);
    cards[selectedGameIndex].scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
  for (let i = 0; i < cards.length; i++) {
    cards[i].classList.toggle('is-selected', i === selectedGameIndex);
  }
  updatePlayHint();
}

function gridColumns(cards) {
  const top = cards[0].getBoundingClientRect().top;
  let cols = 0;
  for (let i = 0; i < cards.length; i++) {
    if (Math.abs(cards[i].getBoundingClientRect().top - top) > 4) break;
    cols++;
  }
  return cols || 1;
}

function moveGameSelection(direction) {
  if (!controllerAttached()) return;
  if (navStack[navStack.length - 1] !== 'gamesPage') {
    moveSettingsSelection(direction);
    return;
  }
  const cards = selectableGameCards();
  if (!cards.length) return;
  if (selectedGameIndex < 0) {
    syncGameSelection(0);
    return;
  }
  const cols = gridColumns(cards);
  let next = selectedGameIndex;
  if (direction === 'left') next -= 1;
  else if (direction === 'right') next += 1;
  else if (direction === 'up') next -= cols;
  else if (direction === 'down') next += cols;
  if (next < 0) next = 0;
  if (next >= cards.length) next = cards.length - 1;
  syncGameSelection(next);
}

function launchSelectedGame() {
  if (!controllerAttached()) return;
  if (navStack[navStack.length - 1] !== 'gamesPage') {
    activateSettingsSelection();
    return;
  }
  const card = selectableGameCards()[selectedGameIndex];
  if (card && typeof card.onclick === 'function') card.onclick();
}

function shortPath(uri) {
  try {
    const u = decodeURIComponent(uri);
    const last = u.lastIndexOf('/');
    return last >= 0 ? u.slice(last + 1) : u;
  } catch (e) {
    return uri;
  }
}

function openSettingsFromPad() {
  if (navStack[navStack.length - 1] !== 'gamesPage') return;
  if (typeof NxEmu.openSettings === 'function') NxEmu.openSettings();
}

function goBackFromPad() {
  if (!controllerAttached()) return;
  if (navStack[navStack.length - 1] === 'gamesPage') return;
  goBack();
}

function currentSettingsId() {
  const id = navStack[navStack.length - 1];
  if (!id || !settingPage(id)) return null;
  return id;
}

function currentSettingsPage() {
  if (!currentSettingsId()) return null;
  const shell = document.getElementById('settingsShell');
  if (!shell || shell.style.display === 'none') return null;
  return shell;
}

function settingsRows(page) {
  if (!page) return [];
  return Array.prototype.filter.call(
    page.querySelectorAll('.settings-item, .folder-item, .primary-btn, .fab'),
    function (el) { return el.getClientRects().length > 0; }
  );
}

function clearSettingsSelection() {
  document.querySelectorAll('.settings-item.is-selected, .folder-item.is-selected, .primary-btn.is-selected, .fab.is-selected').forEach(function (el) {
    el.classList.remove('is-selected');
  });
}

function syncSettingsSelection(index) {
  const page = currentSettingsPage();
  const rows = settingsRows(page);
  clearSettingsSelection();
  if (!controllerAttached() || !page || !rows.length || index < 0) {
    settingsSelection = -1;
    return;
  }
  settingsSelection = Math.min(index, rows.length - 1);
  const settingsId = currentSettingsId();
  if (settingsId) settingsSelectionByPage[settingsId] = settingsSelection;
  rows[settingsSelection].classList.add('is-selected');
  rows[settingsSelection].scrollIntoView({ block: 'nearest', inline: 'nearest' });
}

function refreshSettingsSelection() {
  if (!currentSettingsPage()) return;
  syncSettingsSelection(settingsSelection < 0 ? 0 : settingsSelection);
}

function moveSettingsSelection(direction) {
  if (!controllerAttached() || !currentSettingsPage()) return;
  const rows = settingsRows(currentSettingsPage());
  if (!rows.length) return;
  if (direction === 'left' || direction === 'right') {
    nudgeSelectedSlider(direction);
    return;
  }
  if (settingsSelection < 0) {
    syncSettingsSelection(0);
    return;
  }
  let next = settingsSelection + (direction === 'up' ? -1 : 1);
  if (next < 0) next = 0;
  if (next >= rows.length) next = rows.length - 1;
  syncSettingsSelection(next);
}

function nudgeSelectedSlider(direction) {
  const row = settingsRows(currentSettingsPage())[settingsSelection];
  if (!row) return;
  const input = row.querySelector('input[type="range"]');
  if (!input) return;
  const min = parseFloat(input.min);
  const max = parseFloat(input.max);
  const stepAttr = parseFloat(input.step);
  const span = max - min;
  const step = stepAttr >= 1 ? Math.max(stepAttr, 1) : (span > 0 ? span / 20 : 0.05);
  let value = parseFloat(input.value) + (direction === 'right' ? step : -step);
  if (stepAttr >= 1) value = Math.round(value / stepAttr) * stepAttr;
  value = Math.min(max, Math.max(min, value));
  input.value = String(value);
  input.dispatchEvent(new Event('input', { bubbles: true }));
}

function activateSettingsSelection() {
  if (!controllerAttached() || !currentSettingsPage()) return;
  const row = settingsRows(currentSettingsPage())[settingsSelection];
  if (!row || row.querySelector('input[type="range"]')) return;
  if (row.classList.contains('folder-item')) {
    const remove = row.querySelector('.folder-remove');
    if (remove) remove.click();
    return;
  }
  row.click();
}

function navigate(page) {
  navStack.push(page);
  showPage(page);
}

function goBack() {
  if (navStack.length > 1) {
    navStack.pop();
    showPage(navStack[navStack.length - 1]);
    return;
  }
  if (typeof NxEmu.closeSettings === 'function') {
    NxEmu.closeSettings();
  }
}

function formatGameFolder(uri) {
  const decoded = decodeURIComponent(uri);
  const prefix = 'content://com.android.externalstorage.documents/tree/';

  if (!decoded.startsWith(prefix)) return decoded;

  const path = decoded.substring(prefix.length);
  const colonIndex = path.indexOf(':');
  const volume = path.substring(0, colonIndex);
  const folder = path.substring(colonIndex + 1);

  if (volume === 'primary') {
    return 'Internal storage / ' + folder;
  }
  return volume + ' / ' + folder;
}

function renderControllersSummary() {
  applyControllerBrowser();
  if (navStack[navStack.length - 1] === 'settingsPage') renderCurrentSettings();
}

function getThemeMode() {
  try {
    const mode = NxEmu.getSettingInt('nxui:ThemeMode');
    if (mode === 1 || mode === 2) return mode;
  } catch (e) {}
  return 0;
}

function themeModeLabel(mode) {
  if (mode === 1) return 'Light';
  if (mode === 2) return 'Dark';
  return 'Follow system';
}

function folderCountLabel() {
  const n = getConfiguredFolders().length;
  if (n === 0) return 'None added';
  if (n === 1) return '1 folder';
  return n + ' folders';
}

function overlayShowLabel() {
  let shown = true;
  try {
    shown = NxEmu.getSettingBool('nxui:ShowInputOverlay');
  } catch (e) {}
  return 'Show overlay: ' + (shown ? 'Yes' : 'No');
}

function connectedCountLabel() {
  var count = 0;
  for (var player = 0; player < 8; player++) {
    if (NxEmu.isControllerConnected(player)) count++;
  }
  return count === 0 ? 'Connected: none' : 'Connected: ' + count;
}

function readSettingBool(key) {
  try {
    return !!NxEmu.getSettingBool(key);
  } catch (e) {
    return false;
  }
}

function readSettingInt(key) {
  try {
    return NxEmu.getSettingInt(key);
  } catch (e) {
    return 0;
  }
}

const settingPages = [
  {
    id: 'settingsPage',
    title: 'Settings',
    rows: [
      { label: 'Game folders', type: 'page', page: 'gameFolders', detail: folderCountLabel },
      { label: 'On screen controller', type: 'page', page: 'overlaySettings', detail: overlayShowLabel },
      { label: 'Controllers', type: 'page', page: 'controllerSettings', detail: connectedCountLabel },
      { label: 'Theme', type: 'page', page: 'themeSettings', detail: function () { return themeModeLabel(getThemeMode()); } },
    ],
  },
  {
    id: 'overlaySettings',
    title: 'On screen controller',
    rows: [
      { label: 'Show overlay', type: 'bool', key: 'nxui:ShowInputOverlay' },
      { label: 'Scale', type: 'int', key: 'nxui:OverlayScale', min: 0, max: 150, suffix: '%' },
      { label: 'Opacity', type: 'int', key: 'nxui:OverlayOpacity', min: 0, max: 100, suffix: '%' },
      { label: 'Edit overlay layout', detail: 'Drag buttons to reposition', type: 'action', run: function () { NxEmu.editOverlayLayout(); } },
    ],
  },
  {
    id: 'themeSettings',
    title: 'Theme',
    rows: [
      { label: 'Follow system', type: 'choice', key: 'nxui:ThemeMode', value: 0, picked: function () { return getThemeMode() === 0; }, run: applyTheme },
      { label: 'Light', type: 'choice', key: 'nxui:ThemeMode', value: 1, picked: function () { return getThemeMode() === 1; }, run: applyTheme },
      { label: 'Dark', type: 'choice', key: 'nxui:ThemeMode', value: 2, picked: function () { return getThemeMode() === 2; }, run: applyTheme },
    ],
  },
  { id: 'gameFolders', title: 'Game folders', render: renderGameFolders },
  { id: 'controllerSettings', title: 'Controllers', render: renderControllerSettings },
  { id: 'controllerPlayer', title: function () { return 'Controller ' + (controllerPlayerIndex + 1); }, render: renderControllerPlayer },
  { id: 'controllerChoices', title: function () { return controllerChoiceState.title; }, render: renderControllerChoices },
];

function settingPage(id) {
  for (var i = 0; i < settingPages.length; i++) {
    if (settingPages[i].id === id) return settingPages[i];
  }
  return null;
}

function rowDetail(row) {
  if (typeof row.detail === 'function') return row.detail();
  return row.detail || '';
}

function ensureSettingsList() {
  const host = document.getElementById('settingsBody');
  if (!host) return null;
  host.innerHTML = '';
  const list = document.createElement('div');
  list.className = 'settings-list';
  host.appendChild(list);
  return list;
}

function renderSettingRows(spec) {
  const list = ensureSettingsList();
  if (!list) return;
  spec.rows.forEach(function (row) {
    if (row.type === 'bool') appendBoolRow(list, row);
    else if (row.type === 'int') appendIntRow(list, row);
    else if (row.type === 'choice') appendChoiceRow(list, row);
    else appendLinkRow(list, row);
  });
}

function appendLinkRow(list, row) {
  const item = document.createElement('div');
  item.className = 'settings-item';
  const detail = rowDetail(row);
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(row.label) + '</div>' +
    (detail ? '<div class="setting-sub">' + escapeHtml(detail) + '</div>' : '') +
    '</div>' + controllerChevron();
  item.addEventListener('click', function () {
    if (row.page) navigate(row.page);
    else if (row.run) row.run();
  });
  list.appendChild(item);
}

function appendBoolRow(list, row) {
  const on = readSettingBool(row.key);
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info"><div class="setting-label">' + escapeHtml(row.label) + '</div></div>' +
    '<button class="settings-toggle' + (on ? ' on' : '') + '" type="button" aria-label="' + escapeHtml(row.label) + '"></button>';
  item.addEventListener('click', function () {
    NxEmu.setSettingBool(row.key, !readSettingBool(row.key));
    NxEmu.saveSettings();
    if (row.run) row.run();
    renderCurrentSettings();
  });
  list.appendChild(item);
}

function appendIntRow(list, row) {
  const value = readSettingInt(row.key);
  const suffix = row.suffix || '';
  const step = row.step == null ? 1 : row.step;
  const item = document.createElement('div');
  item.className = 'settings-item setting-item-static';
  item.innerHTML =
    '<div class="setting-info" style="flex:1">' +
    '<div class="setting-label">' + escapeHtml(row.label) + ' <span class="slider-value">' + value + suffix + '</span></div>' +
    '<input class="settings-range" type="range" min="' + row.min + '" max="' + row.max + '" step="' + step + '" value="' + value + '">' +
    '</div>';
  const input = item.querySelector('input');
  const label = item.querySelector('.slider-value');
  input.addEventListener('input', function () {
    const next = parseInt(input.value, 10);
    if (label) label.textContent = next + suffix;
    NxEmu.setSettingInt(row.key, next);
    NxEmu.saveSettings();
    if (row.run) row.run();
  });
  list.appendChild(item);
}

function appendChoiceRow(list, row) {
  const on = row.picked ? !!row.picked() : readSettingInt(row.key) === row.value;
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info"><div class="setting-label">' + escapeHtml(row.label) + '</div></div>' +
    '<svg class="setting-check' + (on ? ' on' : '') + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M20 6L9 17l-5-5"/></svg>';
  item.addEventListener('click', function () {
    if (row.key) {
      NxEmu.setSettingInt(row.key, row.value);
      NxEmu.saveSettings();
    }
    if (row.run) row.run();
    renderCurrentSettings();
  });
  list.appendChild(item);
}

function renderCurrentSettings() {
  const spec = settingPage(navStack[navStack.length - 1]);
  if (!spec) return;
  const title = document.getElementById('settingsTitle');
  if (title) title.textContent = typeof spec.title === 'function' ? spec.title() : spec.title;
  if (spec.rows) renderSettingRows(spec);
  else if (spec.render) spec.render();
  refreshSettingsSelection();
}

function renderGameFolders() {
  const host = document.getElementById('settingsBody');
  if (!host) return;
  host.innerHTML = '';
  const dirs = getConfiguredFolders();
  const list = document.createElement('div');
  list.className = 'settings-list';
  host.appendChild(list);
  if (!dirs.length) {
    const empty = document.createElement('div');
    empty.className = 'empty-state';
    empty.innerHTML =
      '<p class="empty-title">No folders yet</p>' +
      '<p class="empty-copy">Choose a folder that contains .nro, .dxci, or .dnsp files.</p>' +
      '<button class="primary-btn" type="button">Add game folder</button>';
    empty.querySelector('button').addEventListener('click', function () {
      NxEmu.addGameDirectory();
    });
    host.appendChild(empty);
  }
  dirs.forEach(function (uri) {
    const name = formatGameFolder(uri);
    const item = document.createElement('div');
    item.className = 'folder-item';
    item.innerHTML =
      '<div class="setting-info">' +
      '<div class="setting-label">' + escapeHtml(name) + '</div>' +
      '</div>' +
      '<button class="folder-remove" type="button" aria-label="Remove folder">' +
      '<svg viewBox="0 0 1024 1024"><path d="M160 256H96a32 32 0 0 1 0-64h256V95.936a32 32 0 0 1 32-32h256a32 32 0 0 1 32 32V192h256a32 32 0 1 1 0 64h-64v672a32 32 0 0 1-32 32H192a32 32 0 0 1-32-32zm448-64v-64H416v64zM224 896h576V256H224zm192-128a32 32 0 0 1-32-32V416a32 32 0 0 1 64 0v320a32 32 0 0 1-32 32m192 0a32 32 0 0 1-32-32V416a32 32 0 0 1 64 0v320a32 32 0 0 1-32 32"/></svg>' +
      '</button>';
    item.querySelector('.folder-remove').addEventListener('click', function () {
      removeGameFolder(uri);
    });
    list.appendChild(item);
  });
  if (dirs.length) {
    const fab = document.createElement('button');
    fab.className = 'fab';
    fab.type = 'button';
    fab.setAttribute('aria-label', 'Add game folder');
    fab.innerHTML = '<svg viewBox="0 0 24 24"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>';
    fab.addEventListener('click', function () { NxEmu.addGameDirectory(); });
    host.appendChild(fab);
  }
  refreshSettingsSelection();
}

function removeGameFolder(uri) {
  if (!confirm('Remove this folder from the library? The files will not be deleted.')) {
    return;
  }
  const dirs = getConfiguredFolders().filter(function (d) { return d !== uri; });
  NxEmu.setSettingString('nxui:GameDirectories', JSON.stringify(dirs));
  NxEmu.saveSettings();
}

var controllerPlayerIndex = 0;
var controllerFilterIndex = 0;
var controllerChoiceState = { title: 'Controller', items: [], onPick: function () {}, emptyText: '' };
var BTN = { A: 0, B: 1, X: 2, Y: 3, LStick: 4, RStick: 5, L: 6, R: 7, ZL: 8, ZR: 9, Plus: 10, Minus: 11, DLeft: 12, DUp: 13, DRight: 14, DDown: 15, SLLeft: 16, SRLeft: 17, Home: 18, Capture: 19, SLRight: 20, SRRight: 21 };
var STICK = { L: 0, R: 1 };

function controllerStyleName(style) {
  if (style === 3) return 'Pro Controller';
  if (style === 4) return 'Handheld';
  if (style === 5) return 'Dual Joycons';
  if (style === 6) return 'Left Joycon';
  if (style === 7) return 'Right Joycon';
  if (style === 8) return 'GameCube Controller';
  if (style === 0) return 'None';
  return 'Unknown';
}

function controllerChevron() {
  return '<svg class="setting-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 18l6-6-6-6"/></svg>';
}

function parseJsonArray(text) {
  try {
    const parsed = JSON.parse(text);
    return Array.isArray(parsed) ? parsed : [];
  } catch (e) {
    return [];
  }
}

function openControllerPlayer(index) {
  controllerPlayerIndex = index;
  controllerFilterIndex = 0;
  navigate('controllerPlayer');
}

function renderControllerPages() {
  const id = navStack[navStack.length - 1];
  if (id === 'controllerSettings' || id === 'controllerPlayer') renderCurrentSettings();
}

function renderControllerSettings() {
  const list = ensureSettingsList();
  if (!list) return;
  list.innerHTML = '';
  for (var player = 0; player < 8; player++) {
    const connected = NxEmu.isControllerConnected(player);
    const styleName = controllerStyleName(NxEmu.getControllerStyle(player));
    const item = document.createElement('div');
    item.className = 'settings-item';
    item.innerHTML =
      '<div class="setting-info">' +
      '<div class="setting-label">Controller ' + (player + 1) + '</div>' +
      '<div class="setting-sub">' + escapeHtml(connected ? ('Connected - ' + styleName) : 'Disconnected') + '</div>' +
      '</div>' + controllerChevron();
    item.addEventListener('click', openControllerPlayer.bind(null, player));
    list.appendChild(item);
  }
  refreshSettingsSelection();
}

function appendControllerHeader(list, title) {
  const header = document.createElement('div');
  header.className = 'settings-header';
  header.textContent = title;
  list.appendChild(header);
}

function appendControllerSwitch(list, title, checked, onClick) {
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info"><div class="setting-label">' + escapeHtml(title) + '</div></div>' +
    '<button class="settings-toggle' + (checked ? ' on' : '') + '" type="button" aria-label="' + escapeHtml(title) + '"></button>';
  item.addEventListener('click', onClick);
  list.appendChild(item);
}

function appendControllerRow(list, title, subtitle, onClick) {
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(title) + '</div>' +
    (subtitle ? '<div class="setting-sub">' + escapeHtml(subtitle) + '</div>' : '') +
    '</div>' + controllerChevron();
  item.addEventListener('click', onClick);
  list.appendChild(item);
}

function appendControllerMap(list, title, mapId) {
  const subtitle = mapId.indexOf('button:') === 0
    ? NxEmu.getButtonBinding(controllerPlayerIndex, parseInt(mapId.slice(7), 10))
    : NxEmu.getStickBinding(controllerPlayerIndex, stickIdFromMap(mapId), stickPartFromMap(mapId));
  appendControllerRow(list, title, subtitle, function () {
    NxEmu.beginControllerMap(controllerPlayerIndex, mapId, title, controllerFilterIndex);
  });
}

function stickIdFromMap(mapId) {
  return parseInt(mapId.split(':')[1], 10);
}

function stickPartFromMap(mapId) {
  const parts = mapId.split(':');
  return parts[0] === 'modifier' ? 'modifier' : parts[2];
}

function appendControllerSlider(list, title, stick, key, min, max) {
  const value = NxEmu.getStickValue(controllerPlayerIndex, stick, key);
  const item = document.createElement('div');
  item.className = 'settings-item setting-item-static';
  item.innerHTML =
    '<div class="setting-info" style="flex:1">' +
    '<div class="setting-label">' + escapeHtml(title) + ' <span class="slider-value">' + Math.round(value * 100) + '%</span></div>' +
    '<input class="settings-range" type="range" min="' + min + '" max="' + max + '" step="0.01" value="' + value + '">' +
    '</div>';
  const input = item.querySelector('input');
  const label = item.querySelector('.slider-value');
  input.addEventListener('input', function () {
    const next = parseFloat(input.value);
    if (label) label.textContent = Math.round(next * 100) + '%';
    NxEmu.setStickValue(controllerPlayerIndex, stick, key, next);
  });
  list.appendChild(item);
}

function appendStickDirections(list, stick) {
  appendControllerMap(list, 'Up', 'analog:' + stick + ':up');
  appendControllerMap(list, 'Down', 'analog:' + stick + ':down');
  appendControllerMap(list, 'Left', 'analog:' + stick + ':left');
  appendControllerMap(list, 'Right', 'analog:' + stick + ':right');
}

function appendStickExtras(list, stick) {
  if (NxEmu.isControllerStick(controllerPlayerIndex, stick)) {
    appendControllerSlider(list, 'Range', stick, 'range', 0.25, 1.5);
    appendControllerSlider(list, 'Deadzone', stick, 'deadzone', 0, 1);
  } else {
    appendControllerMap(list, 'Modifier', 'modifier:' + stick);
    appendControllerSlider(list, 'Modifier range', stick, 'modifier_scale', 0, 1);
  }
}

function appendButtons(list, buttons) {
  buttons.forEach(function (button) {
    appendControllerMap(list, button[0], 'button:' + button[1]);
  });
}

function renderControllerPlayer() {
  const list = ensureSettingsList();
  if (!list) return;
  const player = controllerPlayerIndex;
  const style = NxEmu.getControllerStyle(player);
  const filters = parseJsonArray(NxEmu.getControllerFilterNames());
  if (controllerFilterIndex >= filters.length) controllerFilterIndex = 0;

  appendControllerSwitch(list, 'Connected', NxEmu.isControllerConnected(player), function () {
    NxEmu.setControllerConnected(player, !NxEmu.isControllerConnected(player));
    renderCurrentSettings();
  });
  appendControllerRow(list, 'Controller type', controllerStyleName(style), openControllerStyles);
  appendControllerRow(list, 'Auto-map a controller', 'Select a device to attempt auto-mapping', openAutoMap);
  appendControllerRow(list, 'Input mapping filter', filters[controllerFilterIndex] || 'Unknown', openControllerFilter);
  appendControllerRow(list, 'Reset to default', '', confirmResetController);

  const full = style === 3 || style === 4 || style === 5;
  const left = style === 6;
  const right = style === 7;
  const gamecube = style === 8;
  if (full) {
    appendControllerHeader(list, 'Buttons');
    appendButtons(list, [['A', BTN.A], ['B', BTN.B], ['X', BTN.X], ['Y', BTN.Y], ['Plus', BTN.Plus], ['Minus', BTN.Minus], ['Home', BTN.Home], ['Capture', BTN.Capture]]);
  } else if (left) {
    appendControllerHeader(list, 'Buttons');
    appendButtons(list, [['Minus', BTN.Minus], ['Capture', BTN.Capture]]);
  } else if (right) {
    appendControllerHeader(list, 'Buttons');
    appendButtons(list, [['A', BTN.A], ['B', BTN.B], ['X', BTN.X], ['Y', BTN.Y], ['Plus', BTN.Plus], ['Home', BTN.Home]]);
  } else if (gamecube) {
    appendControllerHeader(list, 'Buttons');
    appendButtons(list, [['A', BTN.A], ['B', BTN.B], ['X', BTN.X], ['Y', BTN.Y], ['Start/Pause', BTN.Plus]]);
  }

  if (full || left) {
    appendControllerHeader(list, 'D-Pad');
    appendButtons(list, [['Up', BTN.DUp], ['Down', BTN.DDown], ['Left', BTN.DLeft], ['Right', BTN.DRight]]);
  }

  if (full || left) {
    appendControllerHeader(list, 'Left stick');
    appendStickDirections(list, STICK.L);
    appendButtons(list, [['Pressed', BTN.LStick]]);
    appendStickExtras(list, STICK.L);
  } else if (gamecube) {
    appendControllerHeader(list, 'Control stick');
    appendStickDirections(list, STICK.L);
    appendStickExtras(list, STICK.L);
  }

  if (full || right) {
    appendControllerHeader(list, 'Right stick');
    appendStickDirections(list, STICK.R);
    appendButtons(list, [['Pressed', BTN.RStick]]);
    appendStickExtras(list, STICK.R);
  } else if (gamecube) {
    appendControllerHeader(list, 'C-Stick');
    appendStickDirections(list, STICK.R);
    appendStickExtras(list, STICK.R);
  }

  if (style === 3 || style === 4) {
    appendControllerHeader(list, 'Triggers');
    appendButtons(list, [['L', BTN.L], ['R', BTN.R], ['ZL', BTN.ZL], ['ZR', BTN.ZR]]);
  } else if (style === 5) {
    appendControllerHeader(list, 'Triggers');
    appendButtons(list, [['L', BTN.L], ['R', BTN.R], ['ZL', BTN.ZL], ['ZR', BTN.ZR], ['Left SL', BTN.SLLeft], ['Left SR', BTN.SRLeft], ['Right SL', BTN.SLRight], ['Right SR', BTN.SRRight]]);
  } else if (left) {
    appendControllerHeader(list, 'Triggers');
    appendButtons(list, [['L', BTN.L], ['ZL', BTN.ZL], ['Left SL', BTN.SLLeft], ['Left SR', BTN.SRLeft]]);
  } else if (right) {
    appendControllerHeader(list, 'Triggers');
    appendButtons(list, [['R', BTN.R], ['ZR', BTN.ZR], ['Right SL', BTN.SLRight], ['Right SR', BTN.SRRight]]);
  } else if (gamecube) {
    appendControllerHeader(list, 'Triggers');
    appendButtons(list, [['Z', BTN.R], ['L', BTN.ZL], ['R', BTN.ZR]]);
  }
  refreshSettingsSelection();
}

function renderControllerChoices() {
  const list = ensureSettingsList();
  if (!list) return;
  const state = controllerChoiceState;
  const title = document.getElementById('settingsTitle');
  if (title) title.textContent = state.title;
  if (!state.items.length) {
    const empty = document.createElement('div');
    empty.className = 'settings-header';
    empty.textContent = state.emptyText || '';
    list.appendChild(empty);
  }
  state.items.forEach(function (item, index) {
    const row = document.createElement('div');
    row.className = 'settings-item';
    row.innerHTML = '<div class="setting-info"><div class="setting-label">' + escapeHtml(item.label) + '</div></div>' + controllerChevron();
    row.addEventListener('click', function () { state.onPick(item, index); });
    list.appendChild(row);
  });
  refreshSettingsSelection();
}

function openControllerChoices(title, items, onPick, emptyText) {
  controllerChoiceState = {
    title: title,
    items: items,
    onPick: onPick,
    emptyText: emptyText || '',
  };
  if (navStack[navStack.length - 1] === 'controllerChoices') renderCurrentSettings();
  else navigate('controllerChoices');
}

function openControllerStyles() {
  const styles = parseJsonArray(NxEmu.getSupportedControllerStyles(controllerPlayerIndex));
  openControllerChoices('Controller type', styles.map(function (id) {
    return { label: controllerStyleName(id), id: id };
  }), function (item) {
    NxEmu.setControllerStyle(controllerPlayerIndex, item.id);
    goBack();
  });
}

function openAutoMap() {
  const names = parseJsonArray(NxEmu.getAutoMapControllerNames());
  openControllerChoices('Auto-map a controller', names.map(function (name) {
    return { label: name };
  }), function (item, index) {
    NxEmu.autoMapController(controllerPlayerIndex, index);
    alert('Ran auto-mapping against ' + item.label);
    goBack();
  }, 'No controllers found');
}

function openControllerFilter() {
  const names = parseJsonArray(NxEmu.getControllerFilterNames());
  openControllerChoices('Input mapping filter', names.map(function (name) {
    return { label: name };
  }), function (item, index) {
    controllerFilterIndex = index;
    goBack();
  }, 'No controllers found');
}

function confirmResetController() {
  if (!confirm("Reset this player to defaults?")) return;
  NxEmu.resetControllerMappings(controllerPlayerIndex);
  renderCurrentSettings();
}

function showPage(page) {
  document.querySelectorAll('.page').forEach(function (p) { p.style.display = 'none'; });
  const spec = settingPage(page);
  if (spec) {
    document.getElementById('settingsShell').style.display = '';
    const remembered = settingsSelectionByPage[page];
    settingsSelection = remembered == null ? 0 : remembered;
    renderCurrentSettings();
    return;
  }
  const el = document.getElementById(page);
  if (el) el.style.display = '';
  if (page === 'gamesPage' && libraryDirty) refreshGameList();
}
