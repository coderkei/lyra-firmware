import { Fat16Volume } from './fat16.js';

const $ = (id) => document.getElementById(id);
const canvas = $('screen');
const context = canvas.getContext('2d', { alpha: false });
context.imageSmoothingEnabled = false;

let worker = new Worker('./wasm/worker.js', { type: 'module' });
const INTERNAL_FLASH_SIZE = 16 * 1024 * 1024;
// These offsets and sizes mirror the active ESP-IDF partition table in partitions.csv.
const FLASH_PARTITIONS = [
  { name: 'nvs', label: 'NVS', type: 'data · nvs', offset: 0x9000, size: 0x10000 },
  { name: 'otadata', label: 'OTA data', type: 'data · ota', offset: 0x19000, size: 0x2000 },
  { name: 'ota_0', label: 'OTA 0', type: 'app · ota_0', offset: 0x20000, size: 0x770000 },
  { name: 'ota_1', label: 'OTA 1', type: 'app · ota_1', offset: 0x790000, size: 0x770000 },
  { name: 'littlefs', label: 'LittleFS', type: 'data · spiffs', offset: 0xF00000, size: 0x100000 },
];
let sequence = 1;
let requests = new Map();
let readyResolve;
let readyReject;
let ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
let firmwareBuffer = null;
let firmwareName = 'lyra_firmware_merged.bin';
let cardImage = null;
let cardVolume = null;
let folderStack = [];
let selectedEntry = null;
let running = false;
let pointerDown = false;
let lastTouch = { x: 0, y: 0 };
let paused = false;
let consoleLines = [];
const maxConsoleLines = 5000;
let storageSyncPromise = null;
let storageErrorLogged = false;
let flashGeneration = 0;
let flashReady = false;
let flashManagerBusy = false;

function setState(label, mode = '') {
  const el = $('runState');
  el.className = `run-state ${mode}`;
  el.querySelector('span').textContent = label;
}

function addConsole(text) {
  const cleaned = String(text).replace(/\r/g, '').trimEnd();
  if (!cleaned) return;
  consoleLines.push(...cleaned.split('\n'));
  if (consoleLines.length > maxConsoleLines) consoleLines = consoleLines.slice(-maxConsoleLines);
  $('console').textContent = consoleLines.join('\n');
  $('console').scrollTop = $('console').scrollHeight;
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function humanFlashSize(size) {
  return size >= 1024 * 1024 ? `${(size / (1024 * 1024)).toFixed(size % (1024 * 1024) ? 1 : 0)} MiB` : `${size / 1024} KiB`;
}

function formatFlashOffset(offset) {
  return `0x${offset.toString(16).toUpperCase().padStart(6, '0')}`;
}

function refreshFlashControls() {
  const enabled = flashReady && !flashManagerBusy;
  for (const id of ['partitionSelect', 'partitionFormat', 'exportPartition', 'importPartition', 'clearPartition', 'exportFullFlash', 'importFullFlash', 'resetFlash']) {
    $(id).disabled = !enabled;
  }
  $('chooseFirmware').disabled = flashManagerBusy;
  $('restartFirmware').disabled = flashManagerBusy || !flashReady;
  $('pauseFirmware').disabled = flashManagerBusy || !flashReady || (!running && !paused);
  for (const id of ['flashTag', 'flashModalTag']) {
    $(id).textContent = flashManagerBusy ? 'WORKING' : flashReady ? 'READY' : 'WAITING';
    $(id).classList.toggle('ready', flashReady && !flashManagerBusy);
  }
}

function selectedFlashPartition() {
  return FLASH_PARTITIONS.find((partition) => partition.name === $('partitionSelect').value) ?? FLASH_PARTITIONS[0];
}

function updatePartitionDetail() {
  const partition = selectedFlashPartition();
  $('partitionDetail').textContent = `${partition.type} · ${humanFlashSize(partition.size)} · starts at ${formatFlashOffset(partition.offset)}`;
}

function initializePartitionControls() {
  const select = $('partitionSelect');
  for (const partition of FLASH_PARTITIONS) {
    const option = document.createElement('option');
    option.value = partition.name;
    option.textContent = `${partition.label} · ${humanFlashSize(partition.size)}`;
    select.append(option);
  }
  updatePartitionDetail();
  refreshFlashControls();
}

function saveConsoleLog() {
  const header = [
    'Lyra firmware emulator serial log',
    `Firmware: ${firmwareName}`,
    `Saved: ${new Date().toISOString()}`,
    '',
  ].join('\n');
  downloadBlob(new Blob([header, consoleLines.join('\n'), '\n'], { type: 'text/plain;charset=utf-8' }), 'lyra-serial-log.txt');
}

function captureScreen() {
  if ($('captureScreen').disabled) return;
  const filename = `lyra-screen-${new Date().toISOString().replace(/[:.]/g, '-')}.png`;
  canvas.toBlob((blob) => {
    if (!blob) {
      addConsole('[capture] Could not encode the display as PNG.');
      return;
    }
    downloadBlob(blob, filename);
    addConsole(`[capture] Saved ${canvas.width} × ${canvas.height} screen as ${filename}.`);
  }, 'image/png');
}

function setPreviewScale(scale) {
  const native = scale === '1x';
  $('deviceShell').classList.toggle('scale-1x', native);
  $('fitScale').setAttribute('aria-pressed', String(!native));
  $('nativeScale').setAttribute('aria-pressed', String(native));
  updateNativeScreenSize();
  try { localStorage.setItem('lyra-preview-scale', native ? '1x' : 'fit'); } catch { }
}

function updateNativeScreenSize() {
  const ratio = window.devicePixelRatio || 1;
  $('deviceShell').style.setProperty('--screen-width', `${canvas.width / ratio}px`);
  $('deviceShell').style.setProperty('--screen-height', `${canvas.height / ratio}px`);
}

function updatePace(pace) {
  $('paceSpeed').textContent = `${pace.speed.toFixed(2)}×`;
  $('paceMips').textContent = `${pace.mips.toFixed(1)} MIPS`;
  $('paceResyncs').textContent = String(pace.resyncs);
}

function setOverlay(title, detail, failed = false) {
  const overlay = $('screenOverlay');
  overlay.classList.remove('hidden');
  overlay.querySelector('strong').textContent = title;
  overlay.querySelector('span').textContent = detail;
  overlay.querySelector('.spinner').style.display = failed ? 'none' : '';
  if (failed) overlay.style.background = '#11151ceF';
}

function request(op, fields = {}, transfer = []) {
  const requestId = sequence++;
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      requests.delete(requestId);
      reject(new Error(`Emulator did not answer: ${op}`));
    }, 120000);
    requests.set(requestId, { resolve, reject, timeout });
    worker.postMessage({ op, requestId, ...fields }, transfer);
  });
}

function handleWorkerMessage(event, sourceWorker) {
  if (sourceWorker !== worker) return;
  const message = event.data;
  if (message.bin instanceof ArrayBuffer) {
    try { drawFrame(message.bin); }
    catch (error) { addConsole(`[display] ${error.message}`); }
    finally { if (message.ack) sourceWorker.postMessage({ op: 'frame-ack' }); }
    return;
  }
  if (message.ready) {
    readyResolve?.();
    readyResolve = null;
    readyReject = null;
    return;
  }
  if (message.requestId !== undefined && requests.has(message.requestId)) {
    const pending = requests.get(message.requestId);
    clearTimeout(pending.timeout);
    requests.delete(message.requestId);
    if (message.error) pending.reject(new Error(message.error));
    else pending.resolve(message);
  }
  if (message.text !== undefined) {
    try {
      const packet = JSON.parse(message.text);
      if (packet.t === 'serial') addConsole(packet.data);
      else if (packet.t === 'emu') addConsole(packet.msg);
      else if (packet.t === 'board') addConsole(`Board: ${packet.name}`);
    } catch { addConsole(message.text); }
  }
  if (message.pace) updatePace(message.pace);
  if (message.log !== undefined) addConsole(message.log);
  if (message.stopped !== undefined) {
    running = false;
    paused = false;
    setState(`Emulator stopped (${message.stopped})`, 'error');
    $('pauseFirmware').disabled = true;
    setOverlay('Firmware stopped', 'See the serial console for the emulator message.', true);
  }
}

function rejectWorkerRequests(error) {
  for (const pending of requests.values()) {
    clearTimeout(pending.timeout);
    pending.reject(error);
  }
  requests.clear();
}

function attachWorkerHandlers(instance) {
  instance.onmessage = (event) => handleWorkerMessage(event, instance);
  instance.onerror = (event) => {
    if (instance !== worker) return;
    event.preventDefault();
    const error = new Error(event.message || 'The WebAssembly emulator worker failed.');
    readyReject?.(error);
    readyResolve = null;
    readyReject = null;
    rejectWorkerRequests(error);
    running = false;
    paused = false;
    flashReady = false;
    refreshFlashControls();
    setState('Emulator worker failed', 'error');
    setOverlay('Emulator worker failed', error.message, true);
    addConsole(`[error] ${error.message}`);
  };
}

attachWorkerHandlers(worker);

function drawFrame(buffer) {
  const bytes = new Uint8Array(buffer);
  if (bytes.length < 5 || bytes[0] !== 1) return;
  const header = new DataView(buffer);
  const width = header.getUint16(1, true);
  const height = header.getUint16(3, true);
  const pixels = width * height;
  if (width < 1 || height < 1 || bytes.length < 5 + pixels * 2) return;
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
    $('frameInfo').textContent = `${width} × ${height}`;
    updateNativeScreenSize();
  }
  const rgba = context.createImageData(width, height);
  let target = 0;
  for (let source = 5; source < 5 + pixels * 2; source += 2) {
    const pixel = bytes[source] | (bytes[source + 1] << 8);
    const red = (pixel >> 11) & 31;
    const green = (pixel >> 5) & 63;
    const blue = pixel & 31;
    rgba.data[target++] = (red << 3) | (red >> 2);
    rgba.data[target++] = (green << 2) | (green >> 4);
    rgba.data[target++] = (blue << 3) | (blue >> 2);
    rgba.data[target++] = 255;
  }
  context.putImageData(rgba, 0, 0);
  $('screenOverlay').classList.add('hidden');
  $('captureScreen').disabled = false;
}

async function fetchBytes(path, label) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) throw new Error(`${label} could not be loaded (${response.status}).`);
  return response.arrayBuffer();
}

async function fetchOptionalBytes(path, label) {
  const response = await fetch(path, { cache: 'no-store' });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`${label} could not be loaded (${response.status}).`);
  return response.arrayBuffer();
}

async function fetchOptionalText(path, label) {
  const response = await fetch(path, { cache: 'no-store' });
  if (response.status === 404) return null;
  if (!response.ok) throw new Error(`${label} could not be loaded (${response.status}).`);
  return response.text();
}

async function fetchFlashGeneration() {
  const value = await fetchOptionalText('/api/flash-generation', 'Internal flash generation');
  if (value === null) throw new Error('The emulator server is out of date. Close the emulator and reopen its launcher to load the flash recovery update.');
  if (!/^\d+$/.test(value.trim())) throw new Error('The emulator returned an invalid flash generation.');
  return Number(value.trim());
}

async function firmwareFingerprint(image) {
  if (!crypto.subtle) throw new Error('This browser cannot fingerprint firmware for persistent flash. Use a current Edge or Chrome release.');
  const digest = await crypto.subtle.digest('SHA-256', image);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, '0')).join('');
}

async function loadPersistedCard() {
  cardImage = await fetchBytes('/api/sd-card', 'Persistent virtual MicroSD');
  try { cardVolume = new Fat16Volume(cardImage); }
  catch { cardVolume = null; }
  updateCardSummary('virtual-sd.img');
  renderFileManager();
}

function updateCardSummary(name) {
  const mb = cardImage ? cardImage.byteLength / (1024 * 1024) : 0;
  if (cardImage && !cardVolume) {
    try { cardVolume = new Fat16Volume(cardImage); }
    catch { cardVolume = null; }
  }
  const audioCount = cardVolume ? cardVolume.listDirectory(0).filter((entry) => !entry.isDirectory && /\.(MP3|FLAC|AAC|M4A|WAV|OGG|OPUS|AIFF)$/i.test(entry.name)).length : null;
  $('sdName').textContent = name;
  $('sdDetail').textContent = audioCount !== null
    ? `FAT16 · ${mb.toFixed(0)} MB · ${audioCount} root audio file${audioCount === 1 ? '' : 's'} · saved locally`
    : `Raw sector image · ${mb.toFixed(0)} MB · FAT16 preview unavailable`;
  $('sdCapacity').textContent = `${mb.toFixed(0)} MB`;
  $('sdTag').textContent = 'CARD READY';
  setFileManagerEnabled(!!cardVolume);
}

function setFileManagerEnabled(enabled) {
  for (const id of ['fileBack', 'uploadFiles', 'createFolder', 'downloadItem']) $(id).disabled = !enabled;
  $('fileBack').disabled = !enabled || folderStack.length === 0;
  $('downloadItem').disabled = !enabled || !selectedEntry || selectedEntry.isDirectory;
  $('deleteItem').disabled = !enabled || !selectedEntry;
}

function renderFileManager() {
  const list = $('fileList');
  list.replaceChildren();
  if (!cardVolume) {
    const empty = document.createElement('div');
    empty.className = 'file-empty';
    empty.textContent = 'File management needs a FAT16 card image.';
    list.append(empty);
    $('filePath').textContent = '/';
    setFileManagerEnabled(false);
    return;
  }
  const directory = folderStack.at(-1)?.cluster ?? 0;
  $('filePath').textContent = '/' + folderStack.map((folder) => folder.name).join('/');
  $('fileBack').disabled = folderStack.length === 0;
  selectedEntry = null;
  $('downloadItem').disabled = true;
  $('deleteItem').disabled = true;
  const entries = cardVolume.listDirectory(directory).sort((a, b) => Number(b.isDirectory) - Number(a.isDirectory) || a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
  for (const entry of entries) {
    const row = document.createElement('button');
    row.className = `file-row${entry.isDirectory ? ' folder' : ''}`;
    row.type = 'button';
    const icon = document.createElement('span');
    icon.className = 'file-kind';
    icon.textContent = entry.isDirectory ? '▰' : '♪';
    const name = document.createElement('span');
    name.className = 'file-name';
    name.textContent = entry.name;
    const size = document.createElement('small');
    size.textContent = entry.isDirectory ? 'folder' : `${(entry.size / 1024).toFixed(0)} KB`;
    row.append(icon, name, size);
    row.addEventListener('click', () => {
      for (const sibling of list.querySelectorAll('.file-row')) sibling.classList.remove('selected');
      row.classList.add('selected');
      selectedEntry = entry;
      $('downloadItem').disabled = entry.isDirectory;
      $('deleteItem').disabled = false;
    });
    row.addEventListener('dblclick', () => { if (entry.isDirectory) openFolder(entry); });
    list.append(row);
  }
  if (!entries.length) {
    const empty = document.createElement('div');
    empty.className = 'file-empty';
    empty.textContent = 'This folder is empty.';
    list.append(empty);
  }
  setFileManagerEnabled(true);
}

function openFolder(entry) {
  folderStack.push({ name: entry.name, cluster: entry.firstCluster });
  renderFileManager();
}

async function postStorage(path, data, options = {}) {
  const headers = { 'Content-Type': 'application/octet-stream' };
  if (path === '/api/flash-patches') headers['X-Flash-Generation'] = String(options.flashGeneration ?? flashGeneration);
  const response = await fetch(path, { method: 'POST', headers, body: data, cache: 'no-store' });
  if (!response.ok) throw new Error(`Storage save failed (${response.status}).`);
  if (path === '/api/flash-replace') {
    const generationHeader = response.headers.get('X-Flash-Generation');
    const generation = generationHeader === null ? NaN : Number(generationHeader);
    if (!Number.isSafeInteger(generation) || generation < 0) throw new Error('The emulator did not confirm the new flash generation.');
    flashGeneration = generation;
  }
}

async function exportSelectedPartition() {
  if (!flashReady || flashManagerBusy) return;
  const partition = selectedFlashPartition();
  flashManagerBusy = true;
  refreshFlashControls();
  try {
    await flushPersistentStorage();
    const result = await request('flash-save');
    if (!(result.flashImage instanceof ArrayBuffer) || result.flashImage.byteLength !== INTERNAL_FLASH_SIZE) {
      throw new Error('The emulator did not return a complete 16 MiB flash image.');
    }
    const data = result.flashImage.slice(partition.offset, partition.offset + partition.size);
    const extension = $('partitionFormat').value;
    downloadBlob(new Blob([data], { type: 'application/octet-stream' }), `lyra-${partition.name}.${extension}`);
    addConsole(`[flash] Exported ${partition.label} (${humanFlashSize(partition.size)}) as .${extension}.`);
    $('partitionDetail').textContent = `${partition.type} · ${humanFlashSize(partition.size)} · exported as .${extension}`;
  } catch (error) {
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  } finally {
    flashManagerBusy = false;
    refreshFlashControls();
  }
}

async function exportFullFlash() {
  if (!flashReady || flashManagerBusy) return;
  flashManagerBusy = true;
  refreshFlashControls();
  try {
    await flushPersistentStorage();
    const result = await request('flash-save');
    if (!(result.flashImage instanceof ArrayBuffer) || result.flashImage.byteLength !== INTERNAL_FLASH_SIZE) {
      throw new Error('The emulator did not return a complete 16 MiB flash image.');
    }
    const extension = $('partitionFormat').value;
    downloadBlob(new Blob([result.flashImage], { type: 'application/octet-stream' }), `lyra-internal-flash.${extension}`);
    addConsole(`[flash] Exported all 16 MiB of internal flash as .${extension}.`);
    $('partitionDetail').textContent = `Full 16 MiB flash exported as .${extension}`;
  } catch (error) {
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  } finally {
    flashManagerBusy = false;
    refreshFlashControls();
  }
}

async function mutateFlashAndRestart(description, mutate) {
  if (!flashReady || flashManagerBusy) return;
  const wasRunning = running;
  let workerPaused = false;
  let flashSaved = false;
  flashManagerBusy = true;
  refreshFlashControls();
  try {
    if (wasRunning) {
      if (pointerDown) {
        worker.postMessage({ op: 'text', data: JSON.stringify({ t: 'touch', ...lastTouch, down: '0' }) });
        pointerDown = false;
      }
      const reply = await request('pause');
      if (!reply.paused) throw new Error('The emulator could not pause before changing flash.');
      workerPaused = true;
      await flushPersistentStorage();
      running = false;
      paused = true;
      setState('Updating flash', 'paused');
      $('paceSpeed').textContent = 'Paused';
    }

    const result = await request('flash-save');
    if (!(result.flashImage instanceof ArrayBuffer) || result.flashImage.byteLength !== INTERNAL_FLASH_SIZE) {
      throw new Error('The emulator did not return a complete 16 MiB flash image.');
    }
    const flashImage = new Uint8Array(result.flashImage);
    await mutate(flashImage);
    await postStorage('/api/flash-replace', flashImage);
    flashSaved = true;
    addConsole(`[flash] ${description}; restarting firmware from the updated flash.`);
    await bootFirmware(firmwareBuffer, firmwareName);
    if (!running) throw new Error('Flash was saved, but the firmware did not restart. See the serial console.');
    $('partitionDetail').textContent = `${description} · firmware restarted`;
  } catch (error) {
    if (workerPaused && !flashSaved && wasRunning) {
      try {
        const reply = await request('resume');
        if (reply.resumed) {
          running = true;
          paused = false;
          setState('Firmware running', 'running');
          $('paceSpeed').textContent = 'Measuring…';
          addConsole('[emulator] Flash operation failed before saving; firmware resumed.');
        }
      } catch { }
    }
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  } finally {
    flashManagerBusy = false;
    refreshFlashControls();
  }
}

async function clearSelectedPartition() {
  const partition = selectedFlashPartition();
  if (!window.confirm(`Erase ${partition.label} (${humanFlashSize(partition.size)}) at ${formatFlashOffset(partition.offset)}? Firmware will restart.`)) return;
  await mutateFlashAndRestart(`Cleared ${partition.label}`, (flashImage) => {
    flashImage.fill(0xff, partition.offset, partition.offset + partition.size);
  });
}

async function createCleanEmulatorWorker() {
  const replacedError = new Error('The emulator worker was replaced during a full flash reset.');
  readyReject?.(replacedError);
  readyResolve = null;
  readyReject = null;
  worker.terminate();
  rejectWorkerRequests(replacedError);
  storageSyncPromise = null;
  storageErrorLogged = false;
  running = false;
  paused = false;
  pointerDown = false;

  ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  worker = new Worker('./wasm/worker.js', { type: 'module' });
  attachWorkerHandlers(worker);
  const workerReady = ready;
  try {
    const wasm = await fetchBytes('./wasm/esp32sim.wasm.gz', 'WebAssembly runtime');
    const wasmCopy = wasm.slice(0);
    worker.postMessage({ op: 'init', wasm: wasmCopy, frameAck: true }, [wasmCopy]);
    await workerReady;
  } catch (error) {
    readyReject?.(error);
    throw error;
  }
}

async function importSelectedPartition(file) {
  if (!file) return;
  const partition = selectedFlashPartition();
  try {
    const extension = file.name.toLowerCase().split('.').at(-1);
    if (extension !== 'bin' && extension !== 'img') throw new Error('Choose a raw partition .bin or .img file.');
    if (file.size !== partition.size) throw new Error(`${partition.label} requires an image of exactly ${humanFlashSize(partition.size)} (${partition.size} bytes).`);
    if (!window.confirm(`Replace ${partition.label} with “${file.name}”? Firmware will restart.`)) return;
    const partitionImage = new Uint8Array(await file.arrayBuffer());
    if (partitionImage.byteLength !== partition.size) throw new Error('The selected partition image changed size while it was being read.');
    await mutateFlashAndRestart(`Imported ${partition.label} from ${file.name}`, (flashImage) => {
      flashImage.set(partitionImage, partition.offset);
    });
  } catch (error) {
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  }
}

async function importFullFlash(file) {
  if (!file || !firmwareBuffer || !flashReady || flashManagerBusy) return;
  try {
    const extension = file.name.toLowerCase().split('.').at(-1);
    if (extension !== 'bin' && extension !== 'img') throw new Error('Choose a raw full-flash .bin or .img file.');
    if (file.size !== INTERNAL_FLASH_SIZE) throw new Error(`A full-flash image must be exactly 16 MiB (${INTERNAL_FLASH_SIZE} bytes).`);
    if (!window.confirm(`Replace all 16 MiB of internal flash with “${file.name}” and restart the firmware?`)) return;
    flashManagerBusy = true;
    refreshFlashControls();
    setState('Importing full flash');
    setOverlay('Importing full flash', 'Starting a clean emulator and restoring the 16 MiB image…');
    $('partitionDetail').textContent = `Importing ${file.name}…`;

    const flashImage = new Uint8Array(await file.arrayBuffer());
    if (flashImage.byteLength !== INTERNAL_FLASH_SIZE) throw new Error('The selected flash image changed size while it was being read.');
    const bootImage = firmwareBuffer;
    const bootName = firmwareName;
    await createCleanEmulatorWorker();
    await postStorage('/api/flash-replace', flashImage);
    addConsole(`[flash] Imported all 16 MiB from ${file.name}; restarting firmware.`);
    await bootFirmware(bootImage, bootName);
    if (!running) throw new Error('Flash was imported, but the firmware did not restart. See the serial console.');
    $('partitionDetail').textContent = `Full 16 MiB flash imported from ${file.name}.`;
  } catch (error) {
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  } finally {
    if (flashManagerBusy) {
      flashManagerBusy = false;
      refreshFlashControls();
    }
  }
}

async function resetFullFlash() {
  if (!firmwareBuffer || !flashReady || flashManagerBusy) return;
  const confirmed = window.confirm('Reset all 16 MiB of internal flash from the loaded merged firmware image? This erases NVS, OTA data and slots, and LittleFS. The firmware will start fresh.');
  if (!confirmed) return;
  flashManagerBusy = true;
  refreshFlashControls();
  setState('Resetting internal flash');
  setOverlay('Resetting internal flash', 'Starting a clean emulator and rebuilding all 16 MiB from the loaded firmware…');
  $('partitionDetail').textContent = 'Rebuilding a fresh 16 MiB flash image…';
  addConsole('[flash] Resetting all 16 MiB from the loaded firmware image.');
  try {
    const resetFirmware = firmwareBuffer;
    const resetFirmwareName = firmwareName;
    await createCleanEmulatorWorker();
    await bootFirmware(resetFirmware, resetFirmwareName, true);
    if (!running) throw new Error('Flash was reset, but the firmware did not restart. See the serial console.');
    $('partitionDetail').textContent = 'Fresh 16 MiB flash created from the loaded firmware image.';
    addConsole('[flash] Full flash reset complete; firmware started with erased persistent partitions.');
  } catch (error) {
    addConsole(`[flash error] ${error.message}`);
    $('partitionDetail').textContent = error.message;
  } finally {
    flashManagerBusy = false;
    refreshFlashControls();
  }
}

function updateFirmwareSummary(name, size) {
  $('firmwareName').textContent = name;
  $('firmwareDetail').textContent = `${(size / (1024 * 1024)).toFixed(2)} MB · full flash image`;
  $('firmwareTag').textContent = 'LOADED';
  $('firmwareTag').classList.add('ready');
  $('firmwareSummary').classList.add('ready');
  $('restartFirmware').disabled = false;
  $('pauseFirmware').disabled = false;
}

async function bootFirmware(image, name, reflash = false) {
  flashReady = false;
  refreshFlashControls();
  await flushPersistentStorage();
  firmwareBuffer = image;
  firmwareName = name;
  paused = false;
  $('pauseFirmware').textContent = 'Pause';
  $('pauseFirmware').disabled = true;
  updateFirmwareSummary(name, image.byteLength);
  running = false;
  setState('Loading firmware');
  setOverlay('Booting firmware', 'Loading the ROM, merged flash image, and virtual card…');
  addConsole(`\n[emulator] Loading ${name} (${(image.byteLength / 1048576).toFixed(2)} MiB)`);
  try {
    let reply = await request('create', { board: 'lyra-jc3248w535en', flash_mb: 16, psram_mb: 8, jit: true, smoothDisplay: false });
    if (!reply.created) throw new Error('Could not create the ESP32-S3 machine.');
    const romCopy = romImage.slice(0);
    reply = await request('load', { kind: 0, data: romCopy }, [romCopy]);
    if (!reply.ok) throw new Error('The ESP32-S3 mask ROM did not load.');
    const flashCopy = image.slice(0);
    reply = await request('load', { kind: 5, data: flashCopy }, [flashCopy]);
    if (!reply.ok) throw new Error('The merged .bin is not a valid ESP32-S3 flash image.');
    const imageHash = await firmwareFingerprint(image);
    const savedHash = reflash ? null : await fetchOptionalText('/api/firmware-hash', 'Persisted firmware fingerprint');
    if (savedHash === imageHash) {
      const savedFlash = await fetchOptionalBytes('/api/internal-flash', 'Persistent internal flash');
      if (!savedFlash || savedFlash.byteLength !== 16 * 1024 * 1024) throw new Error('The persisted internal flash image does not match the emulator’s 16 MB flash size.');
      const savedCopy = savedFlash.slice(0);
      reply = await request('load', { kind: 5, data: savedCopy }, [savedCopy]);
      if (!reply.ok) throw new Error('Could not restore the persisted internal flash image.');
      addConsole('[emulator] Restored persisted internal flash, including NVS and OTA partitions.');
    } else {
      const initialFlash = await request('flash-save');
      if (!(initialFlash.flashImage instanceof ArrayBuffer) || initialFlash.flashImage.byteLength !== 16 * 1024 * 1024) throw new Error('Could not initialize persistent internal flash.');
      await postStorage('/api/flash-replace', initialFlash.flashImage);
      await postStorage('/api/firmware-hash', imageHash);
      addConsole(savedHash || reflash
        ? '[emulator] Firmware changed; initialized a fresh persistent flash image.'
        : '[emulator] Created persistent 16 MB internal flash.');
    }
    await request('flash-clear-dirty');
    const cardCopy = cardImage.slice(0);
    reply = await request('sd-card', { data: cardCopy }, [cardCopy]);
    if (!reply.sdCardReady) throw new Error(reply.error || 'Could not insert the FAT16 card image.');
    $('saveCard').disabled = false;
    $('sdTag').textContent = 'INSERTED';
    reply = await request('start', { appDirect: false });
    if (!reply.started) throw new Error('The firmware could not start. See the serial console.');
    running = true;
    setState('Firmware running', 'running');
    $('pauseFirmware').disabled = false;
    flashReady = true;
    refreshFlashControls();
    addConsole('[emulator] Firmware is running.');
  } catch (error) {
    running = false;
    paused = false;
    $('pauseFirmware').disabled = true;
    flashReady = false;
    refreshFlashControls();
    setState('Firmware needs attention', 'error');
    setOverlay('Could not start firmware', error.message, true);
    addConsole(`[error] ${error.message}`);
  }
}

function touch(event, down) {
  if (!running || paused) return;
  const bounds = canvas.getBoundingClientRect();
  const x = Math.max(0, Math.min(canvas.width - 1, Math.floor((event.clientX - bounds.left) * canvas.width / bounds.width)));
  const y = Math.max(0, Math.min(canvas.height - 1, Math.floor((event.clientY - bounds.top) * canvas.height / bounds.height)));
  lastTouch = { x, y };
  worker.postMessage({ op: 'text', data: JSON.stringify({ t: 'touch', x, y, down: down ? '1' : '0' }) });
}

async function readFirmwareFile(file) {
  try {
    if (!file || !file.name.toLowerCase().endsWith('.bin')) throw new Error('Choose a merged firmware .bin file.');
    const image = await file.arrayBuffer();
    await bootFirmware(image, file.name, true);
  } catch (error) {
    setState('Firmware needs attention', 'error');
    setOverlay('Firmware image unavailable', error.message, true);
    addConsole(`[error] ${error.message}`);
  }
}

async function readCardFile(file) {
  try {
    if (!file) return;
    const image = await file.arrayBuffer();
    if (image.byteLength < 512 || image.byteLength % 512 !== 0) throw new Error('The card image size must be a non-empty multiple of 512 bytes.');
    await flushPersistentStorage();
    await postStorage('/api/sd-replace', image);
    cardImage = image;
    cardVolume = null;
    folderStack = [];
    selectedEntry = null;
    try { cardVolume = new Fat16Volume(cardImage); } catch { }
    updateCardSummary(file.name);
    renderFileManager();
    if (running) {
      const copy = image.slice(0);
      const reply = await request('sd-card', { data: copy }, [copy]);
      if (!reply.sdCardReady) throw new Error(reply.error || 'Could not insert card image.');
      $('sdTag').textContent = 'RESTART NEEDED';
      addConsole(`[emulator] Inserted ${file.name}. Restart firmware to remount it.`);
    }
  } catch (error) {
    addConsole(`[card error] ${error.message}`);
    $('sdDetail').textContent = error.message;
  }
}

async function saveCardImage() {
  try {
    const result = await request('sd-card-save');
    if (!(result.sdCardImage instanceof ArrayBuffer) || result.sdCardImage.byteLength === 0) throw new Error('The emulator has no inserted card image.');
    const blob = new Blob([result.sdCardImage], { type: 'application/octet-stream' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = 'lyra-virtual-sd.img';
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  } catch (error) { addConsole(`[card error] ${error.message}`); }
}

async function commitCardEdits() {
  if (!cardVolume?.dirtySectors.size) return;
  const patches = cardVolume.takePatches();
  await postStorage('/api/sd-patches', patches);
  if (running) {
    const copy = patches.slice(0);
    const reply = await request('sd-card-patch', { data: copy }, [copy]);
    if (!reply.patched) throw new Error('Could not update the inserted MicroSD card. Restart the firmware and try again.');
    $('sdTag').textContent = 'RESTART NEEDED';
  }
  cardVolume.clearPatches();
  updateCardSummary($('sdName').textContent);
  renderFileManager();
  addConsole('[emulator] MicroSD file changes saved to the emulator folder.');
}

async function uploadCardFiles(files) {
  if (!cardVolume || !files?.length) return;
  try {
    await flushPersistentStorage();
    for (const file of files) cardVolume.createFile(folderStack.at(-1)?.cluster ?? 0, file.name, new Uint8Array(await file.arrayBuffer()));
    await commitCardEdits();
  } catch (error) {
    try { await commitCardEdits(); } catch (saveError) { addConsole(`[card error] ${saveError.message}`); }
    addConsole(`[card error] ${error.message}`);
    $('sdDetail').textContent = error.message;
  }
}

async function createCardFolder() {
  if (!cardVolume) return;
  const name = window.prompt('Folder name');
  if (name === null) return;
  try {
    await flushPersistentStorage();
    cardVolume.createDirectory(folderStack.at(-1)?.cluster ?? 0, name.trim());
    await commitCardEdits();
  } catch (error) {
    addConsole(`[card error] ${error.message}`);
    $('sdDetail').textContent = error.message;
  }
}

async function deleteCardEntry() {
  if (!cardVolume || !selectedEntry) return;
  const entry = selectedEntry;
  const directory = folderStack.at(-1)?.cluster ?? 0;
  const contents = entry.isDirectory ? ' and everything inside it' : '';
  if (!window.confirm(`Delete “${entry.name}”${contents} from the virtual MicroSD?`)) return;
  try {
    await flushPersistentStorage();
    const currentEntry = cardVolume.listDirectory(directory).find((item) => item.name.toLocaleLowerCase() === entry.name.toLocaleLowerCase());
    if (!currentEntry) throw new Error(`“${entry.name}” is no longer in this folder.`);
    cardVolume.deleteEntry(directory, currentEntry);
    await commitCardEdits();
  } catch (error) {
    addConsole(`[card error] ${error.message}`);
    $('sdDetail').textContent = error.message;
  }
}

function downloadCardEntry() {
  if (!cardVolume || !selectedEntry || selectedEntry.isDirectory) return;
  try {
    const blob = new Blob([cardVolume.readFile(selectedEntry)], { type: 'application/octet-stream' });
    const link = document.createElement('a');
    link.href = URL.createObjectURL(blob);
    link.download = selectedEntry.name;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  } catch (error) {
    addConsole(`[card error] ${error.message}`);
  }
}

function acknowledgementRecords(patches, recordSize) {
  if (!(patches instanceof ArrayBuffer)) return null;
  const count = patches.byteLength / recordSize;
  if (!Number.isInteger(count)) throw new Error('The emulator returned an invalid storage update.');
  const source = new Uint8Array(patches);
  const result = new Uint8Array(count * 8);
  for (let index = 0; index < count; index++) result.set(source.subarray(index * recordSize, index * recordSize + 8), index * 8);
  return result.buffer;
}

function flushPersistentStorage() {
  if (!running) return Promise.resolve();
  if (storageSyncPromise) return storageSyncPromise;
  const syncFlashGeneration = flashGeneration;
  const sync = (async () => {
    try {
      const dirty = await request('storage-dirty');
      let sdRecords = null;
      let flashRecords = null;
      if (dirty.sdCardPatches instanceof ArrayBuffer) {
        const patches = new Uint8Array(dirty.sdCardPatches);
        const count = patches.byteLength / 520;
        if (!Number.isInteger(count)) throw new Error('The emulator returned invalid MicroSD sectors.');
        const currentCard = new Uint8Array(cardImage);
        const view = new DataView(dirty.sdCardPatches);
        for (let index = 0; index < count; index++) {
          const offset = index * 520;
          const sector = view.getUint32(offset, true);
          const cardOffset = sector * 512;
          if (cardOffset + 512 > currentCard.length) throw new Error('The firmware wrote outside the virtual MicroSD image.');
          currentCard.set(patches.subarray(offset + 8, offset + 520), cardOffset);
        }
        await postStorage('/api/sd-patches', dirty.sdCardPatches);
        sdRecords = acknowledgementRecords(dirty.sdCardPatches, 520);
        updateCardSummary($('sdName').textContent);
        renderFileManager();
      }
      if (dirty.flashPatches instanceof ArrayBuffer) {
        await postStorage('/api/flash-patches', dirty.flashPatches, { flashGeneration: syncFlashGeneration });
        flashRecords = acknowledgementRecords(dirty.flashPatches, 4104);
      }
      if (sdRecords || flashRecords) {
        const transfer = [sdRecords, flashRecords].filter(Boolean);
        await request('storage-ack', { sdRecords, flashRecords }, transfer);
        storageErrorLogged = false;
      }
    } catch (error) {
      if (flashGeneration === syncFlashGeneration) {
        if (!storageErrorLogged) addConsole(`[storage] ${error.message}; pending writes will be retried.`);
        storageErrorLogged = true;
      }
      throw error;
    }
  })();
  const tracked = sync.finally(() => {
    if (storageSyncPromise === tracked) storageSyncPromise = null;
  });
  storageSyncPromise = tracked;
  return storageSyncPromise;
}

let romImage;
async function initialize() {
  try {
    setState('Loading emulator engine');
    const [wasm, rom, firmware, generation] = await Promise.all([
      fetchBytes('./wasm/esp32sim.wasm.gz', 'WebAssembly runtime'),
      fetchBytes('./rom/esp32s3_rev0_rom.elf', 'ESP32-S3 mask ROM'),
      fetchBytes('/firmware.bin', 'Firmware image').catch((error) => { addConsole(`[firmware] ${error.message} Select a .bin to continue.`); return null; }),
      fetchFlashGeneration(),
      loadPersistedCard(),
    ]);
    romImage = rom;
    flashGeneration = generation;
    worker.postMessage({ op: 'init', wasm, frameAck: true }, [wasm]);
    await ready;
    setState('Emulator ready', 'running');
    if (firmware) await bootFirmware(firmware, new URLSearchParams(location.search).get('name') || 'lyra_firmware_merged.bin');
    else {
      setOverlay('Choose a firmware .bin', 'Open the merged ESP32-S3 flash image to run its GUI.');
      $('firmwareDetail').textContent = 'Waiting for a merged flash image';
      $('firmwareTag').textContent = 'NO IMAGE';
      $('firmwareTag').classList.remove('ready');
      $('chooseFirmware').focus();
    }
  } catch (error) {
    setState('Emulator failed to load', 'error');
    setOverlay('Emulator unavailable', error.message, true);
    addConsole(`[error] ${error.message}`);
  }
}

$('chooseFirmware').addEventListener('click', () => $('firmwarePicker').click());
$('partitionSelect').addEventListener('change', updatePartitionDetail);
$('exportPartition').addEventListener('click', exportSelectedPartition);
$('importPartition').addEventListener('click', () => $('partitionPicker').click());
$('partitionPicker').addEventListener('change', async (event) => {
  await importSelectedPartition(event.target.files?.[0]);
  event.target.value = '';
});
$('exportFullFlash').addEventListener('click', exportFullFlash);
$('importFullFlash').addEventListener('click', () => $('fullFlashPicker').click());
$('fullFlashPicker').addEventListener('change', async (event) => {
  await importFullFlash(event.target.files?.[0]);
  event.target.value = '';
});
$('clearPartition').addEventListener('click', clearSelectedPartition);
$('resetFlash').addEventListener('click', resetFullFlash);
$('fitScale').addEventListener('click', () => setPreviewScale('fit'));
$('nativeScale').addEventListener('click', () => setPreviewScale('1x'));
$('captureScreen').addEventListener('click', captureScreen);
$('saveConsole').addEventListener('click', saveConsoleLog);
$('firmwarePicker').addEventListener('change', (event) => readFirmwareFile(event.target.files?.[0]));
$('pauseFirmware').addEventListener('click', async () => {
  if (!running) return;
  const button = $('pauseFirmware');
  button.disabled = true;
  try {
    if (paused) {
      const reply = await request('resume');
      if (!reply.resumed) throw new Error('The emulator could not resume.');
      paused = false;
      button.textContent = 'Pause';
      $('paceSpeed').textContent = 'Measuring…';
      setState('Firmware running', 'running');
      addConsole('[emulator] Firmware resumed.');
    } else {
      if (pointerDown) {
        worker.postMessage({ op: 'text', data: JSON.stringify({ t: 'touch', ...lastTouch, down: '0' }) });
        pointerDown = false;
      }
      await flushPersistentStorage();
      const reply = await request('pause');
      if (!reply.paused) throw new Error('The emulator could not pause.');
      paused = true;
      button.textContent = 'Resume';
      $('paceSpeed').textContent = 'Paused';
      setState('Firmware paused', 'paused');
      addConsole('[emulator] Firmware paused; emulated state is retained.');
    }
  } catch (error) {
    addConsole(`[emulator] Could not ${paused ? 'resume' : 'pause'} firmware: ${error.message}`);
  } finally {
    button.disabled = !running;
  }
});
$('restartFirmware').addEventListener('click', async () => {
  if (!firmwareBuffer) return;
  $('pauseFirmware').disabled = true;
  try {
    setState('Restarting firmware', 'running');
    setOverlay('Restarting firmware', 'Keeping emulated flash, NVS, and MicroSD contents…');
    await flushPersistentStorage();
    addConsole('[emulator] Restart requested; emulated flash, NVS, and MicroSD contents are retained.');
    const reply = await request('restart');
    if (!reply.restarted) throw new Error('The ESP32-S3 restart was not accepted.');
    running = true;
    paused = false;
    $('pauseFirmware').textContent = 'Pause';
    $('pauseFirmware').disabled = false;
    setState('Firmware running', 'running');
  } catch (error) {
    $('pauseFirmware').disabled = !running;
    setState('Firmware needs attention', 'error');
    setOverlay('Could not restart firmware', error.message, true);
    addConsole(`[error] ${error.message}`);
  }
});
$('manageCard').addEventListener('click', () => $('sdModal').showModal());
$('closeCardManager').addEventListener('click', () => $('sdModal').close());
$('sdModal').addEventListener('click', (event) => {
  if (event.target === $('sdModal')) $('sdModal').close();
});
$('manageFlash').addEventListener('click', () => $('flashModal').showModal());
$('closeFlashManager').addEventListener('click', () => $('flashModal').close());
$('flashModal').addEventListener('click', (event) => {
  if (event.target === $('flashModal')) $('flashModal').close();
});
$('chooseCard').addEventListener('click', () => $('cardPicker').click());
$('cardPicker').addEventListener('change', (event) => readCardFile(event.target.files?.[0]));
$('saveCard').addEventListener('click', saveCardImage);
$('fileBack').addEventListener('click', () => { if (folderStack.length) { folderStack.pop(); renderFileManager(); } });
$('uploadFiles').addEventListener('click', () => $('filesPicker').click());
$('filesPicker').addEventListener('change', async (event) => {
  await uploadCardFiles([...(event.target.files ?? [])]);
  event.target.value = '';
});
$('createFolder').addEventListener('click', createCardFolder);
$('downloadItem').addEventListener('click', downloadCardEntry);
$('deleteItem').addEventListener('click', deleteCardEntry);
$('clearConsole').addEventListener('click', () => { consoleLines = []; $('console').textContent = ''; });

canvas.addEventListener('pointerdown', (event) => {
  if (!running || paused) { event.preventDefault(); return; }
  pointerDown = true;
  canvas.setPointerCapture(event.pointerId);
  touch(event, true);
  event.preventDefault();
});
canvas.addEventListener('pointermove', (event) => { if (pointerDown) touch(event, true); });
for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
  canvas.addEventListener(type, (event) => {
    if (pointerDown) touch(event, false);
    pointerDown = false;
  });
}

const dropTarget = $('firmwareSummary');
dropTarget.addEventListener('dragover', (event) => { event.preventDefault(); dropTarget.classList.add('drag'); });
dropTarget.addEventListener('dragleave', () => dropTarget.classList.remove('drag'));
dropTarget.addEventListener('drop', (event) => {
  event.preventDefault();
  dropTarget.classList.remove('drag');
  readFirmwareFile(event.dataTransfer.files?.[0]);
});
setInterval(() => { flushPersistentStorage().catch(() => {}); }, 2000);

let initialScale = 'fit';
try { if (localStorage.getItem('lyra-preview-scale') === '1x') initialScale = '1x'; } catch { }
initializePartitionControls();
setPreviewScale(initialScale);
window.addEventListener('resize', updateNativeScreenSize);
window.visualViewport?.addEventListener('resize', updateNativeScreenSize);
initialize();
