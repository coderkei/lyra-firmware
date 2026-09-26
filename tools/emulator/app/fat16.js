const SECTOR_SIZE = 512;
const ENTRY_SIZE = 32;
const RECORD_SIZE = 520;
const EOC = 0xffff;
const LFN_OFFSETS = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30];

export class Fat16Volume {
  constructor(buffer) {
    this.buffer = buffer;
    this.bytes = new Uint8Array(buffer);
    this.view = new DataView(buffer);
    this.dirtySectors = new Set();
    const b = this.view;
    this.bytesPerSector = b.getUint16(11, true);
    this.sectorsPerCluster = b.getUint8(13);
    this.reservedSectors = b.getUint16(14, true);
    this.fatCount = b.getUint8(16);
    this.rootEntryCount = b.getUint16(17, true);
    this.totalSectors = b.getUint16(19, true) || b.getUint32(32, true);
    this.sectorsPerFat = b.getUint16(22, true);
    if (this.bytesPerSector !== SECTOR_SIZE || !this.sectorsPerCluster || !this.reservedSectors || !this.fatCount || !this.rootEntryCount || !this.sectorsPerFat) {
      throw new Error('The inserted image does not contain a supported FAT16 filesystem.');
    }
    this.fatStart = this.reservedSectors * SECTOR_SIZE;
    this.fatBytes = this.sectorsPerFat * SECTOR_SIZE;
    this.rootStartSector = this.reservedSectors + this.fatCount * this.sectorsPerFat;
    this.rootStart = this.rootStartSector * SECTOR_SIZE;
    this.rootBytes = this.rootEntryCount * ENTRY_SIZE;
    this.rootSectors = Math.ceil(this.rootBytes / SECTOR_SIZE);
    this.dataStartSector = this.rootStartSector + this.rootSectors;
    this.clusterBytes = this.sectorsPerCluster * SECTOR_SIZE;
    this.clusterCount = Math.floor((this.totalSectors - this.dataStartSector) / this.sectorsPerCluster);
    if (this.totalSectors * SECTOR_SIZE > this.bytes.length || this.clusterCount < 4085 || this.clusterCount >= 65525 || this.fatBytes < (this.clusterCount + 2) * 2) {
      throw new Error('The inserted image has invalid FAT16 geometry.');
    }
  }

  getFat(cluster) { return this.view.getUint16(this.fatStart + cluster * 2, true); }

  setFat(cluster, value) {
    for (let copy = 0; copy < this.fatCount; copy++) {
      const offset = this.fatStart + copy * this.fatBytes + cluster * 2;
      this.view.setUint16(offset, value, true);
      this.mark(offset, 2);
    }
  }

  clusterOffset(cluster) { return this.dataStartSector * SECTOR_SIZE + (cluster - 2) * this.clusterBytes; }

  clusterChain(first) {
    if (!first) return [];
    const chain = [];
    const seen = new Set();
    let cluster = first;
    while (cluster >= 2 && cluster < this.clusterCount + 2 && !seen.has(cluster)) {
      chain.push(cluster);
      seen.add(cluster);
      const next = this.getFat(cluster);
      if (next >= 0xfff8) return chain;
      if (next < 2 || next >= 0xfff0) break;
      cluster = next;
    }
    throw new Error('The FAT16 image contains a broken or looping cluster chain.');
  }

  directorySlots(directoryCluster) {
    if (!directoryCluster) {
      const slots = [];
      for (let offset = this.rootStart; offset < this.rootStart + this.rootBytes; offset += ENTRY_SIZE) slots.push(offset);
      return slots;
    }
    return this.clusterChain(directoryCluster).flatMap((cluster) => {
      const slots = [];
      const start = this.clusterOffset(cluster);
      for (let offset = start; offset < start + this.clusterBytes; offset += ENTRY_SIZE) slots.push(offset);
      return slots;
    });
  }

  listDirectory(directoryCluster = 0) {
    const result = [];
    const longEntries = [];
    for (const offset of this.directorySlots(directoryCluster)) {
      const first = this.bytes[offset];
      if (first === 0) break;
      if (first === 0xe5) { longEntries.length = 0; continue; }
      const attributes = this.bytes[offset + 11];
      if (attributes === 0x0f) { longEntries.push(offset); continue; }
      const shortBytes = this.bytes.slice(offset, offset + 11);
      const shortName = decodeShortName(shortBytes);
      if (!shortName || shortName === '.' || shortName === '..' || (attributes & 0x08)) { longEntries.length = 0; continue; }
      const validLongName = readLongName(this.view, longEntries, shortBytes);
      const name = validLongName || shortName;
      const firstCluster = this.view.getUint16(offset + 26, true) | (this.view.getUint16(offset + 20, true) << 16);
      result.push({
        name,
        shortName,
        attributes,
        isDirectory: (attributes & 0x10) !== 0,
        size: this.view.getUint32(offset + 28, true),
        firstCluster,
        offset,
        longOffsets: validLongName ? [...longEntries] : [],
      });
      longEntries.length = 0;
    }
    return result;
  }

  createFile(directoryCluster, name, content) {
    const payload = content instanceof Uint8Array ? content : new Uint8Array(content);
    const alias = this.makeShortAlias(directoryCluster, name);
    const existing = this.listDirectory(directoryCluster);
    if (existing.some((entry) => entry.name.toLocaleLowerCase() === name.toLocaleLowerCase())) throw new Error(`“${name}” already exists in this folder.`);
    const clusters = this.allocate(payload.length ? Math.ceil(payload.length / this.clusterBytes) : 0);
    try {
      this.linkClusters(clusters);
      this.writeFileData(clusters, payload);
      this.writeEntry(directoryCluster, name, alias, 0x20, clusters[0] || 0, payload.length);
    } catch (error) {
      this.release(clusters);
      throw error;
    }
  }

  createDirectory(directoryCluster, name) {
    const alias = this.makeShortAlias(directoryCluster, name);
    const existing = this.listDirectory(directoryCluster);
    if (existing.some((entry) => entry.name.toLocaleLowerCase() === name.toLocaleLowerCase())) throw new Error(`“${name}” already exists in this folder.`);
    const [cluster] = this.allocate(1);
    try {
      this.linkClusters([cluster]);
      const start = this.clusterOffset(cluster);
      this.writeBytes(start, new Uint8Array(this.clusterBytes));
      writeDotEntry(this, start, '.', cluster);
      writeDotEntry(this, start + ENTRY_SIZE, '..', directoryCluster);
      this.writeEntry(directoryCluster, name, alias, 0x10, cluster, 0);
    } catch (error) {
      this.release([cluster]);
      throw error;
    }
  }

  readFile(entry) {
    if (entry.isDirectory) throw new Error('Select a file to download.');
    const content = new Uint8Array(entry.size);
    let copied = 0;
    for (const cluster of this.clusterChain(entry.firstCluster)) {
      const length = Math.min(this.clusterBytes, content.length - copied);
      if (length <= 0) break;
      const offset = this.clusterOffset(cluster);
      content.set(this.bytes.subarray(offset, offset + length), copied);
      copied += length;
    }
    if (copied !== entry.size) throw new Error(`“${entry.name}” has an incomplete FAT16 cluster chain.`);
    return content;
  }

  deleteEntry(directoryCluster, entry) {
    if (entry.isDirectory) {
      for (const child of this.listDirectory(entry.firstCluster)) this.deleteEntry(entry.firstCluster, child);
    }
    this.release(this.clusterChain(entry.firstCluster));
    for (const offset of [...entry.longOffsets, entry.offset]) {
      this.bytes[offset] = 0xe5;
      this.mark(offset, 1);
    }
  }

  makeShortAlias(directoryCluster, name) {
    validateName(name);
    const existing = new Set(this.listDirectory(directoryCluster).map((entry) => entry.shortName.toUpperCase()));
    const dot = name.lastIndexOf('.');
    const rawBase = dot > 0 ? name.slice(0, dot) : name;
    const rawExt = dot > 0 ? name.slice(dot + 1) : '';
    const base = cleanShortPart(rawBase);
    const ext = cleanShortPart(rawExt).slice(0, 3);
    const direct = base.length <= 8 && ext.length <= 3 && /^[A-Z0-9_$!#%&'()@^`{}~-]+$/.test(rawBase.toUpperCase()) && (!rawExt || /^[A-Z0-9_$!#%&'()@^`{}~-]+$/.test(rawExt.toUpperCase())) && name === name.toUpperCase();
    if (direct) {
      const alias = `${base.padEnd(8, ' ')}${ext.padEnd(3, ' ')}`;
      if (!existing.has(decodeShortName(new TextEncoder().encode(alias)))) return alias;
    }
    const stem = base || 'FILE';
    for (let index = 1; index < 1_000_000; index++) {
      const suffix = `~${index}`;
      const candidate = `${stem.slice(0, 8 - suffix.length)}${suffix}`.padEnd(8, ' ') + ext.padEnd(3, ' ');
      if (!existing.has(decodeShortName(new TextEncoder().encode(candidate)))) return candidate;
    }
    throw new Error('Could not create a unique FAT16 filename.');
  }

  writeEntry(directoryCluster, name, shortName, attributes, firstCluster, size) {
    const shortBytes = new TextEncoder().encode(shortName);
    const needsLongName = name !== decodeShortName(shortBytes);
    const longEntries = needsLongName ? encodeLongEntries(name, shortBytes) : [];
    const offsets = this.findFreeSlots(directoryCluster, longEntries.length + 1);
    for (let index = 0; index < longEntries.length; index++) this.writeBytes(offsets[index], longEntries[index]);
    const entry = new Uint8Array(ENTRY_SIZE);
    entry.set(shortBytes.subarray(0, 11), 0);
    entry[11] = attributes;
    new DataView(entry.buffer).setUint16(26, firstCluster & 0xffff, true);
    new DataView(entry.buffer).setUint16(20, firstCluster >>> 16, true);
    new DataView(entry.buffer).setUint32(28, size, true);
    this.writeBytes(offsets[offsets.length - 1], entry);
    const next = this.directorySlots(directoryCluster)[this.directorySlots(directoryCluster).indexOf(offsets.at(-1)) + 1];
    if (next !== undefined && (this.bytes[next] === 0 || this.bytes[next] === 0xe5)) {
      this.bytes[next] = 0;
      this.mark(next, 1);
    }
  }

  findFreeSlots(directoryCluster, count) {
    let slots = this.directorySlots(directoryCluster);
    while (true) {
      let run = [];
      for (const offset of slots) {
        if (this.bytes[offset] === 0 || this.bytes[offset] === 0xe5) run.push(offset);
        else run = [];
        if (run.length === count) return run;
      }
      if (!directoryCluster) throw new Error('This FAT16 root directory is full.');
      const chain = this.clusterChain(directoryCluster);
      const [extra] = this.allocate(1);
      this.setFat(chain.at(-1), extra);
      this.setFat(extra, EOC);
      this.writeBytes(this.clusterOffset(extra), new Uint8Array(this.clusterBytes));
      slots = this.directorySlots(directoryCluster);
    }
  }

  allocate(count) {
    if (!count) return [];
    const clusters = [];
    for (let cluster = 2; cluster < this.clusterCount + 2 && clusters.length < count; cluster++) {
      if (this.getFat(cluster) === 0) clusters.push(cluster);
    }
    if (clusters.length !== count) throw new Error('The virtual MicroSD does not have enough free space.');
    return clusters;
  }

  linkClusters(clusters) {
    for (let index = 0; index < clusters.length; index++) this.setFat(clusters[index], clusters[index + 1] || EOC);
  }

  writeFileData(clusters, payload) {
    let copied = 0;
    for (const cluster of clusters) {
      const offset = this.clusterOffset(cluster);
      const chunk = new Uint8Array(this.clusterBytes);
      const length = Math.min(chunk.length, payload.length - copied);
      if (length > 0) chunk.set(payload.subarray(copied, copied + length));
      this.writeBytes(offset, chunk);
      copied += length;
    }
  }

  release(clusters) { for (const cluster of clusters) this.setFat(cluster, 0); }

  writeBytes(offset, data) {
    this.bytes.set(data, offset);
    this.mark(offset, data.length);
  }

  mark(offset, length) {
    if (!length) return;
    const first = Math.floor(offset / SECTOR_SIZE);
    const last = Math.floor((offset + length - 1) / SECTOR_SIZE);
    for (let sector = first; sector <= last; sector++) this.dirtySectors.add(sector);
  }

  takePatches() {
    const sectors = [...this.dirtySectors].sort((a, b) => a - b);
    const patches = new Uint8Array(sectors.length * RECORD_SIZE);
    for (let index = 0; index < sectors.length; index++) {
      const sector = sectors[index];
      const offset = index * RECORD_SIZE;
      const view = new DataView(patches.buffer, offset, RECORD_SIZE);
      view.setUint32(0, sector, true);
      view.setUint32(4, 0, true);
      patches.set(this.bytes.subarray(sector * SECTOR_SIZE, (sector + 1) * SECTOR_SIZE), offset + 8);
    }
    return patches.buffer;
  }

  clearPatches() { this.dirtySectors.clear(); }
}

function validateName(name) {
  if (!name || name.length > 255 || /[\\/:*?"<>|\u0000-\u001f]/.test(name) || name === '.' || name === '..' || name.endsWith('.') || name.endsWith(' ')) {
    throw new Error('Use a valid FAT filename (up to 255 characters, without \\/:*?"<>|).');
  }
  if (new TextEncoder().encode(name).length > 765) throw new Error('This filename is too long for FAT16.');
}

function cleanShortPart(part) { return part.toUpperCase().replace(/[^A-Z0-9_$!#%&'()@^`{}~-]/g, '_'); }

function decodeShortName(bytes) {
  const decode = (start, end) => String.fromCharCode(...bytes.subarray(start, end)).trimEnd();
  const base = decode(0, 8);
  const extension = decode(8, 11);
  if (!base) return '';
  return extension ? `${base}.${extension}` : base;
}

function shortChecksum(shortBytes) {
  let sum = 0;
  for (const byte of shortBytes) sum = ((sum & 1) << 7) + (sum >> 1) + byte & 0xff;
  return sum;
}

function readLongName(view, offsets, shortBytes) {
  if (!offsets.length || offsets.some((offset) => view.getUint8(offset + 13) !== shortChecksum(shortBytes))) return null;
  const chunks = new Map();
  for (const offset of offsets) {
    const ordinal = view.getUint8(offset) & 0x1f;
    if (!ordinal) return null;
    let text = '';
    for (const relative of LFN_OFFSETS) {
      const unit = view.getUint16(offset + relative, true);
      if (unit === 0 || unit === 0xffff) break;
      text += String.fromCharCode(unit);
    }
    chunks.set(ordinal, text);
  }
  const total = Math.max(...chunks.keys());
  if (chunks.size !== total) return null;
  return Array.from({ length: total }, (_, index) => chunks.get(index + 1) || '').join('') || null;
}

function encodeLongEntries(name, shortBytes) {
  const units = Array.from({ length: name.length }, (_, index) => name.charCodeAt(index));
  units.push(0);
  while (units.length % 13) units.push(0xffff);
  const count = units.length / 13;
  const entries = [];
  for (let ordinal = count; ordinal >= 1; ordinal--) {
    const entry = new Uint8Array(ENTRY_SIZE);
    entry.fill(0xff);
    entry[0] = ordinal | (ordinal === count ? 0x40 : 0);
    entry[11] = 0x0f;
    entry[12] = 0;
    entry[13] = shortChecksum(shortBytes);
    entry[26] = 0;
    entry[27] = 0;
    const chunk = units.slice((ordinal - 1) * 13, ordinal * 13);
    for (let index = 0; index < LFN_OFFSETS.length; index++) new DataView(entry.buffer).setUint16(LFN_OFFSETS[index], chunk[index], true);
    entries.push(entry);
  }
  return entries;
}

function writeDotEntry(volume, offset, name, cluster) {
  const entry = new Uint8Array(ENTRY_SIZE);
  entry.fill(0x20, 0, 11);
  entry[0] = 0x2e;
  if (name === '..') entry[1] = 0x2e;
  entry[11] = 0x10;
  new DataView(entry.buffer).setUint16(26, cluster, true);
  volume.writeBytes(offset, entry);
}
