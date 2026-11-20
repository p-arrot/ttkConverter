// OpenDoc exports DOCX as a single-volume ZIP32 archive with stored entries.
// Keep its media intact and repair forbidden XML characters before publishing.
const localSignature = 0x04034b50;
const centralSignature = 0x02014b50;
const endSignature = 0x06054b50;
const endHeaderSize = 22;
const centralHeaderSize = 46;
const localHeaderSize = 30;

function requireValid(condition, reason) {
    if (!condition) throw new Error('Invalid OpenDoc DOCX: ' + reason);
}

function archiveDirectory(view) {
    const archiveEnd = view.byteLength;
    const earliest = Math.max(0, archiveEnd - endHeaderSize - 0xffff);
    for (let end = archiveEnd - endHeaderSize; end >= earliest; --end) {
        if (view.getUint32(end, true) !== endSignature ||
            end + endHeaderSize + view.getUint16(end + 20, true) !== archiveEnd) continue;
        const count = view.getUint16(end + 10, true);
        const length = view.getUint32(end + 12, true);
        const offset = view.getUint32(end + 16, true);
        requireValid(view.getUint16(end + 4, true) === 0 && view.getUint16(end + 6, true) === 0 &&
            view.getUint16(end + 8, true) === count && count > 0 && count !== 0xffff &&
            offset + length === end && count * centralHeaderSize <= length,
        'unsupported or incomplete central directory');
        return {offset, end, count};
    }
    throw new Error('Invalid OpenDoc DOCX: ZIP directory is missing');
}

function entryName(bytes, start, length) {
    let name = '';
    for (let index = start; index < start + length; ++index) name += String.fromCharCode(bytes[index]);
    return name;
}

function readEntries(bytes, view, directory) {
    const names = new Set();
    const entries = [];
    let central = directory.offset;
    for (let index = 0; index < directory.count; ++index) {
        requireValid(central + centralHeaderSize <= directory.end &&
            view.getUint32(central, true) === centralSignature, 'invalid central entry');
        const flags = view.getUint16(central + 8, true);
        const crc = view.getUint32(central + 16, true);
        const size = view.getUint32(central + 20, true);
        const nameLength = view.getUint16(central + 28, true);
        const nameStart = central + centralHeaderSize;
        const centralEnd = nameStart + nameLength + view.getUint16(central + 30, true) +
            view.getUint16(central + 32, true);
        const local = view.getUint32(central + 42, true);
        // Only allow the optional UTF-8 name flag. No encryption or descriptor.
        requireValid((flags & ~0x0800) === 0 && view.getUint16(central + 10, true) === 0 &&
            view.getUint32(central + 24, true) === size && view.getUint16(central + 34, true) === 0 &&
            nameLength > 0 && centralEnd <= directory.end && local + localHeaderSize <= directory.offset,
        'unsupported entry flags, storage method, or size');
        requireValid(view.getUint32(local, true) === localSignature &&
            view.getUint16(local + 6, true) === flags && view.getUint16(local + 8, true) === 0 &&
            view.getUint32(local + 14, true) === crc && view.getUint32(local + 18, true) === size &&
            view.getUint32(local + 22, true) === size && view.getUint16(local + 26, true) === nameLength,
        'local and central entry headers disagree');
        const data = local + localHeaderSize + nameLength + view.getUint16(local + 28, true);
        requireValid(data + size <= directory.offset, 'entry data is outside the archive');
        const name = entryName(bytes, nameStart, nameLength);
        requireValid(!names.has(name) && name === entryName(bytes, local + localHeaderSize, nameLength),
            'duplicate or mismatched entry name');
        names.add(name);
        entries.push({name, central, centralEnd, local, data, size, crc, removed: 0, output: local});
        central = centralEnd;
    }
    requireValid(central === directory.end, 'incorrect directory length');
    const localOrder = entries.slice().sort((a, b) => a.local - b.local);
    let next = 0;
    for (const entry of localOrder) {
        requireValid(entry.local === next, 'overlapping or missing local entries');
        next = entry.data + entry.size;
    }
    requireValid(next === directory.offset, 'incorrect amount of entry data');
    return {entries, localOrder};
}

function illegalXmlBytes(bytes, index, end) {
    const byte = bytes[index];
    if (byte < 0x20 && byte !== 0x09 && byte !== 0x0a && byte !== 0x0d) return 1;
    if (byte === 0xef && index + 2 < end && bytes[index + 1] === 0xbf &&
        (bytes[index + 2] === 0xbe || bytes[index + 2] === 0xbf)) return 3;
    return 0;
}

const sectionTag = '<w:sectPr';

function isSection(bytes, index, end) {
    const after = index + sectionTag.length;
    if (after >= end) return false;
    const delimiter = bytes[after];
    if (delimiter !== 0x3e && delimiter !== 0x2f && delimiter !== 0x20 &&
        delimiter !== 0x09 && delimiter !== 0x0a && delimiter !== 0x0d) return false;
    for (let i = 1; i < sectionTag.length; ++i)
        if (bytes[index + i] !== sectionTag.charCodeAt(i)) return false;
    return true;
}

function inspectXml(bytes, entry, countSections) {
    let sections = 0;
    const end = entry.data + entry.size;
    for (let i = entry.data; i < end;) {
        const invalid = illegalXmlBytes(bytes, i, end);
        entry.removed += invalid;
        if (countSections && bytes[i] === 0x3c && isSection(bytes, i, end)) ++sections;
        i += invalid || 1;
    }
    return sections;
}

const crcTable = new Uint32Array(256);
for (let index = 0; index < crcTable.length; ++index) {
    let crc = index;
    for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    crcTable[index] = crc;
}

function crc32(bytes, begin, end) {
    let crc = 0xffffffff;
    for (let index = begin; index < end; ++index)
        crc = crcTable[(crc ^ bytes[index]) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
}

function compactXml(bytes, entry, target) {
    const end = entry.data + entry.size;
    let segment = entry.data;
    for (let index = segment; index < end;) {
        const invalid = illegalXmlBytes(bytes, index, end);
        if (!invalid) {
            ++index;
            continue;
        }
        bytes.copyWithin(target, segment, index);
        target += index - segment;
        index += invalid;
        segment = index;
    }
    bytes.copyWithin(target, segment, end);
}

function moveLocalEntries(bytes, view, entries) {
    let cursor = 0;
    for (const entry of entries) {
        entry.output = cursor;
        const data = cursor + entry.data - entry.local;
        const end = data + entry.size - entry.removed;
        if (cursor !== entry.local) bytes.copyWithin(cursor, entry.local, entry.data);
        if (entry.removed) {
            compactXml(bytes, entry, data);
            entry.crc = crc32(bytes, data, end);
            view.setUint32(cursor + 14, entry.crc, true);
            view.setUint32(cursor + 18, end - data, true);
            view.setUint32(cursor + 22, end - data, true);
        } else if (data !== entry.data) {
            bytes.copyWithin(data, entry.data, entry.data + entry.size);
        }
        cursor = end;
    }
    return cursor;
}

// Validate first, then reuse the input allocation to compact repaired entries.
// The clean path returns its original array and does not rewrite ZIP metadata.
export function sanitizeDocx(bytes, expectedSections = null) {
    requireValid(bytes instanceof Uint8Array && bytes.length >= endHeaderSize && bytes.length < 0xffffffff,
        'not a ZIP32 byte array');
    requireValid(expectedSections === null || (Number.isInteger(expectedSections) && expectedSections > 0),
        'invalid expected section count');
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const directory = archiveDirectory(view);
    const {entries, localOrder} = readEntries(bytes, view, directory);
    let hasDocument = false;
    let dirty = false;
    for (const entry of entries) {
        const name = entry.name.toLowerCase();
        if (!name.endsWith('.xml') && !name.endsWith('.rels')) continue;
        const document = entry.name === 'word/document.xml';
        const sections = inspectXml(bytes, entry, document && expectedSections !== null);
        if (document) {
            hasDocument = true;
            requireValid(expectedSections === null || sections === expectedSections,
                'expected ' + expectedSections + ' page sections, got ' + sections);
        }
        if (entry.removed) dirty = true;
    }
    requireValid(hasDocument, 'word/document.xml is missing');
    if (!dirty) return bytes;

    const centralOffset = moveLocalEntries(bytes, view, localOrder);
    let cursor = centralOffset;
    for (const entry of entries) {
        bytes.copyWithin(cursor, entry.central, entry.centralEnd);
        view.setUint32(cursor + 42, entry.output, true);
        if (entry.removed) {
            view.setUint32(cursor + 16, entry.crc, true);
            view.setUint32(cursor + 20, entry.size - entry.removed, true);
            view.setUint32(cursor + 24, entry.size - entry.removed, true);
        }
        cursor += entry.centralEnd - entry.central;
    }
    bytes.copyWithin(cursor, directory.end);
    view.setUint32(cursor + 16, centralOffset, true);
    return bytes.subarray(0, cursor + bytes.length - directory.end);
}
