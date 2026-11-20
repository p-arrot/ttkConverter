// The application supplies a private working directory with these ASCII names.
const requestFile = 'opendoc-job.json';
const inputFile = 'input.pdf';
const outputFile = 'output.docx';
const modulePath = import.meta.url.replaceAll('/', '\\');
const engineRoot = (scriptArgs[0] || modulePath.slice(0, modulePath.lastIndexOf('\\'))).replaceAll('/', '\\');

function readEngineAsset(relativePath) {
    // readRelativeToScript drops the drive letter when the job is on another drive.
    return os.file.readFile(engineRoot + '\\' + relativePath.replaceAll('/', '\\'), 'binary');
}

// The SpiderMonkey shell lacks the Browser Encoding API used by wasm-bindgen.
// Its OpenDoc bindings decode complete, strict UTF-8 strings.
if (typeof globalThis.TextDecoder === 'undefined') {
    globalThis.TextDecoder = class {
        decode(bytes = new Uint8Array()) {
            if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
            const fragments = [];
            let units = [];
            for (let position = 0; position < bytes.length;) {
                const first = bytes[position++];
                let point = first;
                if (first >= 128) {
                    const trailing = first >= 240 && first <= 244 ? 3 :
                        first >= 224 && first <= 239 ? 2 : first >= 194 && first <= 223 ? 1 : 0;
                    if (!trailing || position + trailing > bytes.length) throw new TypeError('Invalid UTF-8.');
                    point = first & (127 >> (trailing + 1));
                    for (let index = 0; index < trailing; ++index) {
                        const next = bytes[position++];
                        if ((next & 192) !== 128) throw new TypeError('Invalid UTF-8 continuation.');
                        point = (point << 6) | (next & 63);
                    }
                    const minimum = trailing === 1 ? 128 : trailing === 2 ? 2048 : 65536;
                    if (point < minimum || point > 1114111 || (point >= 55296 && point <= 57343)) {
                        throw new TypeError('Invalid UTF-8 code point.');
                    }
                }
                if (point >= 65536) {
                    point -= 65536;
                    units.push(55296 + (point >> 10), 56320 + (point & 1023));
                } else {
                    units.push(point);
                }
                if (units.length >= 1024) {
                    fragments.push(String.fromCharCode(...units));
                    units = [];
                }
            }
            if (units.length) fragments.push(String.fromCharCode(...units));
            return fragments.join('');
        }
    };
}

if (typeof globalThis.TextEncoder === 'undefined') {
    globalThis.TextEncoder = class {
        encode(value = '') {
            value = String(value);
            const bytes = new Uint8Array(value.length * 3);
            let length = 0;
            for (const character of value) {
                let point = character.codePointAt(0);
                if (point >= 55296 && point <= 57343) point = 65533;
                if (point < 128) {
                    bytes[length++] = point;
                } else if (point < 2048) {
                    bytes[length++] = (point >> 6) | 192;
                    bytes[length++] = (point & 63) | 128;
                } else if (point < 65536) {
                    bytes[length++] = (point >> 12) | 224;
                    bytes[length++] = ((point >> 6) & 63) | 128;
                    bytes[length++] = (point & 63) | 128;
                } else {
                    bytes[length++] = (point >> 18) | 240;
                    bytes[length++] = ((point >> 12) & 63) | 128;
                    bytes[length++] = ((point >> 6) & 63) | 128;
                    bytes[length++] = (point & 63) | 128;
                }
            }
            return bytes.subarray(0, length);
        }
    };
}

globalThis.fetch = () => { throw new Error('OpenDoc conversion only loads local assets.'); };

function provideCharacterMaps(pdf, names, provided) {
    let count = 0;
    for (const name of names) {
        if (provided.has(name) || !/^[A-Za-z0-9_-]+$/.test(name)) continue;
        let bytes;
        try {
            bytes = readEngineAsset('../xpdf-cmaps/CMap/' + name);
        } catch {
            continue;
        }
        pdf.provideCMap(name, bytes);
        provided.add(name);
        ++count;
    }
    return count;
}

function convert(core, pdf, source, preserveLayout) {
    const provided = new Set();
    let reported = 0;
    // Restart with the newly supplied maps when the PDF requests them mid-page.
    for (let attempt = 0; attempt < 3; ++attempt) {
        const job = pdf.beginPdfImport(source);
        try {
            provideCharacterMaps(pdf, job.missingCMaps, provided);
            while (job.status === 'pending') {
                const lastPage = job.pagesDone;
                job.step(1);
                if (job.status === 'pending' && job.pagesDone === lastPage) {
                    throw new Error('PDF page import made no progress.');
                }
                while (reported < job.pagesDone) print('page=' + (++reported) + '/' + job.pageCount);
            }
            const pages = preserveLayout ? job.finish() : job.finishReflowed();
            const document = preserveLayout ? core.openPdfImport(pages, source) : core.openPdfReflow(pages, source);
            try {
                return document.exportDocx();
            } finally {
                document.free();
            }
        } catch (error) {
            if (!provideCharacterMaps(pdf, job.neededCMaps, provided)) throw error;
        } finally {
            job.free();
        }
    }
    throw new Error('Required PDF character maps could not be resolved.');
}

try {
    const {mode} = JSON.parse(read(requestFile).replace(/^\uFEFF/, ''));
    if (mode !== 'fixed' && mode !== 'reflow') throw new Error('Expected fixed or reflow conversion mode.');
    const core = await import('./casual_doc_wasm.mjs');
    const pdf = await import('./casual_doc_wasm_pdf.mjs');
    core.initSync({module: readEngineAsset('casual_doc_wasm_bg.wasm')});
    pdf.initSync({module: readEngineAsset('casual_doc_wasm_pdf_bg.wasm')});
    pdf.setFaceHost({face: (family, bold, italic) => core.bundledFace(family, bold, italic)});
    const result = convert(core, pdf, read(inputFile, 'binary'), mode === 'fixed');
    if (!result.length) throw new Error('OpenDoc produced an empty Word document.');
    os.file.writeTypedArrayToFile(outputFile, result);
} catch (error) {
    printErr('OpenDoc PDF conversion failed: ' + (error?.message || String(error)));
    quit(1);
}
