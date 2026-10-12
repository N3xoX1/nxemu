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
    if (top === 'gameFolders' || top === 'settingsPage' || top === 'setup') renderCurrentSettings();
    if (top === 'gamesPage') refreshGameList();
  }
  if (setting === 'nxui:ThemeMode') {
    applyTheme();
    const top = navStack[navStack.length - 1];
    if (top === 'themeSettings' || top === 'settingsPage') renderCurrentSettings();
  }
}

function handleAndroidBack() {
  if (navStack[navStack.length - 1] === 'setup' && setupStep === 0) return false;
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
  if (navStack[navStack.length - 1] === 'setup') {
    if (setupStep > 0) {
      setupStep--;
      settingsSelection = 0;
      renderCurrentSettings();
    }
    return;
  }
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
  const top = navStack[navStack.length - 1];
  if (top === 'settingsPage' || top === 'setup') renderCurrentSettings();
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
  return shown ? 'Shown' : 'Hidden';
}

function connectedCountLabel() {
  var count = 0;
  for (var player = 0; player < 8; player++) {
    if (NxEmu.isControllerConnected(player)) count++;
  }
  return count === 0 ? 'None connected' : count + ' connected';
}

function readSettingBool(key) {
  try {
    return !!NxEmu.getSettingBool(key);
  } catch (e) {
    return false;
  }
}

function gameIsRunning() {
  return readSettingBool('nxcore:EmulationRunning');
}

function readSettingInt(key) {
  try {
    return NxEmu.getSettingInt(key);
  } catch (e) {
    return 0;
  }
}

function optionLabel(options, value) {
  for (var i = 0; i < options.length; i++) {
    if (options[i].value === value) return options[i].label;
  }
  return String(value);
}

var resolutionOptions = [
  { label: '0.5x', value: 0, scale: 0.5 },
  { label: '0.75x', value: 1, scale: 0.75 },
  { label: '1x', value: 2, scale: 1 },
  { label: '1.5x', value: 3, scale: 1.5 },
  { label: '2x', value: 4, scale: 2 },
  { label: '3x', value: 5, scale: 3 },
  { label: '4x', value: 6, scale: 4 },
  { label: '5x', value: 7, scale: 5 },
  { label: '6x', value: 8, scale: 6 },
  { label: '7x', value: 9, scale: 7 },
  { label: '8x', value: 10, scale: 8 },
];
var accuracyOptions = [
  { label: 'Normal', value: 0, detail: 'Fastest. Works well for most games' },
  { label: 'High', value: 1, detail: 'Fixes some graphical glitches. Slower' },
  { label: 'Extreme', value: 2, detail: 'Most accurate. Much slower, for troubleshooting' },
];
var vsyncOptions = [
  { label: 'Immediate', value: 0, detail: 'Lowest latency. The picture may tear' },
  { label: 'Mailbox', value: 1, detail: 'Low latency without tearing. Uses more power' },
  { label: 'FIFO', value: 2, detail: 'No tearing. Recommended' },
  { label: 'FIFO Relaxed', value: 3, detail: 'Like FIFO, but tears briefly instead of stuttering when a frame is late' },
];
var aspectOptions = [
  { label: '16:9', value: 0, detail: 'Same as the Switch. Recommended' },
  { label: '4:3', value: 1, detail: 'Stretches the picture to 4:3' },
  { label: '21:9', value: 2, detail: 'Stretches the picture to ultrawide' },
  { label: '16:10', value: 3, detail: 'Stretches the picture to 16:10' },
  { label: 'Stretch', value: 4, detail: 'Fills the whole screen' },
];
var anisotropyOptions = [
  { label: 'Automatic', value: 0, detail: 'Picks a level to match the resolution' },
  { label: 'Default', value: 1, detail: 'What the game asks for. Fastest' },
  { label: '2x', value: 2, detail: 'Slightly sharper textures on floors and walls' },
  { label: '4x', value: 3, detail: 'Sharper textures on floors and walls' },
  { label: '8x', value: 4, detail: 'Sharper still. Costs more on slower phones' },
  { label: '16x', value: 5, detail: 'Sharpest. Costs the most' },
];
var antiAliasingOptions = [
  { label: 'None', value: 0, detail: 'Fastest' },
  { label: 'FXAA', value: 1, detail: 'Smooths jagged edges cheaply. Slightly blurry' },
  { label: 'SMAA', value: 2, detail: 'Smoother edges than FXAA. Costs a little more' },
];
var scalingOptions = [
  { label: 'Nearest neighbor', value: 0, detail: 'Crisp pixels. Can look blocky' },
  { label: 'Bilinear', value: 1, detail: 'Smooth and slightly soft' },
  { label: 'Bicubic', value: 2, detail: 'Smooth and a little sharper than bilinear' },
  { label: 'Gaussian', value: 3, detail: 'Very smooth and soft' },
  { label: 'ScaleForce', value: 4, detail: 'Sharp upscaling, good for low resolutions' },
  { label: 'FSR', value: 5, detail: 'AMD FidelityFX upscaling. Sharpest, costs a little performance' },
];
var speedLimitOptions = [
  { label: '25%', value: 25 },
  { label: '50%', value: 50, detail: 'Half speed' },
  { label: '75%', value: 75 },
  { label: '100%', value: 100, detail: 'Normal speed' },
  { label: '125%', value: 125 },
  { label: '150%', value: 150 },
  { label: '200%', value: 200, detail: 'Double speed' },
  { label: '300%', value: 300 },
  { label: '400%', value: 400 },
];
var settingChoiceState = null;

function dockedLabel() {
  return readSettingInt('nxos:DockedMode') === 1 ? 'Docked' : 'Handheld';
}

function consoleSummary() {
  if (!readSettingBool('nxos:UseSpeedLimit')) return dockedLabel() + ' · Unlimited speed';
  const limit = readSettingInt('nxos:SpeedLimit');
  return limit === 100 ? dockedLabel() : dockedLabel() + ' · Speed limit ' + limit + '%';
}

// The Switch renders at 720p handheld and 1080p docked; the scale multiplies that.
function resolutionHeight(option, docked) {
  return Math.round((docked ? 1080 : 720) * option.scale) + 'p';
}

function resolutionOption() {
  const value = readSettingInt('nxvideo:ResolutionSetup');
  for (var i = 0; i < resolutionOptions.length; i++) {
    if (resolutionOptions[i].value === value) return resolutionOptions[i];
  }
  return null;
}

function resolutionLabel() {
  const option = resolutionOption();
  if (!option) return '';
  const docked = readSettingInt('nxos:DockedMode') === 1;
  return option.label + ' · ' + resolutionHeight(option, docked) + (docked ? ' docked' : ' handheld');
}

function resolutionOptionDetail(option) {
  return resolutionHeight(option, false) + ' handheld · ' + resolutionHeight(option, true) + ' docked';
}

function graphicsSummary() {
  const option = resolutionOption();
  if (!option) return '';
  return 'Resolution ' + option.label + ' (' + resolutionHeight(option, readSettingInt('nxos:DockedMode') === 1) + ')';
}

function gpuDriverSupported() {
  try {
    return !!JSON.parse(NxEmu.gpuDriverList()).supported;
  } catch (e) {
    return false;
  }
}

function gpuDriverLabel() {
  try {
    const info = JSON.parse(NxEmu.gpuDriverList());
    return info.active || 'System';
  } catch (e) {
    return 'System';
  }
}

function renderGpuDriversPage() {
  const top = navStack[navStack.length - 1];
  if (top === 'gpuDrivers' || top === 'graphicsSettings' || top === 'setup' || isGpuDownloadPage(top)) renderCurrentSettings();
}

var gpuFetch = { catalog: null, loading: false, group: null, release: null, download: null };

function isGpuDownloadPage(id) {
  return id === 'gpuDriverDownload' || id === 'gpuDriverGroup' || id === 'gpuDriverRelease';
}

function loadGpuCatalog(force) {
  if (gpuFetch.loading) return;
  gpuFetch.loading = true;
  NxEmu.fetchGpuDriverCatalog(!!force);
}

function onGpuDriverCatalog(catalog) {
  gpuFetch.loading = false;
  gpuFetch.catalog = catalog;
  const top = navStack[navStack.length - 1];
  if (isGpuDownloadPage(top) || top === 'setup' || top === 'gpuDrivers') renderCurrentSettings();
}

function onGpuDriverDownload(event) {
  if (!gpuFetch.download) return;
  if (event.result) {
    gpuFetch.download = null;
  } else {
    if (gpuFetch.download.percent === event.percent) return;
    gpuFetch.download.percent = event.percent;
    const label = document.getElementById('gpuDownloadStatus');
    if (label) {
      label.textContent = 'Downloading ' + event.percent + '%';
      return;
    }
  }
  renderGpuDriversPage();
}

function storedGpuDriverFiles() {
  try {
    return (JSON.parse(NxEmu.gpuDriverList()).drivers || []).reduce(function (files, driver) {
      files[driver.file] = driver;
      return files;
    }, {});
  } catch (e) {
    return {};
  }
}

function formatBytes(size) {
  if (!size) return '';
  if (size < 1024 * 1024) return Math.max(1, Math.round(size / 1024)) + ' KB';
  return (size / (1024 * 1024)).toFixed(1) + ' MB';
}

function formatReleaseDate(text) {
  const date = new Date(text);
  if (isNaN(date.getTime())) return '';
  return date.toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}

function startGpuDownload(repo, tag, asset) {
  if (gpuFetch.download) return;
  gpuFetch.download = { repo: repo, tag: tag, asset: asset, percent: 0 };
  NxEmu.downloadGpuDriver(repo, tag, asset);
  renderCurrentSettings();
}

function isGpuDownloading(repo, tag, asset) {
  const d = gpuFetch.download;
  return !!d && d.repo === repo && d.tag === tag && d.asset === asset;
}

function appendGpuAssetRow(list, title, repo, tag, asset, size, file, stored, published) {
  const downloading = isGpuDownloading(repo, tag, asset);
  const installed = stored[file];
  const released = published ? formatReleaseDate(published) : '';
  var status = [formatBytes(size), released ? 'Released ' + released : ''].filter(Boolean).join(' · ');
  if (downloading) status = 'Downloading ' + gpuFetch.download.percent + '%';
  else if (installed && installed.selected) status = 'Installed · in use';
  else if (installed) status = 'Installed · tap to use';
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(title) + '</div>' +
    '<div class="setting-sub"' + (downloading ? ' id="gpuDownloadStatus"' : '') + '>' + escapeHtml(status) + '</div>' +
    '</div>' +
    (installed
      ? '<svg class="setting-check' + (installed.selected ? ' on' : '') + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M20 6L9 17l-5-5"/></svg>'
      : '<svg class="setting-download' + (downloading ? ' busy' : '') + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11"/><path d="M7 10l5 5 5-5"/><path d="M5 20h14"/></svg>');
  item.addEventListener('click', function () {
    if (gpuFetch.download) return;
    if (installed) {
      if (!installed.selected) NxEmu.selectGpuDriver(file);
      return;
    }
    startGpuDownload(repo, tag, asset);
  });
  list.appendChild(item);
}

function renderGpuCatalogState(host) {
  const catalog = gpuFetch.catalog;
  if (!catalog && !gpuFetch.loading) loadGpuCatalog(false);
  if (!catalog) {
    host.innerHTML = '<div class="empty-hint">Loading drivers from GitHub…</div>';
    refreshSettingsSelection();
    return null;
  }
  if (catalog.error) {
    host.innerHTML =
      '<div class="empty-state">' +
      '<p class="empty-title">Couldn\'t load drivers</p>' +
      '<p class="empty-copy">' + escapeHtml(catalog.error) + '</p>' +
      '<button class="primary-btn" type="button">Try again</button>' +
      '</div>';
    host.querySelector('.primary-btn').addEventListener('click', function () {
      gpuFetch.catalog = null;
      loadGpuCatalog(true);
      renderCurrentSettings();
    });
    refreshSettingsSelection();
    return null;
  }
  return catalog;
}

function renderGpuDriverDownload() {
  const host = document.getElementById('settingsBody');
  if (!host) return;
  const catalog = renderGpuCatalogState(host);
  if (!catalog) return;
  const list = ensureSettingsList();
  if (!list) return;
  const stored = storedGpuDriverFiles();
  const rec = catalog.recommended;
  const header = document.createElement('div');
  header.className = 'settings-header';
  header.textContent = catalog.gpu ? 'Recommended for ' + catalog.gpu : 'Recommended';
  list.appendChild(header);
  if (rec && rec.asset) {
    appendGpuAssetRow(list, rec.label, rec.repo, rec.tag, rec.asset, rec.size, rec.file, stored, rec.published);
  } else {
    const none = document.createElement('div');
    none.className = 'setting-sub';
    none.textContent = rec
      ? rec.label + ' isn\'t listed right now. Pick a driver below.'
      : 'No specific driver is recommended for this GPU. A recent Turnip build is a good start.';
    list.appendChild(none);
  }
  const sources = document.createElement('div');
  sources.className = 'settings-header';
  sources.textContent = 'All drivers';
  list.appendChild(sources);
  (catalog.groups || []).forEach(function (group) {
    const releases = group.releases || [];
    const latest = releases.filter(function (r) { return r.latest; })[0] || releases[0];
    var detail = group.error || (latest ? 'Latest: ' + latest.title + ' · ' + releases.length + ' releases' : 'No downloads');
    if (group.stale) detail += ' · offline copy';
    appendLinkRow(list, {
      label: group.name,
      detail: detail,
      run: function () {
        if (!releases.length) return;
        gpuFetch.group = group;
        navigate('gpuDriverGroup');
      },
    });
  });
  appendLinkRow(list, {
    label: gpuFetch.loading ? 'Refreshing…' : 'Refresh list',
    detail: 'Check GitHub for new releases',
    run: function () {
      loadGpuCatalog(true);
      renderCurrentSettings();
    },
  });
  refreshSettingsSelection();
}

function renderGpuDriverGroup() {
  const group = gpuFetch.group;
  const list = ensureSettingsList();
  if (!list || !group) return;
  const stored = storedGpuDriverFiles();
  (group.releases || []).forEach(function (release) {
    const tags = [];
    if (release.latest) tags.push('Latest');
    if (release.prerelease) tags.push('Pre-release');
    const date = formatReleaseDate(release.published);
    if (date) tags.push(date);
    if (release.assets.some(function (a) { return stored[a.file]; })) tags.push('Installed');
    appendLinkRow(list, {
      label: release.title,
      detail: tags.join(' · '),
      run: function () {
        gpuFetch.release = release;
        navigate('gpuDriverRelease');
      },
    });
  });
  refreshSettingsSelection();
}

function renderGpuDriverRelease() {
  const group = gpuFetch.group;
  const release = gpuFetch.release;
  const list = ensureSettingsList();
  if (!list || !group || !release) return;
  const stored = storedGpuDriverFiles();
  const header = document.createElement('div');
  header.className = 'settings-header';
  header.textContent = release.assets.length > 1 ? 'Downloads · pick one' : 'Download';
  list.appendChild(header);
  release.assets.forEach(function (asset) {
    appendGpuAssetRow(list, asset.name, group.repo, release.tag, asset.name, asset.size, asset.file, stored);
  });
  if (release.notes) {
    const notesHeader = document.createElement('div');
    notesHeader.className = 'settings-header';
    notesHeader.textContent = 'Release notes';
    list.appendChild(notesHeader);
    const notes = document.createElement('div');
    notes.className = 'release-notes';
    notes.textContent = release.notes.replace(/\r\n/g, '\n').trim();
    list.appendChild(notes);
  }
  refreshSettingsSelection();
}

const settingPages = [
  {
    id: 'settingsPage',
    title: 'Settings',
    rows: [
      { label: 'Game folders', type: 'page', page: 'gameFolders', detail: folderCountLabel },
      { label: 'Controllers', type: 'page', page: 'controllerSettings', detail: connectedCountLabel },
      { label: 'On screen controller', type: 'page', page: 'overlaySettings', detail: overlayShowLabel },
      { label: 'Console', type: 'page', page: 'systemSettings', detail: consoleSummary },
      { label: 'Graphics', type: 'page', page: 'graphicsSettings', detail: graphicsSummary },
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
    id: 'systemSettings',
    title: 'Console',
    rows: [
      { label: 'Limit speed', type: 'bool', key: 'nxos:UseSpeedLimit', detail: 'Off runs as fast as possible. This session only', save: false, visible: gameIsRunning },
      { label: 'Speed limit', type: 'select', key: 'nxos:SpeedLimit', options: speedLimitOptions, format: function () { return readSettingInt('nxos:SpeedLimit') + '%'; } },
      { label: 'Docked mode', type: 'bool', key: 'nxos:DockedMode', intBool: true, detail: 'Higher resolution, lower performance' },
    ],
  },
  {
    id: 'graphicsSettings',
    title: 'Graphics',
    rows: [
      { label: 'GPU driver', type: 'page', page: 'gpuDrivers', detail: gpuDriverLabel, visible: gpuDriverSupported, restart: true },
      { label: 'Accuracy', type: 'select', key: 'nxvideo:AccuracyLevel', options: accuracyOptions },
      { label: 'Resolution', type: 'select', key: 'nxvideo:ResolutionSetup', options: resolutionOptions, format: resolutionLabel, optionDetail: resolutionOptionDetail, restart: true },
      { label: 'VSync', type: 'select', key: 'nxvideo:VSyncMode', options: vsyncOptions },
      { label: 'Window adapting filter', type: 'select', key: 'nxvideo:ScalingFilter', options: scalingOptions },
      { label: 'FSR sharpness', type: 'int', key: 'nxvideo:FSPSharpness', min: 0, max: 200, suffix: '%', visible: function () { return readSettingInt('nxvideo:ScalingFilter') === 5; } },
      { label: 'Anti-aliasing', type: 'select', key: 'nxvideo:AntiAliasing', options: antiAliasingOptions },
      { label: 'Anisotropic filtering', type: 'select', key: 'nxvideo:AnisotropicFiltering', options: anisotropyOptions },
      { label: 'Aspect ratio', type: 'select', key: 'nxvideo:AspectRatio', options: aspectOptions },
      { label: 'Force maximum clocks', type: 'bool', key: 'nxvideo:ForceMaximumClocks', detail: 'Adreno GPUs. Can overheat the device', restart: true },
      { label: 'Asynchronous shaders', type: 'bool', key: 'nxvideo:UseAsynchronousShaderBuilding', detail: 'Reduces stutter, but can glitch', restart: true },
      { label: 'Reactive flushing', type: 'bool', key: 'nxvideo:EnableReactiveFlushing', detail: 'More accurate, costs performance', restart: true },
      { label: 'Disk shader cache', type: 'bool', key: 'nxvideo:UseDiskPipelineCache', detail: 'Games load faster after the first run', restart: true },
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
  { id: 'setup', title: setupTitle, render: renderSetup },
  { id: 'gpuDrivers', title: 'GPU driver', render: renderGpuDrivers },
  {
    id: 'gpuDriverAdd',
    title: 'Add a driver',
    rows: [
      { label: 'Download', detail: 'Browse Turnip and other drivers on GitHub', run: function () { navStack.pop(); navigate('gpuDriverDownload'); } },
      { label: 'Install from file', detail: 'Choose a driver .zip already on this device', run: function () { goBack(); NxEmu.installGpuDriver(); } },
    ],
  },
  { id: 'gpuDriverDownload', title: 'Download drivers', render: renderGpuDriverDownload },
  { id: 'gpuDriverGroup', title: function () { return gpuFetch.group ? gpuFetch.group.name : 'Drivers'; }, render: renderGpuDriverGroup },
  { id: 'gpuDriverRelease', title: function () { return gpuFetch.release ? gpuFetch.release.title : 'Driver'; }, render: renderGpuDriverRelease },
  { id: 'gameFolders', title: 'Game folders', render: renderGameFolders },
  { id: 'controllerSettings', title: 'Controllers', render: renderControllerSettings },
  { id: 'controllerPlayer', title: function () { return 'Controller ' + (controllerPlayerIndex + 1); }, render: renderControllerPlayer },
  { id: 'controllerChoices', title: function () { return controllerChoiceState.title; }, render: renderControllerChoices },
  { id: 'settingChoices', title: function () { return settingChoiceState ? settingChoiceState.label : 'Settings'; }, render: renderSettingChoices },
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
  const running = gameIsRunning();
  var hidden = 0;
  spec.rows.forEach(function (row) {
    if (row.visible && !row.visible()) return;
    if (row.restart && running) {
      hidden++;
      return;
    }
    if (row.type === 'bool') appendBoolRow(list, row);
    else if (row.type === 'int') appendIntRow(list, row);
    else if (row.type === 'choice') appendChoiceRow(list, row);
    else if (row.type === 'select') appendSelectRow(list, row);
    else appendLinkRow(list, row);
  });
  if (hidden) {
    const note = document.createElement('div');
    note.className = 'settings-header';
    note.textContent = 'Close the game to change resolution, the GPU driver and other options that apply when a game starts.';
    list.insertBefore(note, list.firstChild);
  }
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

function rowIsOn(row) {
  if (row.intBool) return readSettingInt(row.key) === 1;
  return readSettingBool(row.key);
}

function appendBoolRow(list, row) {
  const on = rowIsOn(row);
  const detail = rowDetail(row);
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(row.label) + '</div>' +
    (detail ? '<div class="setting-sub">' + escapeHtml(detail) + '</div>' : '') +
    '</div>' +
    '<button class="settings-toggle' + (on ? ' on' : '') + '" type="button" aria-label="' + escapeHtml(row.label) + '"></button>';
  item.addEventListener('click', function () {
    if (row.intBool) NxEmu.setSettingInt(row.key, rowIsOn(row) ? 0 : 1);
    else NxEmu.setSettingBool(row.key, !readSettingBool(row.key));
    if (row.save !== false) NxEmu.saveSettings();
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

function appendSelectRow(list, row) {
  const item = document.createElement('div');
  item.className = 'settings-item';
  const detail = row.format ? row.format() : optionLabel(row.options, readSettingInt(row.key));
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(row.label) + '</div>' +
    (detail ? '<div class="setting-sub">' + escapeHtml(detail) + '</div>' : '') +
    '</div>' + controllerChevron();
  item.addEventListener('click', function () {
    settingChoiceState = row;
    navigate('settingChoices');
  });
  list.appendChild(item);
}

function renderSettingChoices() {
  const spec = settingChoiceState;
  const list = ensureSettingsList();
  if (!list || !spec) return;
  const current = readSettingInt(spec.key);
  spec.options.forEach(function (option) {
    const on = current === option.value;
    const detail = spec.optionDetail ? spec.optionDetail(option) : (option.detail || '');
    const item = document.createElement('div');
    item.className = 'settings-item';
    item.innerHTML =
      '<div class="setting-info">' +
      '<div class="setting-label">' + escapeHtml(option.label) + '</div>' +
      (detail ? '<div class="setting-sub">' + escapeHtml(detail) + '</div>' : '') +
      '</div>' +
      '<svg class="setting-check' + (on ? ' on' : '') + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M20 6L9 17l-5-5"/></svg>';
    item.addEventListener('click', function () {
      NxEmu.setSettingInt(spec.key, option.value);
      NxEmu.saveSettings();
      goBack();
    });
    list.appendChild(item);
  });
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
  const back = document.querySelector('#settingsShell .back-btn');
  if (back) back.style.visibility = navStack[navStack.length - 1] === 'setup' && setupStep === 0 ? 'hidden' : '';
  if (spec.rows) renderSettingRows(spec);
  else if (spec.render) spec.render();
  refreshSettingsSelection();
}

var setupStep = 0;
var setupSteps = [];

// Runs on the home screen. Shows first-run setup until it has been finished once;
// people who already added game folders are treated as set up.
function startApp() {
  var complete = true;
  try { complete = NxEmu.isSetupComplete(); } catch (e) {}
  if (complete) {
    refreshGameList();
    return;
  }
  if (getConfiguredFolders().length) {
    NxEmu.completeSetup();
    refreshGameList();
    return;
  }
  setupSteps = ['folders', 'controls'];
  var recommendation = '';
  try { recommendation = NxEmu.gpuDriverRecommendation(); } catch (e) {}
  if (gpuDriverSupported() && gpuDriverLabel() === 'System' && recommendation) setupSteps.push('driver');
  setupStep = 0;
  navStack = ['gamesPage', 'setup'];
  showPage('setup');
}

function finishSetup() {
  NxEmu.completeSetup();
  navStack = ['gamesPage'];
  settingsSelection = -1;
  showPage('gamesPage');
  applyControllerBrowser();
}

function setupTitle() {
  const step = setupSteps[setupStep];
  if (step === 'folders') return 'Welcome to NxEmu';
  if (step === 'controls') return 'Controls';
  return 'GPU driver';
}

function setupNext() {
  if (setupStep >= setupSteps.length - 1) {
    finishSetup();
    return;
  }
  setupStep++;
  settingsSelection = 0;
  renderCurrentSettings();
}

function appendSetupIntro(list, copy) {
  const intro = document.createElement('div');
  intro.className = 'setup-intro';
  intro.innerHTML =
    '<p class="setup-step">Step ' + (setupStep + 1) + ' of ' + setupSteps.length + '</p>' +
    '<p class="setup-copy">' + escapeHtml(copy) + '</p>';
  list.appendChild(intro);
}

function appendSetupNext(list, label, done) {
  const actions = document.createElement('div');
  actions.className = 'setup-actions';
  const next = document.createElement('button');
  next.className = done ? 'primary-btn done-btn' : 'primary-btn';
  next.type = 'button';
  next.textContent = label;
  next.addEventListener('click', setupNext);
  actions.appendChild(next);
  list.appendChild(actions);
}

function connectedControllerList() {
  try {
    const names = JSON.parse(NxEmu.connectedControllerNames());
    return Array.isArray(names) ? names : [];
  } catch (e) {
    return [];
  }
}

function renderSetup() {
  const list = ensureSettingsList();
  if (!list) return;
  const step = setupSteps[setupStep];
  const last = setupStep === setupSteps.length - 1;
  if (step === 'folders') {
    const folders = getConfiguredFolders();
    appendSetupIntro(list, 'Choose the folders that hold your games. NxEmu reads them where they are and never moves or copies your files.');
    folders.forEach(function (uri) {
      const folder = document.createElement('div');
      folder.className = 'setup-folder';
      folder.textContent = formatGameFolder(uri);
      list.appendChild(folder);
    });
    appendLinkRow(list, {
      label: folders.length ? 'Add another folder' : 'Add game folder',
      detail: 'Games in .nro, .dxci or .dnsp format',
      run: function () { NxEmu.addGameDirectory(); },
    });
    if (!folders.length) appendSetupNext(list, 'Skip');
    else appendSetupNext(list, last ? 'Done' : 'Next', last);
  } else if (step === 'controls') {
    const names = connectedControllerList();
    appendSetupIntro(list, names.length
      ? 'Controller connected: ' + names.join(', ') + '. It\'s ready to play. Use A to select and B to go back.'
      : 'No controller needed. Touch controls appear on screen while you play. You can connect a Bluetooth or USB controller at any time.');
    appendBoolRow(list, { label: 'Show on-screen controls', type: 'bool', key: 'nxui:ShowInputOverlay', detail: names.length ? 'Usually off when using a controller' : 'Turn off if you only play with a controller' });
    if (names.length) appendLinkRow(list, { label: 'Controller settings', detail: 'Change button mapping', page: 'controllerSettings' });
    appendSetupNext(list, last ? 'Done' : 'Next', last);
  } else {
    const catalog = gpuFetch.catalog;
    if (!catalog && !gpuFetch.loading) loadGpuCatalog(false);
    appendSetupIntro(list, 'Your phone has ' + ((catalog && catalog.gpu) || 'an Adreno GPU') + '. Many games run faster and with fewer glitches on a community driver. You can switch back to the system driver at any time in Settings, Graphics.');
    const rec = catalog && catalog.recommended;
    if (!catalog) {
      const loading = document.createElement('div');
      loading.className = 'empty-hint';
      loading.textContent = 'Finding the best driver for your phone…';
      list.appendChild(loading);
    } else if (rec && rec.asset) {
      appendGpuAssetRow(list, 'Install ' + rec.label, rec.repo, rec.tag, rec.asset, rec.size, rec.file, storedGpuDriverFiles(), rec.published);
    } else {
      const none = document.createElement('div');
      none.className = 'setup-folder';
      none.textContent = catalog.error || 'The recommended driver isn\'t available right now. You can install one later from Settings, Graphics, GPU driver.';
      list.appendChild(none);
    }
    const active = gpuDriverLabel() !== 'System';
    appendSetupNext(list, active ? 'Done' : 'Skip', active);
  }
}

function renderGpuDrivers() {
  const host = document.getElementById('settingsBody');
  if (!host) return;
  var info = { supported: false, active: 'System', drivers: [] };
  try {
    info = JSON.parse(NxEmu.gpuDriverList());
  } catch (e) {}
  if (!info.supported) {
    host.innerHTML =
      '<div class="empty-state">' +
      '<p class="empty-title">Adreno only</p>' +
      '<p class="empty-copy">Custom GPU drivers need an Adreno GPU. This device keeps using the system driver.</p>' +
      '</div>';
    refreshSettingsSelection();
    return;
  }
  const list = ensureSettingsList();
  if (!list) return;
  const note = document.createElement('div');
  note.className = 'settings-header';
  note.textContent = 'Applies the next time a game starts.';
  list.appendChild(note);
  appendGpuDriverRow(list, 'System', 'Device driver', info.active === 'System', function () {
    NxEmu.selectGpuDriver('');
  }, null);
  (info.drivers || []).forEach(function (driver) {
    appendGpuDriverRow(list, driver.name, driver.version || '', !!driver.selected, function () {
      NxEmu.selectGpuDriver(driver.file);
    }, function (event) {
      event.stopPropagation();
      if (!confirm('Remove ' + driver.name + '?')) return;
      NxEmu.removeGpuDriver(driver.file);
    });
  });
  appendGpuRecommendation(list);
  const fab = document.createElement('button');
  fab.className = 'fab';
  fab.type = 'button';
  fab.setAttribute('aria-label', 'Add a driver');
  fab.innerHTML = '<svg viewBox="0 0 24 24"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>';
  fab.addEventListener('click', function () { navigate('gpuDriverAdd'); });
  host.appendChild(fab);
  refreshSettingsSelection();
}

// Offers the recommended driver under the installed list until it has been installed.
function appendGpuRecommendation(list) {
  const catalog = gpuFetch.catalog;
  if (!catalog) {
    if (!gpuFetch.loading) loadGpuCatalog(false);
    return;
  }
  const rec = catalog.recommended;
  if (!rec || !rec.asset) return;
  const stored = storedGpuDriverFiles();
  if (stored[rec.file]) return;
  const header = document.createElement('div');
  header.className = 'settings-header';
  header.textContent = catalog.gpu ? 'Recommended for ' + catalog.gpu : 'Recommended';
  list.appendChild(header);
  appendGpuAssetRow(list, rec.label, rec.repo, rec.tag, rec.asset, rec.size, rec.file, stored, rec.published);
}

function appendGpuDriverRow(list, title, subtitle, selected, onSelect, onRemove) {
  const item = document.createElement('div');
  item.className = 'settings-item';
  item.innerHTML =
    '<div class="setting-info">' +
    '<div class="setting-label">' + escapeHtml(title) + '</div>' +
    (subtitle ? '<div class="setting-sub">' + escapeHtml(subtitle) + '</div>' : '') +
    '</div>' +
    (onRemove
      ? '<button class="folder-remove" type="button" aria-label="Remove driver">' +
        '<svg viewBox="0 0 1024 1024"><path d="M160 256H96a32 32 0 0 1 0-64h256V95.936a32 32 0 0 1 32-32h256a32 32 0 0 1 32 32V192h256a32 32 0 1 1 0 64h-64v672a32 32 0 0 1-32 32H192a32 32 0 0 1-32-32zm448-64v-64H416v64zM224 896h576V256H224zm192-128a32 32 0 0 1-32-32V416a32 32 0 0 1 64 0v320a32 32 0 0 1-32 32m192 0a32 32 0 0 1-32-32V416a32 32 0 0 1 64 0v320a32 32 0 0 1-32 32"/></svg>' +
        '</button>'
      : '') +
    '<svg class="setting-check' + (selected ? ' on' : '') + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M20 6L9 17l-5-5"/></svg>';
  item.addEventListener('click', onSelect);
  if (onRemove) {
    item.querySelector('.folder-remove').addEventListener('click', onRemove);
  }
  list.appendChild(item);
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
